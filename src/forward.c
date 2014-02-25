/* -*- mode: c; c-file-style: "openbsd" -*- */
/*
 * Copyright (c) 2013 Vincent Bernat <vbe@deezer.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "ro-ro-tcp.h"
#include "event.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

static void
tcp_cork_set(int fd, bool enable)
{
#ifdef TCP_CORK
	int val = enable;
	if (setsockopt(fd, IPPROTO_TCP, TCP_CORK, &val, sizeof(val)) == -1)
		log_warn("remote", "unable to %s TCP cork on fd %d",
		    enable?"set":"unset", fd);
#endif
}

/**
 * Advertise how many bytes we will send to remote.
 *
 * @param many    How many bytes we would like to send.
 * @param partial How many bytes (of the header) we have to send yet.
 * @return        How many bytes (of the header) we have sent
 */
static ssize_t
remote_prepare_sending(struct ro_remote *remote, size_t many, size_t partial)
{
	/* Our header is quite simple: the serial, the size of the buffer we
	 * want to transmit. */
	char buf[RO_HEADER_SIZE] = {};
	uint16_t serial = htons(remote->local->event->send_serial);
	uint32_t bytes = htonl(many);
	memcpy(buf, &serial, sizeof(serial));
	memcpy(buf + sizeof(serial), &bytes, sizeof(bytes));
	ssize_t n;
	while ((n = write(event_get_fd(remote->event->write),
		    ((char *)buf) + (RO_HEADER_SIZE - partial), partial)) <= 0) {
		if (errno == EINTR) continue;
		if (n == 0) {
			log_debug("remote", "connection [%s]:%s <-> [%s]:%s was closed",
			    remote->laddr, remote->lserv,
			    remote->raddr, remote->rserv);
			local_destroy(remote->local);
			return -1;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}
		log_warn("remote", "unable to send header to [%s]:%s",
		    remote->raddr, remote->rserv);
		local_destroy(remote->local);
		return -1;
	}
	return n;
}

/**
 * Receive the header from the remote nd
 *
 * @param partial How many bytes (of the header) we have to receive yet.
 * @return        How many bytes (of the header) we have received
 */
static ssize_t
remote_prepare_receiving(struct ro_remote *remote, size_t partial)
{
	ssize_t n;
	while ((n = read(event_get_fd(remote->event->read),
		    (char *)remote->event->partial_header + partial,
		    RO_HEADER_SIZE - partial)) <= 0) {
		if (errno == EINTR) continue;
		if (n == 0) {
			log_debug("remote",
			    "connection [%s]:%s <-> [%s]:%s was closed",
			    remote->laddr, remote->lserv,
			    remote->raddr, remote->rserv);
			local_destroy(remote->local);
			return -1;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}
		log_warn("remote", "unable to read header from [%s]:%s",
		    remote->raddr, remote->rserv);
		local_destroy(remote->local);
		return -1;
	}
	return n;
}

/**
 * Splice data from remote end.
 *
 * We need to be sure that this is the right remote to read right now. If it is
 * not, we disable reading on it and we store it in the remote structure.
 *
 */
