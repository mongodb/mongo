/*
 * librd - Rapid Development C library
 *
 * Copyright (c) 2012, Magnus Edenhill
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

#include "rd.h"
#include "rdrand.h"
#include "rdtime.h"
#include "tinycthread.h"

int rd_jitter(int low, int high) {
        int rand_num;
#if HAVE_RAND_R
        static RD_TLS unsigned int seed = 0;

        /* Initial seed with time+thread id */
        if (unlikely(seed == 0)) {
                struct timeval tv;
                rd_gettimeofday(&tv, NULL);
                seed = (unsigned int)(tv.tv_usec / 1000);
                seed ^= (unsigned int)(intptr_t)thrd_current();
        }

        rand_num = rand_r(&seed);
#else
        rand_num = rand();
#endif
        return (low + (rand_num % ((high - low) + 1)));
}

void rd_array_shuffle(void *base, size_t nmemb, size_t entry_size) {
        int i;
        void *tmp = rd_alloca(entry_size);

        /* FIXME: Optimized version for word-sized entries. */

        for (i = (int)nmemb - 1; i > 0; i--) {
                int j = rd_jitter(0, i);
                if (unlikely(i == j))
                        continue;

                memcpy(tmp, (char *)base + (i * entry_size), entry_size);
                memcpy((char *)base + (i * entry_size),
                       (char *)base + (j * entry_size), entry_size);
                memcpy((char *)base + (j * entry_size), tmp, entry_size);
        }
}
