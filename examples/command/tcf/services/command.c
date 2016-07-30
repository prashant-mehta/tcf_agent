/*******************************************************************************
 * Copyright (c) 2008, 2011 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 * You may elect to redistribute this code under either of these licenses.
 *
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * Execute linux commands service implementation.
 * Example - For listing all USB devices connected -
 * 				 tcf Command execute "lsusb"
 */

#include <tcf/config.h>
#include <tcf/framework/json.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/exceptions.h>
#include <tcf/services/command.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char * COMMAND = "Command";

static void execute_command (char * token, Channel * c) {

    FILE *fp;
    char command[0x100];
    char commandOutput[0x1000];

    int len = json_read_string(&c->inp, command, sizeof(command));
    if ((len < 0) || (len >= (int)sizeof(command))) exception(ERR_JSON_SYNTAX);
    json_test_char(&c->inp, MARKER_EOA);
    json_test_char(&c->inp, MARKER_EOM);

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);

    // Reference - http://stackoverflow.com/questions/646241/c-run-a-system-command-and-get-output
    fp = popen(command, "r");
    
	// If no output    
    if (fp == NULL) {
        json_write_string(&c->out, "Failed to run command\n");
        write_stream(&c->out, 0);
        write_stream(&c->out, MARKER_EOM);
	    /* close */
	    pclose(fp);
	    exit(1);
    }

    json_write_string(&c->out, "USB Devices connected to the server are -");
    write_stream(&c->out, '\n');

    /* Read the output a line at a time - output it. */
    while (fgets(commandOutput, sizeof(commandOutput)-1, fp) != NULL) {
        json_write_string_len(&c->out, commandOutput, strlen(commandOutput)-1);
        write_stream(&c->out, '\n');
    }
    
    write_stream(&c->out, 0);
    write_stream(&c->out, MARKER_EOM);

    /* close */
    pclose(fp);

}

void ini_command_service(Protocol * proto) {
    // Install command handler
    add_command_handler(proto, COMMAND, "execute", execute_command);
}
