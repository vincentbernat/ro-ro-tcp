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

#include <stdio.h>
#include <unistd.h>
#include <string.h>

extern const char *__progname;

int
main(int argc, char *argv[])
{
	int exitcode = EXIT_FAILURE;

	/* Common arguments */
#define RO_COMMON_ARGS(X) \
	struct arg_lit *arg_ ## X ## _debug = arg_litn("d", "debug", 0, 3, "be more verbose"); \
	struct arg_lit *arg_ ## X ## _help  = arg_lit0("h", "help",  "display help and exit"); \
	struct arg_lit *arg_ ## X ## _version     = arg_lit0("v", "version", "print version and exit"); \
	struct arg_int *arg_ ## X ## _listen      = arg_intn("l", "listen", "conns", 0, 1, "listen queue length"); \
	struct arg_addr *arg_ ## X ## _local       = arg_addr1(NULL, NULL, "laddress:lport", "address and port to bind to", ':'); \
	struct arg_addr *arg_ ## X ## _remote      = arg_addr1(NULL, NULL, "raddress:rport", "address and port to connect to", ':');
#define RO_COMMON_ARGTABLE(X) \
	    arg_ ## X ## _debug, arg_ ## X ## _help, arg_ ## X ## _version, arg_ ## X ## _listen

	/* Proxy arguments */
	RO_COMMON_ARGS(proxy);
	struct arg_lit *arg_proxy       = arg_lit1("p", "proxy", "act as a proxy");
	struct arg_int *arg_proxy_conns = arg_int0("z", "connections", "conns", "number of connections to relay");
	struct arg_end *arg_proxy_end   = arg_end(5);
	void *argtable_proxy[] = { RO_COMMON_ARGTABLE(proxy),
				   arg_proxy,
				   arg_proxy_conns,
				   arg_proxy_local, arg_proxy_remote,
				   arg_proxy_end };

	/* Relay arguments */
	RO_COMMON_ARGS(relay);
	struct arg_lit *arg_relay     = arg_lit1("r", "relay", "act as a relay");
	struct arg_end *arg_relay_end = arg_end(5);
	void *argtable_relay[] = { RO_COMMON_ARGTABLE(relay),
				   arg_relay,
				   arg_relay_local, arg_relay_remote,
				   arg_relay_end };

	if (arg_nullcheck(argtable_proxy) != 0 ||
	    arg_nullcheck(argtable_relay) != 0) {
		fprintf(stderr, "%s: insufficient memory\n", __progname);
		goto exit;
	}

	arg_proxy_conns->ival[0] = RO_CONNECTION_NUMBER;
	arg_proxy_listen->ival[0] = arg_relay_listen->ival[0] = RO_LISTEN_QUEUE;

	int nerrors_proxy, nerrors_relay;
	nerrors_proxy = arg_parse(argc, argv, argtable_proxy);
	nerrors_relay = arg_parse(argc, argv, argtable_relay);

	if (arg_proxy_version->count || arg_relay_version->count) {
		fprintf(stdout, "%s\n", PACKAGE_VERSION);
		goto exit;
	}
	int debug = (arg_proxy_debug->count < arg_relay_debug->count)?
	    arg_relay_debug->count:
	    arg_proxy_debug->count;
	if (nerrors_proxy && nerrors_relay) {
		if (arg_relay->count > 0 && arg_relay_help->count == 0) {
			arg_print_errors(stderr, arg_relay_end, __progname);
		}
		if (arg_proxy->count > 0 && arg_proxy_help->count == 0) {
			arg_print_errors(stderr, arg_proxy_end, __progname);
		}
		if (arg_relay->count || !(arg_relay->count + arg_proxy->count)) {
			fprintf(stderr, "Usage: %s", __progname);
			arg_print_syntax(stderr, argtable_relay, "\n");
			arg_print_glossary(stderr, argtable_relay, "  %-25s %s\n");
			fprintf(stderr, "\n");
		}
		if (arg_proxy->count || !(arg_proxy->count + arg_proxy->count)) {
			fprintf(stderr, "Usage: %s", __progname);
			arg_print_syntax(stderr, argtable_proxy, "\n");
			arg_print_glossary(stderr, argtable_proxy, "  %-25s %s\n");
			fprintf(stderr, "\n");
		}
		fprintf(stderr, "see manual page " PACKAGE "(8) for more information\n");
		goto exit;
	}

	log_init(debug, __progname);

	struct ro_cfg cfg = {
		.role = (!nerrors_proxy)?ROLE_PROXY:ROLE_RELAY,
		.local = (!nerrors_proxy)?arg_proxy_local->info:arg_relay_local->info,
		.remote = (!nerrors_proxy)?arg_proxy_remote->info:arg_relay_remote->info,
		.backlog = (!nerrors_proxy)?arg_proxy_listen->ival[0]:arg_relay_listen->ival[0],
		.conns = (!nerrors_proxy)?arg_proxy_conns->ival[0]:0
	};
	TAILQ_INIT(&cfg.locals);
	if (event_configure(&cfg) == -1) {
		log_crit("main", "unable to configure libevent");
		goto exit;
	}

	if (!debug) {
		log_debug("main", "detach from foreground");
		if (daemon(0, 0) != 0) {
			log_warn("main", "failed to detach daemon");
			goto exit;
		}
	}

	if (event_loop(&cfg) == -1) {
		log_crit("main", "unable to run libevent loop");
		goto exit;
	}

	exitcode = EXIT_SUCCESS;
exit:
	event_shutdown(&cfg);
	if (arg_proxy_remote && arg_proxy_remote->info) freeaddrinfo(arg_proxy_remote->info);
	if (arg_proxy_local && arg_proxy_local->info) freeaddrinfo(arg_proxy_local->info);
	if (arg_relay_remote && arg_relay_remote->info) freeaddrinfo(arg_relay_remote->info);
	if (arg_relay_local && arg_relay_local->info) freeaddrinfo(arg_relay_local->info);
	arg_freetable(argtable_proxy, sizeof(argtable_proxy) / sizeof(argtable_proxy[0]));
	arg_freetable(argtable_relay, sizeof(argtable_relay) / sizeof(argtable_relay[0]));
	return exitcode;
}
