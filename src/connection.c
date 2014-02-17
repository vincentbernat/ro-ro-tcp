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

#include <string.h>
#include <inttypes.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>

struct incoming_connection {
	struct ro_cfg *cfg;
	int fd;
	unsigned id;
	struct bufferevent *bev;
	char addr[INET6_ADDRSTRLEN];
	char serv[SERVSTRLEN];
};

/**
 * Destroy an incoming connection.
 */
static void
incoming_destroy(struct incoming_connection *connection, bool cl)
{
	if (connection) {
		if (connection->bev) bufferevent_free(connection->bev);
		if (cl && connection->fd != -1) close(connection->fd);
	}
	free(connection);
}

static void
incoming_read(struct bufferevent *bev, void *arg)
{
	struct incoming_connection *incoming = arg;
	uint32_t id;
	size_t len;
	if ((len = evbuffer_remove(bufferevent_get_input(bev), &id, sizeof(id))) != sizeof(id)) {
		log_warnx("connection",
		    "incorrect length for group ID received: %zu != %zu",
		    len, sizeof(uint32_t));
		incoming_destroy(incoming, true);
		return;
	}
	id = ntohl(id);
	if (id == 0) {
		log_debug("connection",
		    "incoming connection from [%s]:%s will be attached to group ID #%" PRIu32,
		    incoming->addr, incoming->serv, id);
	}
	while (id == 0) {
		id = ++incoming->cfg->last_group_id;
		/* Check it is not already used. */
		struct ro_local *local;
		TAILQ_FOREACH(local, &incoming->cfg->locals, next) {
			if (local->group_id == id) {
				id = 0;
				break;
			}
		}
		log_debug("connection",
		    "incoming connection from [%s]:%s will use group ID #%" PRIu32,
		    incoming->addr, incoming->serv, id);
	}
	incoming->id = id;
	id = htonl(id);
	if (bufferevent_write(bev,
		&id, sizeof(id)) == -1) {
		log_warnx("connection",
		    "unable to push group ID to remote");
		incoming_destroy(incoming, true);
		return;
	}
}

static void
incoming_write(struct bufferevent *bev, void *arg)
{
	struct incoming_connection *incoming = arg;
	struct ro_cfg *cfg = incoming->cfg;

	/* OK, now, we should find or create the appropriate local connection */
	struct ro_local *local;
	int sfd = -1;
	TAILQ_FOREACH(local, &cfg->locals, next)
	    if (local->group_id == incoming->id) break;
	if (local == NULL) {
		char addr[INET6_ADDRSTRLEN] = {};
		char serv[SERVSTRLEN] = {};
		if ((sfd = endpoint_connect(cfg->local, addr, serv)) == -1 ||
		    (local = local_init(cfg, sfd, addr, serv)) == NULL) {
			incoming_destroy(incoming, true);
			return;
		}
		local->group_id = incoming->id;
		TAILQ_INSERT_TAIL(&cfg->locals, local, next);
	}

	/* And attach a new remote on it. */
	struct ro_remote *remote = NULL;
	if ((remote = remote_init(cfg, local, incoming->fd,
		    incoming->addr, incoming->serv)) == NULL) {
		incoming_destroy(incoming, false);
		local_destroy(local);
		return;
	}
	TAILQ_INSERT_TAIL(&local->remotes, remote, next);
}

static void
incoming_event(struct bufferevent *bev, short what, void *arg)
{
	struct incoming_connection *incoming = arg;
	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR|BEV_EVENT_TIMEOUT)) {
		log_info("connection",
		    "incoming connection with [%s]:%s aborted before completion",
		    incoming->addr, incoming->serv);
		incoming_destroy(incoming, true);
		return;
	}
}

/**
 * Called when a new client connects.
 */
