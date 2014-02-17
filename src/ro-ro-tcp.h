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

#ifndef _BOOTSTRAP_H
#define _BOOTSTRAP_H

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#include "log.h"

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <argtable2.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <netdb.h>
#include <event2/event.h>

#define SERVSTRLEN 6

#define RO_LISTEN_QUEUE 20
#define RO_CONNECTION_NUMBER 4

struct ro_cfg;
struct ro_local;
struct ro_remote;

/* arg.c */
struct arg_addr {
	struct arg_hdr hdr;
	int count;
	char sep;
	struct addrinfo *info;
};
struct arg_addr *arg_addr1(const char *, const char *,
    const char *, const char *, char);

/* event.c */
struct event_private;
struct event_remote_private;
int  event_configure(struct ro_cfg *);
int  event_loop(struct ro_cfg *);
void event_shutdown(struct ro_cfg *);

/* endpoint.c */
struct ro_local *local_init(struct ro_cfg *, int,
    char[static INET6_ADDRSTRLEN], char[static SERVSTRLEN]);
struct ro_remote *remote_init(struct ro_cfg *, struct ro_local *, int,
    char[static INET6_ADDRSTRLEN], char[static SERVSTRLEN]);
void remote_destroy(struct ro_remote *);
void local_destroy(struct ro_local *);
int  endpoint_connect(struct addrinfo *,
    char[static INET6_ADDRSTRLEN], char[static SERVSTRLEN]);

/* connection.c */
struct local_private;
struct remote_private;
int connection_listen(struct ro_cfg *cfg);

/* forward.c */
void remote_data_cb(evutil_socket_t, short, void *);
void pipe_write_data_cb(evutil_socket_t, short, void *);
void pipe_read_data_cb(evutil_socket_t, short, void *);
void local_data_cb(evutil_socket_t, short, void *);

/* General */
enum ro_role {
	ROLE_PROXY=1,
	ROLE_RELAY
};

/**
 * Describe one remote.
 */
struct ro_remote {
	TAILQ_ENTRY(ro_remote) next;

	struct ro_cfg *cfg;
	struct ro_local *local;
	bool connected;

	/* To display messages about this remote */
	char addr[INET6_ADDRSTRLEN];
	char serv[SERVSTRLEN];

	struct {
		size_t in;	/* input bytes */
		size_t out;	/* output bytes */
	} stats;

	struct remote_private *event;
};

/**
 * Describe one local endpoint.
 */
struct ro_local {
	TAILQ_ENTRY(ro_local) next;

	struct ro_cfg *cfg;
	bool ready;

	/* To display messages about this local endpoint */
	char addr[INET6_ADDRSTRLEN];
	char serv[SERVSTRLEN];

	uint32_t group_id;	/* Group ID */

	struct {
		size_t in;	/* input bytes */
		size_t out;	/* output bytes */
	} stats;

	/* Where data should be forwarded to */
	TAILQ_HEAD(, ro_remote) remotes;

	struct local_private *event;
};

struct ro_cfg {
	enum ro_role role;	 /* role */
	struct addrinfo *local;	 /* bind to */
	struct addrinfo *remote; /* connect to */
	int backlog;		 /* listen queue for local socket */
	int conns;		 /* number of connections to open to remote */

	uint32_t last_group_id;	/* Last group we provided */

	/* List of local endpoints */
	TAILQ_HEAD(, ro_local) locals;

	struct event_private *event; /* private data for libevent */
};

#endif
