/*******************************************************************************
 * Copyright (c) 2008 Wind River Systems, Inc. and others.
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
 * This module implements DWARF expressions evaluation.
 *
 * Functions in this module use exceptions to report errors, see exceptions.h
 */
#ifndef D_dwarfexpr
#define D_dwarfexpr

#include "dwarfcache.h"

extern void dwarf_evaluate_expression(U8_T base, PropertyValue * value);

#endif /* D_dwarfexpr */