static void
client_accept_cb(struct evconnlistener *listener,
    evutil_socket_t fd, struct sockaddr *address, int socklen,
    void *arg)
{
	struct ro_cfg *cfg = arg;
	char addr[INET6_ADDRSTRLEN] = {};
	char serv[SERVSTRLEN] = {};
	getnameinfo(address, socklen,
	    addr, sizeof(addr),
	    serv, sizeof(serv),
	    NI_NUMERICHOST | NI_NUMERICSERV);
	log_info("connection", "accepting connection from [%s]:%s", addr, serv);

	struct ro_remote *remote = NULL;
	struct ro_local  *local  = NULL;
	struct incoming_connection *incoming = NULL;

	switch (cfg->role) {
	case ROLE_PROXY:
		/* We setup this new local endpoint */
		local = local_init(cfg, fd, addr, serv);
		fd = -1;
		if (local == NULL) goto error;
		TAILQ_INSERT_TAIL(&cfg->locals, local, next);

		/* We open the first connection to remote */
		char raddr[INET6_ADDRSTRLEN] = {};
		char rserv[SERVSTRLEN] = {};
		int sfd;
		if ((sfd = endpoint_connect(cfg->remote, raddr, rserv)) == -1 ||
		    (remote = remote_init(cfg, local, sfd, raddr, rserv)) == NULL)
			goto error;
		TAILQ_INSERT_TAIL(&local->remotes, remote, next);
		return;

	case ROLE_RELAY:
		/* We don't create anything yet. We need to find the appropriate
		 * local endpoint. For that purpose, we use a simple protocol
		 * (which also gives the possibility to hijack a connection):
		 * the proxy will send "0" to say it is the first connection of
		 * a group of connections. We send back a group number. Other
		 * connections from the same group should send this group number
		 * when establishing a connection. In this case, the relay will
		 * echo back this group number or 0 if the group is not known.
		 */
		if ((incoming = calloc(1, sizeof(struct incoming_connection))) == NULL) {
			log_warn("connection",
			    "unable to allocate memory for incoming connection");
			goto error;
		}
		incoming->cfg = cfg;
		incoming->fd = fd;
		strncpy(incoming->addr, addr, sizeof(addr));
		strncpy(incoming->serv, serv, sizeof(serv));
		if ((incoming->bev = bufferevent_socket_new(cfg->event->base,
			    incoming->fd,
			    0)) == NULL) {
			log_warnx("connection",
			    "unable to create buffer event for incoming connection");
			goto error;
		}
		bufferevent_setcb(incoming->bev, incoming_read, incoming_write,
		    incoming_event, incoming);
		bufferevent_setwatermark(incoming->bev, EV_READ,
		    sizeof(uint32_t), sizeof(uint32_t));
		bufferevent_enable(incoming->bev, EV_READ|EV_WRITE);
		return;
	}
error:
	incoming_destroy(incoming, false);
	local_destroy(local);
	if (fd != -1) close(fd);
}

/**
 * Called when we cannot accept a client
 */
static void
client_accept_error_cb(struct evconnlistener *listener,
    void *arg)
{
        log_warnx("connection", "got an error when accepting a request");
	/* Really nothing else to do */
}

/**
 * Listen for new connections
 */
int
connection_listen(struct ro_cfg *cfg)
{
	struct addrinfo *listenaddr =
	    (cfg->role == ROLE_PROXY)?cfg->local:cfg->remote, *la;
	char addr[INET6_ADDRSTRLEN] = {};
	char serv[SERVSTRLEN] = {};

	for (la = listenaddr; la != NULL; la = la->ai_next) {
		getnameinfo(la->ai_addr, la->ai_addrlen,
		    addr, sizeof(addr),
		    serv, sizeof(serv),
		    NI_NUMERICHOST | NI_NUMERICSERV); /* cannot fail */
		log_debug("connection", "try to bind and listen to [%s]:%s", addr, serv);

		cfg->event->listener = evconnlistener_new_bind(cfg->event->base,
		    client_accept_cb, cfg,
		    LEV_OPT_CLOSE_ON_FREE |
		    LEV_OPT_CLOSE_ON_EXEC |
		    LEV_OPT_REUSEABLE,
		    cfg->backlog,
		    la->ai_addr, la->ai_addrlen);
		if (cfg->event->listener) break;
	}
	if (la == NULL) {
		log_warn("connection", "unable to bind to [%s]:%s", addr, serv);
		return -1;
	}
	evconnlistener_set_error_cb(cfg->event->listener, client_accept_error_cb);
	log_info("connection", "listening to [%s]:%s", addr, serv);
	return 0;
}




