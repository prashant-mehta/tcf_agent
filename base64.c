/*******************************************************************************
 * Copyright (c) 2007, 2008 Wind River Systems, Inc. and others.
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
 * This module implements BASE64 encoding and decoding of binary data.
 * BASE64 encoding is adapted from RFC 1421, with one change:
 * BASE64 eliminates the "*" mechanism for embedded clear text.
 * Also TCF version of the encoding does not allow characters outside of the BASE64 alphabet. 
 */

#include "config.h"
#include <assert.h>
#include "base64.h"
#include "exceptions.h"
#include "errors.h"

static const char int2char[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3',
    '4', '5', '6', '7', '8', '9', '+', '/'
};

static const int char2int[] = {
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  62,  -1,  -1,  -1,  63,
    52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,   0,   1,   2,   3,   4,   5,   6,
     7,   8,   9,  10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,
    23,  24,  25,  -1,  -1,  -1,  -1,  -1,
    -1,  26,  27,  28,  29,  30,  31,  32,
    33,  34,  35,  36,  37,  38,  39,  40,
    41,  42,  43,  44,  45,  46,  47,  48,
    49, 50, 51
};

int write_base64(OutputStream * out, const char * buf0, int len) {
    int pos = 0;
    const unsigned char * buf = (const unsigned char *)buf0;

    while (pos < len) {
        int byte0 = buf[pos++];
        write_stream(out, int2char[byte0 >> 2]);
        if (pos == len) {
            write_stream(out, int2char[(byte0 << 4) & 0x3f]);
            write_stream(out, '=');
            write_stream(out, '=');
        }
        else {
            int byte1 = buf[pos++];
            write_stream(out, int2char[(byte0 << 4) & 0x3f | (byte1 >> 4)]);
            if (pos == len) {
                write_stream(out, int2char[(byte1 << 2) & 0x3f]);
                write_stream(out, '=');
            }
            else {
                int byte2 = buf[pos++];
                write_stream(out, int2char[(byte1 << 2) & 0x3f | (byte2 >> 6)]);
                write_stream(out, int2char[byte2 & 0x3f]);
            }
        }
    }
    assert(pos == len);
    return ((len + 2) / 3) * 4;
}

int read_base64(InputStream * inp, char * buf, int buf_size) {
    int pos = 0;
    int ch_max = sizeof(char2int) / sizeof(int);

    assert(buf_size >= 3);
    while (pos + 3 <= buf_size) {
        int n0, n1, n2, n3;
        int ch0, ch1, ch2, ch3;

        ch0 = peek_stream(inp);
        if (ch0 < 0 || ch0 >= ch_max || (n0 = char2int[ch0]) < 0) break;
        read_stream(inp);
        ch1 = read_stream(inp);
        ch2 = read_stream(inp);
        ch3 = read_stream(inp);
        if (ch1 < 0 || ch1 >= ch_max || (n1 = char2int[ch1]) < 0) exception(ERR_BASE64);
        buf[pos++] = (char)((n0 << 2) | (n1 >> 4));
        if (ch2 == '=') break;
        if (ch2 < 0 || ch2 >= ch_max || (n2 = char2int[ch2]) < 0) exception(ERR_BASE64);
        buf[pos++] = (char)((n1 << 4) | (n2 >> 2));
        if (ch3 == '=') break;
        if (ch3 < 0 || ch3 >= ch_max || (n3 = char2int[ch3]) < 0) exception(ERR_BASE64);
        buf[pos++] = (char)((n2 << 6) | n3);
    }
    return pos;
}

