/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

typedef enum { BOOL_TYPE, CONFIG_STRING_TYPE, INT_TYPE, STRING_TYPE, UINT32_TYPE } CONFIG_OPT_TYPE;

typedef struct {
    const char *name;
    const char *description;
    const char *defaultval;
    CONFIG_OPT_TYPE type;
    size_t offset;
} CONFIG_OPT;

typedef struct __config_queue_entry {
    char *string;
    TAILQ_ENTRY(__config_queue_entry) q;
} CONFIG_QUEUE_ENTRY;

typedef struct { /* Option structure */
#define OPT_DECLARE_STRUCT
#include "wtperf_opt_inline.h"
#undef OPT_DECLARE_STRUCT

    /* Queue head to save a copy of the config to be output */
    TAILQ_HEAD(__config_qh, __config_queue_entry) config_head;
} CONFIG_OPTS;
