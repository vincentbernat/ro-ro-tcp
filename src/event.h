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

#include <unistd.h>
#include <event2/event.h>

#ifndef _RO_EVENT_H
#define _RO_EVENT_H

struct event_private {
	struct event_base *base;
	struct evconnlistener *listener;

	struct {
		struct event *sigint;
		struct event *sigterm;
	} signals;
};

struct local_private {
	struct event *read;
	struct event *write;
	struct {
		struct event *read[2];  /* pipe for splicing from the local endpoint */
		size_t nr;	        /* Number of bytes in read pipe */
		struct event *write[2]; /* Pipe for splicing from the remote */
		size_t nw;		/* Number of bytes in write pipe */
	} pipe;

	struct ro_remote *current_remote; /* We are currently sending to this remote */
	uint32_t remaining_bytes;	  /* We need to send this many bytes */
	size_t partial_bytes;		  /* But before that, we only wrote this many bytes for the header */

	uint32_t send_serial;	 /* Current serial number for sending */
	uint32_t receive_serial; /* Current serial number for receiving */
};

struct remote_private {
	struct event *read;
	struct event *write;

	uint32_t partial_header[2]; /* Partial received header */
	size_t partial_bytes;	    /* Size of partially received header */

	uint32_t remaining_bytes; /* We need to receive this many bytes */
};

static inline void
event_close_and_free(struct event *event)
{
	if (event) {
		close(event_get_fd(event));
		event_free(event);
	}
}

#define MAX_SPLICE_AT_ONCE (1<<30)
#define MAX_SPLICE_BYTES (1448 * 16)

#endif
