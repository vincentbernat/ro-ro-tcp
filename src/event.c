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

#include <signal.h>
#include <errno.h>
#include <arpa/inet.h>
#include <event2/listener.h>

static void
levent_log_cb(int severity, const char *msg)
{
	switch (severity) {
	case _EVENT_LOG_DEBUG: log_debug("libevent", "%s", msg); break;
	case _EVENT_LOG_MSG:   log_info ("libevent", "%s", msg);  break;
	case _EVENT_LOG_WARN:  log_warnx("libevent", "%s", msg);  break;
	case _EVENT_LOG_ERR:   log_warnx("libevent", "%s", msg); break;
	}
}

static void
levent_dump(evutil_socket_t fd, short what, void *arg)
{
	struct ro_cfg *cfg = arg;
	struct ro_local *local;
	TAILQ_FOREACH(local, &cfg->locals, next)
	    local_debug(local);
}

static void
levent_stop(evutil_socket_t fd, short what, void *arg)
{
        struct event_base *base = arg;
        (void)fd; (void)what;
        event_base_loopbreak(base);
}

/**
 * Configure libevent.
 */
int
event_configure(struct ro_cfg *cfg)
{
	log_debug("event", "configure libevent");
	event_set_log_callback(levent_log_cb);

	if ((cfg->event = calloc(1, sizeof(struct event_private))) == NULL) {
		log_warn("event", "unable to allocate private data for events");
		return -1;
	}
	if (!(cfg->event->base = event_base_new())) {
		log_warnx("event", "unable to initialize libevent");
		return -1;
	}

	log_info("event", "libevent %s initialized with %s method",
	    event_get_version(),
	    event_base_get_method(cfg->event->base));

	/* Signals */
	log_debug("event", "register signals");
        signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	evsignal_add(cfg->event->signals.sigint = evsignal_new(cfg->event->base,
		SIGINT, levent_stop, cfg->event->base),
	    NULL);
	evsignal_add(cfg->event->signals.sigterm = evsignal_new(cfg->event->base,
		SIGTERM, levent_stop, cfg->event->base),
	    NULL);
	evsignal_add(cfg->event->signals.sigusr1 = evsignal_new(cfg->event->base,
		SIGUSR1, levent_dump, cfg),
	    NULL);

	return connection_listen(cfg);
}

int
event_loop(struct ro_cfg *cfg)
{
	if (event_reinit(cfg->event->base)) {
		log_warnx("event", "unable to reinit event loop");
		return -1;
	}
	log_info("event", "start main event loop");
	if (event_base_loop(cfg->event->base, 0) == -1) {
		log_warnx("event", "unable to run libevent loop");
		return -1;
	}
	log_info("event", "end of main loop");
	return 0;
}

/**
 * Shutdown libevent.
 */
void
event_shutdown(struct ro_cfg *cfg)
{
	if (cfg->event) {
		if (cfg->event->listener)
			evconnlistener_free(cfg->event->listener);
		if (cfg->event->signals.sigint)
			event_free(cfg->event->signals.sigint);
		if (cfg->event->signals.sigterm)
			event_free(cfg->event->signals.sigterm);
		if (cfg->event->signals.sigusr1)
			event_free(cfg->event->signals.sigusr1);

		/* Remove all local endpoints */
		struct ro_local *local, *local_next;
		for (local = TAILQ_FIRST(&cfg->locals);
		     local != NULL;
		     local = local_next) {
			local_next = TAILQ_NEXT(local, next);
			local_destroy(local); /* Will do TAILQ_REMOVE */
		}

		event_base_free(cfg->event->base);
		free(cfg->event);
	}
}