static void
remote_splice_in(struct ro_remote *remote)
{
	struct ro_local *local = remote->local;
	if (remote->event->partial_bytes != RO_HEADER_SIZE) {
		/* No header yet */
		ssize_t n = remote_prepare_receiving(remote, remote->event->partial_bytes);
		if (n < 0) return;
		if (n == 0) {
			log_debug("forward",
			    "[%s]:%s <-> [%s]:%s: no incoming data available, start reading",
			    remote->laddr, remote->lserv,
			    remote->raddr, remote->rserv);
			event_add(remote->event->read, NULL);
			return;
		}
		remote->event->partial_bytes += n;
		if (remote->event->partial_bytes == RO_HEADER_SIZE) {
			memcpy(&remote->event->receive_serial,
			    remote->event->partial_header,
			    sizeof(remote->event->receive_serial));
			remote->event->receive_serial = ntohs(remote->event->receive_serial);
			memcpy(&remote->event->remaining_bytes,
			    remote->event->partial_header + sizeof(remote->event->receive_serial),
			    sizeof(remote->event->remaining_bytes));
			remote->event->remaining_bytes = ntohl(remote->event->remaining_bytes);
			if (remote->event->receive_serial != local->event->receive_serial + 1) {
				/* Not the right remote, stop reading */
				log_debug("forward",
				    "[%s]:%s <-> [%s]:%s: "
				    "serial is %" PRIu16 " while expecting %" PRIu16"; stop reading",
				    remote->laddr, remote->lserv,
				    remote->raddr, remote->rserv,
				    remote->event->receive_serial,
				    local->event->receive_serial + 1);
				event_del(remote->event->read);
				return;
			}
			local->event->receive_serial++;
			local->event->current_send_remote = remote;
		}
	}

	/* Splice data */
	size_t max = remote->event->remaining_bytes;
	if (max > MAX_SPLICE_BYTES) max = MAX_SPLICE_BYTES;
	while (max > 0) {
		ssize_t n = splice(event_get_fd(remote->event->read),
		    NULL,
		    local->event->pipe.write[1],
		    NULL,
		    max,
		    SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
		if (n <= 0) {
			if (errno == EINTR) continue;
			if (n == 0) {
				log_debug("remote",
				    "while remote splice in, connection [%s]:%s <-> [%s]:%s closed",
				    remote->laddr, remote->lserv,
				    remote->raddr, remote->rserv);
				local_destroy(local);
				return;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				log_debug("forward",
				    "[%s]:%s <-> [%s]:%s: splice in would block, stop reading",
				    remote->laddr, remote->lserv,
				    remote->raddr, remote->rserv);
				event_del(remote->event->read);
				return;
			}
			if (errno == ENOSYS || errno == EINVAL) {
				log_warn("remote", "splice not supported, nothing will work");
				local_destroy(local);
				return;
			}
			log_warn("remote", "unexpected problem while splicing");
			local_destroy(local);
			return;
		}
		remote->stats.in += n;
		remote->event->remaining_bytes -= n;
		max -= n;

		/* We put data in the write pipe, let's read it */
		log_debug("forward",
		    "[%s]:%s <-> [%s]:%s: put data in the write pipe, start writing on local",
		    remote->laddr, remote->lserv,
		    remote->raddr, remote->rserv);
		event_add(local->event->write, NULL);

		if ((local->event->pipe.nw += n) >= MAX_SPLICE_BYTES) {
			/* Stop reading, the splice pipe is almost full */
			log_debug("forward",
			    "[%s]:%s <-> [%s]:%s: spliced more than %d bytes, stop reading",
			    remote->laddr, remote->lserv,
			    remote->raddr, remote->rserv,
			    MAX_SPLICE_BYTES);
			event_del(remote->event->read);
			break;
		}
	}

	if (remote->event->remaining_bytes == 0) {
		/* Be ready for next header */
		remote->event->partial_bytes = 0;

		/* Let's enable another remote if possible */
		log_debug("forward",
		    "[%s]:%s <-> [%s]:%s: read all data from remote, stop reading",
		    remote->laddr, remote->lserv,
		    remote->raddr, remote->rserv);
		event_del(remote->event->read);
		struct ro_remote *other;
		TAILQ_FOREACH(other, &local->remotes, next)  {
			if (other->event->partial_bytes == RO_HEADER_SIZE &&
			    other->event->receive_serial == local->event->receive_serial + 1) {
				log_debug("forward",
				    "[%s]:%s <-> [%s]:%s: next remote, start reading",
				    other->laddr, other->lserv,
				    other->raddr, other->laddr);
				event_add(other->event->read, NULL);
				break;
			}
		}
	}
	return;
}

static void
remote_splice_out(struct ro_local *local)
{
	/* We need to select a remote */
	struct ro_remote *remote = local->event->current_send_remote;
	if (local->event->remaining_bytes == 0) {
		/* Select next available remote. */
		int loop = 0;
		while (1) {
			if (remote == NULL)
				remote = TAILQ_FIRST(&local->remotes);
			else if ((remote =
				TAILQ_NEXT(remote, next)) == NULL) {
				remote = TAILQ_FIRST(&local->remotes);
				loop++;
			}
			if (remote == NULL || loop >= 2) {
				/* Should not happen */
				log_warnx("forward", "no remote available?");
				local_destroy(local);
				return;
			}
			if (remote->connected) break;
		}
		log_debug("forward",
		    "[%s]:%s <-> [%s]:%s: selected as next remote for %zu bytes (serial %"PRIu16,
		    remote->laddr, remote->lserv,
		    remote->raddr, remote->rserv,
		    local->event->pipe.nr,
		    local->event->send_serial+1);
		local->event->partial_bytes = RO_HEADER_SIZE;
		local->event->remaining_bytes = local->event->pipe.nr;
		local->event->send_serial++;
		local->event->current_send_remote = remote;
	}

	/* Write the header */
	if (local->event->partial_bytes > 0) {
		tcp_cork_set(event_get_fd(remote->event->write), 1);
		ssize_t n = remote_prepare_sending(remote,
		    local->event->remaining_bytes,
		    local->event->partial_bytes);
		if (n < 0) return;
		if (n == 0) {
			/* Cannot write to remote */
			log_debug("forward",
			    "[%s]:%s <-> [%s]:%s: currently cannot send header to remote, start writing",
			    remote->laddr, remote->lserv,
			    remote->raddr, remote->rserv);
			event_add(remote->event->write, NULL);
			return;
		}
		if ((local->event->partial_bytes -= n) > 0) {
			/* Partial write? */
			log_debug("forward",
			    "[%s]:%s <-> [%s]:%s: partial header sent, start writing",
			    remote->laddr, remote->lserv,
			    remote->raddr, remote->rserv);
			event_add(remote->event->write, NULL);
			return;
		}
	}

	/* Splice data */
	while (local->event->remaining_bytes > 0) {
		ssize_t n = splice(local->event->pipe.read[0],
		    NULL,
		    event_get_fd(remote->event->write),
		    NULL,
		    local->event->remaining_bytes,
		    SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
		if (n <= 0) {
			if (errno == EINTR) continue;
			if (n == 0) {
				log_debug("remote",
				    "while remote splice out, connection [%s]:%s <-> [%s]:%s closed",
				    remote->laddr, remote->lserv,
				    remote->raddr, remote->rserv);
				local_destroy(local);
				return;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				log_debug("forward",
				    "[%s]:%s <-> [%s]:%s: currently cannot splice data to remote (%"PRIu32" remaining), "
				    "start writing",
				    remote->laddr, remote->lserv,
				    remote->raddr, remote->rserv,
				    local->event->remaining_bytes);
				event_add(remote->event->write, NULL);
				return;
			}
			if (errno == ENOSYS || errno == EINVAL) {
				log_warn("remote", "splice not supported, nothing will work");
				local_destroy(local);
				return;
			}
			log_warn("remote", "unexpected problem while splicing");
			local_destroy(local);
			return;
		}
		remote->stats.out += n;
		local->event->remaining_bytes -= n;
		local->event->pipe.nr -= n;
		/* We can push more data to read pipe */
		log_debug("forward",
		    "[%s]:%s <-> [%s]:%s: data has been sent to remote, start reading on local",
		    remote->laddr, remote->lserv,
		    remote->raddr, remote->rserv);
		event_add(local->event->read, NULL);
	}
	tcp_cork_set(event_get_fd(remote->event->write), 0);
}


static void
local_splice_in(struct ro_local *local)
{
	while (1) {
		ssize_t n = splice(event_get_fd(local->event->read), NULL,
		    local->event->pipe.read[1], NULL,
		    MAX_SPLICE_AT_ONCE,
		    SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
		if (n <= 0) {
			if (errno == EINTR) continue;
			if (n == 0) {
				log_debug("local",
				    "while local splice in, connection with [%s]:%s closed",
				    local->addr, local->serv);
				local_destroy(local);
				return;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (local->event->pipe.nr > 0) {
					log_debug("forward",
					    "[%s]:%s: cannot splice more data from local, stop reading",
					    local->addr, local->serv);
					event_del(local->event->read);
				} else {
					log_debug("forward",
					    "[%s]:%s: cannot splice more data from local, wait for read read",
					    local->addr, local->serv);
					event_add(local->event->read, NULL); /* useless, but for consistency */
				}
				break;
			}
			if (errno == ENOSYS || errno == EINVAL) {
				log_warn("forward", "splice not supported, nothing will work");
				local_destroy(local);
				return;
			}
			log_warn("forward", "unknown problem while splicing");
			local_destroy(local);
			return;
		}
		local->stats.out += n;
		if ((local->event->pipe.nr += n) >= MAX_SPLICE_BYTES) {
			/* Stop feeding, the splice pipe is almost full */
			log_debug("forward",
			    "[%s]:%s: already spliced more than %d bytes, stop reading",
			    local->addr, local->serv,
			    MAX_SPLICE_BYTES);
			event_del(local->event->read);
			break;
		}
	}
	/* We should enable remote, but maybe we don't have one yet. */
	remote_splice_out(local);
}

static void
local_splice_out(struct ro_local *local)
{
	while (local->event->pipe.nw > 0) {
		ssize_t n = splice(local->event->pipe.write[0], NULL,
		    event_get_fd(local->event->write), NULL,
		    local->event->pipe.nw,
		    SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
		if (n <= 0) {
			if (errno == EINTR) continue;
			if (n == 0) {
				log_debug("local",
				    "while local splice out, connection with [%s]:%s closed",
				    local->addr, local->serv);
				local_destroy(local);
				return;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}
			if (errno == ENOSYS || errno == EINVAL) {
				log_warn("forward", "splice not supported, nothing will work");
				local_destroy(local);
				return;
			}
			log_warn("forward", "unknown problem while splicing");
			local_destroy(local);
			return;
		}

		local->stats.in += n;
		local->event->pipe.nw -= n;
		/* We can push more data to write pipe. */
		struct ro_remote *remote = local->event->current_receive_remote;
		if (remote) {
			/* Just wake up this remote */
			log_debug("forward",
			    "[%s]:%s: can receive more data, waking up [%s]:%s <-> [%s]:%s for read",
			    local->addr, local->serv,
			    remote->laddr, remote->lserv,
			    remote->raddr, remote->rserv);
			event_add(remote->event->read, NULL);
		} else {
			/* Wake all remotes */
			log_debug("forward",
			    "[%s]:%s: can receive more data, waking up all remotes for read",
			    local->addr, local->serv);
			TAILQ_FOREACH(remote, &local->remotes, next) {
				if (remote->connected) {
					event_add(remote->event->read, NULL);
				} else {
					log_debug("forward",
					    "[%s]:%s <-> [%s]:%s: not waking up, not connected yet",
					    remote->laddr, remote->lserv,
					    remote->raddr, remote->rserv);
				}
			}
		}
	}
	log_debug("forward",
	    "[%s]:%s: emptied the write pipe, stop writing",
	    local->addr, local->serv);
	event_del(local->event->write);
}

void
local_data_cb(evutil_socket_t fd, short what, void *arg)
{
	struct ro_local *local = arg;
	if (!local->connected) {
		if (what == EV_WRITE) {
			/* Are we connected? */
			socklen_t len = sizeof(errno);
			getsockopt(fd, SOL_SOCKET, SO_ERROR, &errno, &len);
			if (errno == EINTR) return;
			if (errno == EINPROGRESS) return; /* ??? */
			if (errno != 0) {
				log_warn("local", "unable to connect to [%s]:%s",
				    local->addr, local->serv);
				local_destroy(local);
				return;
			}

			event_del(local->event->write);
			event_add(local->event->read, NULL);
			local->connected = true;
			log_debug("local", "connected to [%s]:%s (fd: %d)",
			    local->addr, local->serv, fd);

			/* See `incoming_write()` in `connection.c` */
			struct ro_remote *remote;
			TAILQ_FOREACH(remote, &local->remotes, next) {
				if (remote->connected) {
					log_debug("forward",
					    "[%s]:%s <-> [%s]:%s: enabling read",
					    remote->laddr, remote->lserv,
					    remote->raddr, remote->rserv);
					event_add(remote->event->read, NULL);
				}
			}
			return;
		}
		goto end;
	}
	switch (what) {
	case EV_READ:
		/* Incoming data available. Let's splice. */
		local_splice_in(local);
		return;
	case EV_WRITE:
		local_splice_out(local);
		return;
	}
end:
	log_warnx("forward", "unable to handle local event %d on fd %d",
	    what, fd);
}

void
remote_data_cb(evutil_socket_t fd, short what, void *arg)
{
	struct ro_remote *remote = arg;
	struct ro_local *local = remote->local;
	if (!remote->connected) {
		if (what == EV_WRITE) {
			/* Are we connected? */
			socklen_t len = sizeof(errno);
			getsockopt(fd, SOL_SOCKET, SO_ERROR, &errno, &len);
			if (errno == EINTR) return;
			if (errno == EINPROGRESS) return; /* ??? */
			if (errno != 0) {
				log_warn("remote", "unable to connect to [%s]:%s",
				    remote->raddr, remote->rserv);
				local_destroy(local);
				return;
			}

			event_del(remote->event->write);
			event_add(remote->event->read, NULL);
			event_add(local->event->read, NULL);
			remote->connected = true;
			log_debug("remote", "connected [%s]:%s <-> [%s]:%s (fd: %d)",
			    remote->laddr, remote->lserv,
			    remote->raddr, remote->rserv,
			    fd);
			connection_established(local, remote);
			return;
		}
		goto end;
	}
	switch (what) {
	case EV_READ:
		remote_splice_in(remote);
		return;
	case EV_WRITE:
		remote_splice_out(remote->local);
		return;
	}
end:
	log_warnx("remote", "unable to handle event %d on fd %d",
	    what, fd);
}
