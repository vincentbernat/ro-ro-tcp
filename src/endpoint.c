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

#include <inttypes.h>
#include <errno.h>
#include <string.h>

/**
 * Dump information about a remote.
 */
void
remote_debug(struct ro_remote *remote)
{
	log_info("endpoint",
	    "remote [%s]:%s:\n"
	    "  connected: %s\n"
	    "  in:        %-10zu bytes   out: %-10zu bytes\n"
	    "  read:      %-10s       write: %-10s\n"
	    "  header: %zu (out of %zu)\n"
	    "  serial: %"PRIu32"\n"
	    "  to receive: %"PRIu32" bytes\n",
	    remote->addr, remote->serv,
	    remote->connected?"yes":"no",
	    remote->stats.in, remote->stats.out,
	    event_pending(remote->event->read, EV_READ, NULL)?"wait":"no",
	    event_pending(remote->event->write, EV_WRITE, NULL)?"wait":"no",
	    remote->event->partial_bytes, 2*sizeof(uint16_t),
	    (remote->event->partial_bytes == 2*sizeof(uint16_t))?remote->event->partial_header[0]:0,
	    remote->event->remaining_bytes);
}

/**
 * Dump information about a local endpoint.
 */
void
local_debug(struct ro_local *local)
{
	log_info("endpoint",
	    "local [%s]:%s:\n"
	    "  connected: %s\n"
	    "  in:        %-10zu bytes   out: %-10zu bytes\n"
	    "\n"
	    "  socket:     read:  %-7s    write: %-7s\n"
	    "  read pipe:  bytes: %-10zu\n"
	    "  write pipe: bytes: %-10zu\n"
	    "\n"
	    "  remote: sending to [%s]%s%s, receiving from [%s]%s%s\n"
	    "  serial: sending %"PRIu32", receiving %"PRIu32"\n"
	    "  to receive: %"PRIu32" bytes (+ %zu bytes of header)\n",
	    local->addr, local->serv,
	    local->connected?"yes":"no",
	    local->stats.in, local->stats.out,
	    event_pending(local->event->read, EV_READ, NULL)?"wait":"no",
	    event_pending(local->event->write, EV_WRITE, NULL)?"wait":"no",
	    local->event->pipe.nr,
	    local->event->pipe.nw,
	    local->event->current_send_remote?local->event->current_send_remote->addr:"none",
	    local->event->current_send_remote?":":"",
	    local->event->current_send_remote?local->event->current_send_remote->serv:"",
	    local->event->current_receive_remote?local->event->current_receive_remote->addr:"none",
	    local->event->current_receive_remote?":":"",
	    local->event->current_receive_remote?local->event->current_receive_remote->serv:"",
	    local->event->send_serial, local->event->receive_serial,
	    local->event->remaining_bytes,
	    2*sizeof(uint16_t) - local->event->partial_bytes);

	struct ro_remote *remote;
	TAILQ_FOREACH(remote, &local->remotes, next)
	    remote_debug(remote);
}

/**
 * Destroy a remote endpoint
 */
void
remote_destroy(struct ro_remote *remote)
{
	if (!remote) return;
	struct ro_local *local = remote->local;
	log_debug("endpoint", "destroy remote [%s]:%s",
	    remote->addr, remote->serv);

	if (remote->event) {
		event_close_and_free(remote->event->read);
		event_close_and_free(remote->event->write);
		free(remote->event);
	}

	if (remote->next.tqe_prev != NULL &&
	    remote->next.tqe_next != NULL)
		TAILQ_REMOVE(&local->remotes, remote, next);
	free(remote);
}

/**
 * Destroy a local endpoint
 */
void
local_destroy(struct ro_local *local)
{
	if (!local) return;
	struct ro_cfg *cfg = local->cfg;
	log_debug("endpoint", "destroy local [%s]:%s",
	    local->addr, local->serv);

	/* Close all remotes */
	struct ro_remote *re, *re_next;
	for (re = TAILQ_FIRST(&local->remotes);
	     re != NULL;
	     re = re_next) {
		re_next = TAILQ_NEXT(re, next);
		remote_destroy(re); /* Will do TAILQ_REMOVE */
	}

	if (local->event) {
		if (local->event->pipe.read[0] != -1) close(local->event->pipe.read[0]);
		if (local->event->pipe.read[1] != -1) close(local->event->pipe.read[1]);
		if (local->event->pipe.write[0] != -1) close(local->event->pipe.write[0]);
		if (local->event->pipe.write[1] != -1) close(local->event->pipe.write[1]);
		event_close_and_free(local->event->read);
		event_close_and_free(local->event->write);
		free(local->event);
	}

	if (local->next.tqe_prev != NULL &&
	    local->next.tqe_prev != NULL)
		TAILQ_REMOVE(&cfg->locals, local, next);
	free(local);
}

