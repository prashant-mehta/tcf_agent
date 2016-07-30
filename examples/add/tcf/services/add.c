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
 * Add operation service implementation.
 * Prints sum of two input 64-bit signed integers
 * Example command - tcf Add inputs -254 458
 */

#include <tcf/config.h>
#include <tcf/framework/json.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/exceptions.h>
#include <tcf/services/add.h>


static const char * ADD = "Add";

static void command_add_two_numbers(char * token, Channel * c) {

	int64_t inputOne, inputTwo;

    // Read command argumnet: string TZ - time zone name
    //json_read_string(&c->inp, str, sizeof(str));
	inputOne = json_read_int64(&c->inp);
    // Each JSON encoded argument should end with zero byte
    json_test_char(&c->inp, MARKER_EOA);

	inputTwo = json_read_int64(&c->inp);
    
    json_test_char(&c->inp, MARKER_EOA);
    json_test_char(&c->inp, MARKER_EOM);

    // Start reply message with zero terminated string "R"
    write_stringz(&c->out, "R");
    // Send back the command token
    write_stringz(&c->out, token);
    // Send reply data

    json_write_int64(&c->out, inputOne + inputTwo);
    // JSON encoded data should end with zero byte
    c->out.write(&c->out, 0);
    // Done sending reply data.
    // The reply message should end with MARKER_EOM (End Of Message)
    c->out.write(&c->out, MARKER_EOM);
    // Done sending reply message.
    // Command handling is complete.
}

void ini_add_service(Protocol * proto) {
        // Install command handler
    add_command_handler(proto, ADD, "inputs", command_add_two_numbers);
}
