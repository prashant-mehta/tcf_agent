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
 * Add operation service header file.
 * Prints sum of two input 64-bit signed integers
 * Example command - tcf Add inputs -254 458
 */

#ifndef ADD_H_
#define ADD_H_

#include <tcf/config.h>
#include <tcf/framework/protocol.h>

extern void ini_add_service(Protocol * proto);

#endif /*ADD_H_*/
