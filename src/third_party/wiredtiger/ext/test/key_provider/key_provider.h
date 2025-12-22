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

#include "wiredtiger_ext.h"

#include <time.h>

/*
 * A test key provider extension. This extension implements the WT_KEY_PROVIDER interface to provide
 * encryption key management functionality for testing purposes.
 *
 * Configuration parameters:
 * - verbose: verbosity level for logging (default: WT_VERBOSE_INFO)
 * - key_expires: key expiration period, in seconds (default: 12 hours = 43200 seconds).
 *     On creation, initial key is set.
 *     Special values:
 *       -1 - key never expires, i.e. get_key always returns without a key
 *        0 - key always expired, i.e. every get_key call gets the default key
 *
 * Enforced key states:
 * - KEY_STATE_CURRENT: the current active key used for new checkpoint writes
 *    allowed API calls: get_key(size), key_load
 * - KEY_STATE_PENDING: the key size has been requested and successfully retrieved
 *    allowed API calls: get_key(data)
 * - KEY_STATE_READ: the key data has been read
 *    allowed API calls: on_key_update
 *
 * Transitions:
 * key not expired:
 * - CURRENT -> CURRENT: get_key called with empty key data, no change
 * key expired:
 * - CURRENT -> PENDING: get_key called with empty key data, key size returned
 * - PENDING -> READ: get_key called with allocated key data, key data filled
 * - READ -> CURRENT: on_key_update called, key marked as current
 * irrespective of key expiration:
 * - CURRENT -> CURRENT: key_load called, key loaded from persisted data
 */

enum {
    KEY_EXPIRES_NEVER = -1, /* Key never expires */
    KEY_EXPIRES_ALWAYS = 0  /* Key always expires */
};

typedef enum { KEY_STATE_CURRENT = 0, KEY_STATE_PENDING, KEY_STATE_READ } KEY_STATE;

typedef struct {
    /* Key provider interface */
    WT_KEY_PROVIDER iface;

    /* Extension API for logging and error reporting */
    WT_EXTENSION_API *wtext;

    /* Configuration options */
    int verbose;     /* Verbosity level for logging. See WT_VERBOSE_LEVEL . */
    int key_expires; /* Key expiration time in seconds, or special values as described above */

    /* Simulated key state */
    struct {
        uint64_t lsn;
        KEY_STATE key_state;
        clock_t key_time;
        size_t key_size;
        uint8_t *key_data;
    } state;
} KEY_PROVIDER;
