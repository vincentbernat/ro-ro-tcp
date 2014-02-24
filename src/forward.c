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
	uint16_t buf[2] = { htons(remote->local->event->send_serial), htons(many) };
	ssize_t n;
	while ((n = write(event_get_fd(remote->event->write),
		    ((char *)buf) + (sizeof(uint16_t)*2 - partial), partial)) <= 0) {
		if (errno == EINTR) continue;
		if (n == 0) {
			log_debug("remote", "connection to [%s]:%s was closed",
			    remote->addr, remote->serv);
			local_destroy(remote->local);
			return -1;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}
		log_warn("remote", "unable to send header to [%s]:%s",
		    remote->addr, remote->serv);
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
		    sizeof(uint16_t)*2 - partial)) <= 0) {
		if (errno == EINTR) continue;
		if (n == 0) {
			log_debug("remote",
			    "connection to [%s]:%s was closed",
			    remote->addr, remote->serv);
			local_destroy(remote->local);
			return -1;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}
		log_warn("remote", "unable to read header from [%s]:%s",
		    remote->addr, remote->serv);
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
static int
remote_splice_in(struct ro_remote *remote)
{
	struct ro_local *local = remote->local;
	if (remote->event->partial_bytes != sizeof(uint16_t)*2) {
		/* No header yet */
		ssize_t n = remote_prepare_receiving(remote, remote->event->partial_bytes);
		if (n < 0) return -1;
		if (n == 0) {
			event_add(remote->event->read, NULL);
			return 0;
		}
		remote->event->partial_bytes += n;
		if (remote->event->partial_bytes == sizeof(uint16_t)*2) {
			remote->event->partial_header[0] = ntohs(remote->event->partial_header[0]);
			remote->event->partial_header[1] = ntohs(remote->event->partial_header[1]);
			if (remote->event->partial_header[0] != local->event->receive_serial + 1) {
				/* Not the right remote, stop reading */
				event_del(remote->event->read);
				return 0;
			}
			local->event->receive_serial++;
			remote->event->remaining_bytes = remote->event->partial_header[1];
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
				    "while remote splice in, connection with [%s]:%s closed",
				    remote->addr, remote->serv);
				local_destroy(local);
				return -1;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				event_del(remote->event->read);
				return 0;
			}
			if (errno == ENOSYS || errno == EINVAL) {
				log_warn("remote", "splice not supported, nothing will work");
				local_destroy(local);
				return -1;
			}
			log_warn("remote", "unexpected problem while splicing");
			local_destroy(local);
			return -1;
		}
		remote->stats.in += n;
		remote->event->remaining_bytes -= n;
		max -= n;
		if ((local->event->pipe.nw += n) >= MAX_SPLICE_BYTES) {
			/* Stop reading, the splice pipe is almost full */
			event_del(remote->event->read);
			break;
		}

		/* We put data in the write pipe, let's read it */
		event_add(local->event->write, NULL);
	}

	if (remote->event->remaining_bytes == 0) {
		/* Be ready for next header */
		remote->event->partial_bytes = 0;

		/* Let's enable another remote if possible */
		event_del(remote->event->read);
		struct ro_remote *other;
		TAILQ_FOREACH(other, &local->remotes, next)  {
			if (other->event->partial_bytes == sizeof(uint16_t) * 2 &&
			    other->event->partial_header[0] == local->event->receive_serial + 1) {
				event_add(other->event->read, NULL);
				break;
			}
		}
	}
	return 1;
}

static void
remote_splice_out(struct ro_local *local)
{
	/* We need to select a remote */
	if (local->event->remaining_bytes == 0) {
		/* Select next available remote. */
		int loop = 0;
		while (1) {
			if (local->event->current_send_remote == NULL)
				local->event->current_send_remote = TAILQ_FIRST(&local->remotes);
			else if ((local->event->current_send_remote =
				TAILQ_NEXT(local->event->current_send_remote, next)) == NULL) {
				local->event->current_send_remote = TAILQ_FIRST(&local->remotes);
				loop++;
			}
			if (local->event->current_send_remote == NULL || loop >= 2) {
				/* Should not happen */
				log_warnx("forward", "no remote available?");
				local_destroy(local);
				return;
			}
			if (local->event->current_send_remote->connected) break;
		}
		local->event->partial_bytes = sizeof(uint16_t)*2; /* We need to send the serial + the size */
		local->event->remaining_bytes = local->event->pipe.nr;
		local->event->send_serial++;
	}

	/* Write the header */
	if (local->event->partial_bytes > 0) {
		tcp_cork_set(event_get_fd(local->event->current_send_remote->event->write), 1);
		ssize_t n = remote_prepare_sending(local->event->current_send_remote,
		    local->event->remaining_bytes,
		    local->event->partial_bytes);
		if (n < 0) return;
		if (n == 0) {
			/* Cannot write to remote */
			event_add(local->event->current_send_remote->event->write, NULL);
			return;
		}
		if ((local->event->partial_bytes -= n) > 0) {
			/* Partial write? */
			event_add(local->event->current_send_remote->event->write, NULL);
			return;
		}
	}

	/* Splice data */
	struct ro_remote *remote = local->event->current_send_remote;
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
				    "while remote splice out, connection with [%s]:%s closed",
				    remote->addr, remote->serv);
				local_destroy(local);
				return;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				event_del(remote->event->write);
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
				log_debug("forward",
				    "while local splice in, connection with [%s]:%s closed",
				    local->addr, local->serv);
				local_destroy(local);
				return;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				event_del(local->event->read);
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
				log_debug("forward",
				    "while local splice out, connection with [%s]:%s closed",
				    local->addr, local->serv);
				local_destroy(local);
				return;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				event_del(local->event->write);
				return;
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
		if (local->event->current_receive_remote)
			/* Just wake up this remote */
			event_add(local->event->current_receive_remote->event->read, NULL);
		else {
			/* Wake all remotes */
			struct ro_remote *remote;
			TAILQ_FOREACH(remote, &local->remotes, next)
			    event_add(remote->event->read, NULL);
		}
	}
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

			/* See `incoming_write()` in `connection.c` */
			struct ro_remote *remote;
			TAILQ_FOREACH(remote, &local->remotes, next) {
				if (remote->connected)
					event_add(remote->event->read, NULL);
			}
			log_debug("local", "connected to [%s]:%s (fd: %d)",
			    local->addr, local->serv, fd);
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
				    remote->addr, remote->serv);
				local_destroy(local);
				return;
			}

			event_del(remote->event->write);
			event_add(remote->event->read, NULL);
			event_add(local->event->read, NULL);
			remote->connected = true;
			log_debug("remote", "connected to [%s]:%s (fd: %d)",
			    remote->addr, remote->serv, fd);
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
