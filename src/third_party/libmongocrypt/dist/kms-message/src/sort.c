/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Peter McIlroy.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * This code is originally from:
 * https://github.com/freebsd/freebsd/blob/e7c6cef9514d3bb1f14a30a5ee871231523e43db/lib/libc/stdlib/merge.c
 */

#include <stddef.h>

/*
 * This is to avoid out-of-bounds addresses in sorting the
 * last 4 elements.
 */

typedef int (*cmp_t) (const void *, const void *);
#define CMP(x, y) cmp (x, y)
#define swap(a, b)   \
   {                 \
      s = b;         \
      i = size;      \
      do {           \
         tmp = *a;   \
         *a++ = *s;  \
         *s++ = tmp; \
      } while (--i); \
      a -= size;     \
   }

void
insertionsort (unsigned char *a, size_t n, size_t size, cmp_t cmp)
{
   unsigned char *ai, *s, *t, *u, tmp;
   size_t i;

   for (ai = a + size; --n >= 1; ai += size)
      for (t = ai; t > a; t -= size) {
         u = t - size;
         if (CMP (u, t) <= 0)
            break;
         swap (u, t);
      }
}
