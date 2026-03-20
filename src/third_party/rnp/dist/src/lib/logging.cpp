/*-
 * Copyright (c) 2017-2021 Ribose Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "string.h"
#include "logging.h"

/* -1 -- not initialized
    0 -- logging is off
    1 -- logging is on
*/
static int8_t _rnp_log_switch =
#ifdef NDEBUG
  -1 // lazy-initialize later
#else
  1 // always on in debug build
#endif
  ;

/* Temporary disable logging */
static size_t _rnp_log_disable = 0;

void
set_rnp_log_switch(int8_t value)
{
    _rnp_log_switch = value;
}

bool
rnp_log_switch()
{
    if (_rnp_log_switch < 0) {
        const char *var = getenv(RNP_LOG_CONSOLE);
        _rnp_log_switch = (var && strcmp(var, "0")) ? 1 : 0;
    }
    return !_rnp_log_disable && !!_rnp_log_switch;
}

void
rnp_log_stop()
{
    if (_rnp_log_disable < SIZE_MAX) {
        _rnp_log_disable++;
    }
}

void
rnp_log_continue()
{
    if (_rnp_log_disable) {
        _rnp_log_disable--;
    }
}
