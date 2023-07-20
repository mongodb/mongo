/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The two variants are almost identical modulo function signatures. Define the macro that causes
 * the normal version to use signatures compatible with the _r variant, and include the source file
 * directly. This doesn't hide the "regular" quicksort since this is a different compilation unit.
 */
#define WT_QSORT_R_DEF
#include "qsort.c"
