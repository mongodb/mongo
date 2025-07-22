/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2016-2022, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * System portability
 */

#include "rd.h"


#include <stdlib.h>

/**
 * qsort_r substitute
 * This nicely explains why we wont bother with the native implementation
 * on Win32 (qsort_s), OSX/FreeBSD (qsort_r with diff args):
 * http://forum.theorex.tech/t/different-declarations-of-qsort-r-on-mac-and-linux/93/2
 */
static RD_TLS int (*rd_qsort_r_cmp)(const void *, const void *, void *);
static RD_TLS void *rd_qsort_r_arg;

static RD_UNUSED int rd_qsort_r_trampoline(const void *a, const void *b) {
        return rd_qsort_r_cmp(a, b, rd_qsort_r_arg);
}

void rd_qsort_r(void *base,
                size_t nmemb,
                size_t size,
                int (*compar)(const void *, const void *, void *),
                void *arg) {
        rd_qsort_r_cmp = compar;
        rd_qsort_r_arg = arg;
        qsort(base, nmemb, size, rd_qsort_r_trampoline);
        rd_qsort_r_cmp = NULL;
        rd_qsort_r_arg = NULL;
}