int
endpoint_connect(struct addrinfo *rem,
    char addr[static INET6_ADDRSTRLEN], char serv[static SERVSTRLEN])
{
	int sfd = -1;
	int err;
	struct addrinfo *re;
	for (re = rem; re != NULL; re = re->ai_next) {
		getnameinfo(re->ai_addr, re->ai_addrlen,
		    addr, INET6_ADDRSTRLEN,
		    serv, SERVSTRLEN,
		    NI_NUMERICHOST | NI_NUMERICSERV); /* cannot fail */
		log_debug("endpoint", "try to connect to [%s]:%s", addr, serv);
		if ((sfd = socket(re->ai_family, re->ai_socktype, re->ai_protocol)) == -1)
			continue;
		evutil_make_socket_nonblocking(sfd);
		while ((err = 0, connect(sfd, re->ai_addr, re->ai_addrlen)) == -1) {
			if (errno == EINTR) continue;
			if (errno == EINPROGRESS) break; /* async connect */
			err = errno;
			close(sfd); sfd = -1;
			break;
		}
		if (!err) break;
		errno = err;
	}
	if (re == NULL) {
		log_warn("endpoint", "unable to connect to [%s]:%s", addr, serv);
		if (sfd != -1) close(sfd);
		return -1;
	}
	return sfd;
}

struct ro_remote *
remote_init(struct ro_cfg *cfg, struct ro_local *local, int sfd,
    char addr[static INET6_ADDRSTRLEN], char serv[static SERVSTRLEN])
{
	struct ro_remote *remote = calloc(1, sizeof(struct ro_remote));
	if (remote == NULL) {
		log_warn("remote", "unable to allocate memory for new remote");
		goto error;
	}

	int sfd2 = -1;

	remote->cfg = cfg;
	remote->local = local;
	memcpy(remote->addr, addr, sizeof(remote->addr));
	memcpy(remote->serv, serv, sizeof(remote->serv));

	sfd2 = dup(sfd);
	log_debug("remote", "new remote setup (socket=%d/%d)",
	    sfd, sfd2);

	if (sfd2 == -1 ||
	    (remote->event = calloc(1, sizeof(struct remote_private))) == NULL ||
	    (remote->event->read = event_new(cfg->event->base, sfd,
		EV_READ|EV_PERSIST,
		remote_data_cb,
		remote)) == NULL ||
	    ((sfd = -1, remote->event->write = event_new(cfg->event->base, sfd2,
		    EV_WRITE|EV_PERSIST,
		    remote_data_cb,
		    remote))) == NULL ||
	    ((sfd2 = -1, 0))) {
		log_warnx("remote", "unable to allocate events for new remote");
		goto error;
	}
	return remote;

error:
	if (sfd != -1) close(sfd);
	if (sfd2 != -1) close(sfd2);
	remote_destroy(remote);
	return NULL;
}

/**
 * Initialize a local endpoint.
 *
 * @param fd File descriptor to the local peer. We take care of closing this
 *           file descriptor if needed, even in case of error.
 */
struct ro_local *
local_init(struct ro_cfg *cfg, int fd,
    char addr[static INET6_ADDRSTRLEN], char serv[static SERVSTRLEN])
{
	int pipe_read[2] = { -1, -1 };
	int pipe_write[2] = { -1, -1 };
	int fd2 = -1;

	struct ro_local *local = calloc(1, sizeof(struct ro_local));
	if (local == NULL) {
		log_warn("local", "unable to allocate memory for new local endpoint [%s]:%s",
		    addr, serv);
		goto error;
	}
	TAILQ_INIT(&local->remotes);
	local->cfg = cfg;
	memcpy(local->addr, addr, INET6_ADDRSTRLEN);
	memcpy(local->serv, serv, SERVSTRLEN);

	if (pipe(pipe_read) == -1 ||
	    pipe(pipe_write) == -1 ||
	    (fd2 = dup(fd)) == -1) {
		log_warn("local", "unable to setup additional file descriptors");
		goto error;
	}
	evutil_make_socket_nonblocking(pipe_read[0]);
	evutil_make_socket_nonblocking(pipe_read[1]);
	evutil_make_socket_nonblocking(pipe_write[0]);
	evutil_make_socket_nonblocking(pipe_write[1]);

	log_debug("local", "new local endpoint setup (socket=%d/%d, pipe_read=(%d,%d), pipe_write=(%d, %d))",
	    fd, fd2, pipe_read[0], pipe_read[1], pipe_write[0], pipe_write[1]);

	if ((local->event = calloc(1, sizeof(struct local_private))) == NULL ||
	    (local->event->pipe.read[0] = local->event->pipe.read[1] =
		local->event->pipe.write[0] = local->event->pipe.write[1] = -1, 0) ||
	    (local->event->read = event_new(cfg->event->base, fd,
		EV_READ|EV_PERSIST,
		local_data_cb,
		local)) == NULL ||
	    ((fd = -1, local->event->write = event_new(cfg->event->base, fd2,
		    EV_WRITE|EV_PERSIST,
		    local_data_cb,
		    local))) == NULL ||
	    ((fd2 = -1, 0))) {
		log_warnx("local", "unable to allocate events for new local endpoint [%s]:%s",
		    addr, serv);
		goto error;
	}

	local->event->pipe.read[0] = pipe_read[0];
	local->event->pipe.read[1] = pipe_read[1];
	local->event->pipe.write[0] = pipe_write[0];
	local->event->pipe.write[1] = pipe_write[1];

	return local;

error:
	if (fd != -1) close(fd);
	if (fd2 != -1) close(fd2);
	if (pipe_read[0] != -1) close(pipe_read[0]);
	if (pipe_read[1] != -1) close(pipe_write[1]);
	if (pipe_write[0] != -1) close(pipe_read[0]);
	if (pipe_write[1] != -1) close(pipe_write[1]);
	local_destroy(local);
	return NULL;
}
