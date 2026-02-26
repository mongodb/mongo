/*
 * Copyright (c) 2021 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/** Time utilities
 *  @file
 */

#include <stdint.h>
#include "time-utils.h"

static inline time_t
adjust_time32(time_t t)
{
    return (sizeof(t) == 4 && t < 0) ? INT32_MAX : t;
}

bool
rnp_y2k38_warning(time_t t)
{
    return (sizeof(t) == 4 && (t < 0 || t == INT32_MAX));
}

time_t
rnp_mktime(struct tm *tm)
{
    return adjust_time32(mktime(tm));
}

void
rnp_gmtime(time_t t, struct tm &tm)
{
    time_t adjusted = adjust_time32(t);
#ifndef _WIN32
    gmtime_r(&adjusted, &tm);
#else
    (void) gmtime_s(&tm, &adjusted);
#endif
}

void
rnp_localtime(time_t t, struct tm &tm)
{
    time_t adjusted = adjust_time32(t);
#ifndef _WIN32
    localtime_r(&adjusted, &tm);
#else
    (void) localtime_s(&tm, &adjusted);
#endif
}

std::string
rnp_ctime(time_t t)
{
    char   time_buf[26];
    time_t adjusted = adjust_time32(t);
#ifndef _WIN32
    (void) ctime_r(&adjusted, time_buf);
#else
    (void) ctime_s(time_buf, sizeof(time_buf), &adjusted);
#endif
    return std::string(time_buf);
}

time_t
rnp_timeadd(time_t t1, time_t t2)
{
    if (sizeof(time_t) == 4) {
        if (t1 < 0 || t2 < 0) {
            return INT32_MAX;
        }
        return adjust_time32(t1 + t2);
    }
    return t1 + t2;
}
