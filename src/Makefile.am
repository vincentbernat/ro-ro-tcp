bin_PROGRAMS = ro-ro-tcp
dist_man_MANS = ro-ro-tcp.8

ro_ro_tcp_SOURCES  = log.c log.h arg.c \
		     ro-ro-tcp.h ro-ro-tcp.c \
                     event.h event.c connection.c forward.c endpoint.c
ro_ro_tcp_CFLAGS   = @LIBEVENT_CFLAGS@ @ARGTABLE_CFLAGS@
ro_ro_tcp_LDFLAGS  = @LIBEVENT_LIBS@   @ARGTABLE_LIBS@
