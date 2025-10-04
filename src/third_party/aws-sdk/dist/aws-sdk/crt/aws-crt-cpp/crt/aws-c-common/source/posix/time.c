/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/time.h>

#if defined(__ANDROID__) && !defined(__LP64__)
/*
 * This branch brought to you by the kind folks at google chromium. It's been modified a bit, but
 * gotta give credit where it's due.... I'm not a lawyer so I'm just gonna drop their copyright
 * notification here to avoid all of that.
 */

/*
 * Copyright 2014 The Chromium Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 * From src/base/os_compat_android.cc:
 */
#    include <time64.h>

static const time_t s_time_max = ~(1L << ((sizeof(time_t) * __CHAR_BIT__ - 1)));
static const time_t s_time_min = (1L << ((sizeof(time_t)) * __CHAR_BIT__ - 1));

/* 32-bit Android has only timegm64() and not timegm(). */
time_t aws_timegm(struct tm *const t) {

    time64_t result = timegm64(t);
    if (result < s_time_min || result > s_time_max) {
        return -1;
    }
    return (time_t)result;
}

#else

#    ifndef __APPLE__
/* glibc.... you disappoint me.. */
extern time_t timegm(struct tm *);
#    endif

time_t aws_timegm(struct tm *const t) {
    return timegm(t);
}

#endif /* defined(__ANDROID__) && !defined(__LP64__) */

void aws_localtime(time_t time, struct tm *t) {
    localtime_r(&time, t);
}

void aws_gmtime(time_t time, struct tm *t) {
    gmtime_r(&time, t);
}
