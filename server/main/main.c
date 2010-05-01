/*******************************************************************************
 * Copyright (c) 2007, 2009 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * TCF Server main module.
 *
 * TCF Server is a value-add that provides StackTrace, Symbols, LineNumbers and Expressions services.
 */

#define CONFIG_MAIN
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include "asyncreq.h"
#include "events.h"
#include "trace.h"
#include "myalloc.h"
#include "errors.h"
#include "proxy.h"
#include "context-proxy.h"

static char * progname;
static Protocol * proto;
static ChannelServer * serv;
static TCFBroadcastGroup * bcg;

static void channel_redirection_listener(Channel * host, Channel * target) {
    if (target->state == ChannelStateStarted) {
        ini_line_numbers_service(target->protocol);
        ini_symbols_service(target->protocol);
    }
    if (target->state == ChannelStateConnected) {
        int i;
        int service_ln = 0;
        int service_mm = 0;
        int service_pm = 0;
        int service_sm = 0;
        for (i = 0; i < target->peer_service_cnt; i++) {
            char * nm = target->peer_service_list[i];
            if (strcmp(nm, "LineNumbers") == 0) service_ln = 1;
            if (strcmp(nm, "Symbols") == 0) service_sm = 1;
            if (strcmp(nm, "MemoryMap") == 0) service_mm = 1;
            if (strcmp(nm, "PathMap") == 0) service_pm = 1;
        }
        if (!service_pm) {
            ini_path_map_service(host->protocol);
        }
        if (service_mm) {
            if (!service_ln) ini_line_numbers_service(host->protocol);
            if (!service_sm) ini_symbols_service(host->protocol);
            create_context_proxy(host, target);
        }
    }
}

static void channel_new_connection(ChannelServer * serv, Channel * c) {
    protocol_reference(proto);
    c->protocol = proto;
    channel_set_broadcast_group(c, bcg);
    channel_start(c);
}

#if defined(_WRS_KERNEL)
int tcf_va(void) {
#else
int main(int argc, char ** argv) {
#endif
    int c;
    int ind;
    char * s;
    char * log_name = 0;
    char * url = "TCP:";
    PeerServer * ps;

    ini_mdep();
    ini_trace();
    ini_events_queue();
    ini_asyncreq();

#if defined(_WRS_KERNEL)

    progname = "tcf";
    open_log_file("-");
    log_mode = 0;

#else

    progname = argv[0];

    /* Parse arguments */
    for (ind = 1; ind < argc; ind++) {
        s = argv[ind];
        if (*s != '-') {
            break;
        }
        s++;
        while ((c = *s++) != '\0') {
            switch (c) {
            case 'l':
            case 'L':
            case 's':
                if (*s == '\0') {
                    if (++ind >= argc) {
                        fprintf(stderr, "%s: error: no argument given to option '%c'\n", progname, c);
                        exit(1);
                    }
                    s = argv[ind];
                }
                switch (c) {
                case 'l':
                    log_mode = strtol(s, 0, 0);
                    break;

                case 'L':
                    log_name = s;
                    break;

                case 's':
                    url = s;
                    break;

                default:
                    fprintf(stderr, "%s: error: illegal option '%c'\n", progname, c);
                    exit(1);
                }
                s = "";
                break;

            default:
                fprintf(stderr, "%s: error: illegal option '%c'\n", progname, c);
                exit(1);
            }
        }
    }

    open_log_file(log_name);

#endif

    bcg = broadcast_group_alloc();
    proto = protocol_alloc();
    ini_services(proto, bcg);

    ps = channel_peer_from_url(url);
    if (ps == NULL) {
        fprintf(stderr, "%s: invalid server URL (-s option value): %s\n", progname, url);
        exit(1);
    }
    peer_server_addprop(ps, loc_strdup("Name"), loc_strdup("TCF Proxy"));
    peer_server_addprop(ps, loc_strdup("Proxy"), loc_strdup(""));
    serv = channel_server(ps);
    if (serv == NULL) {
        fprintf(stderr, "%s: cannot create TCF server: %s\n", progname, errno_to_str(errno));
        exit(1);
    }
    serv->new_conn = channel_new_connection;
    add_channel_redirection_listener(channel_redirection_listener);

    discovery_start();

    run_event_loop();
    return 0;
}