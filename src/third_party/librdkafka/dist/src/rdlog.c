/*
 * librd - Rapid Development C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
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

#include "rdkafka_int.h"
#include "rdlog.h"

#include <stdarg.h>
#include <string.h>
#include <ctype.h>



void rd_hexdump(FILE *fp, const char *name, const void *ptr, size_t len) {
        const char *p = (const char *)ptr;
        size_t of     = 0;


        if (name)
                fprintf(fp, "%s hexdump (%" PRIusz " bytes):\n", name, len);

        for (of = 0; of < len; of += 16) {
                char hexen[16 * 3 + 1];
                char charen[16 + 1];
                int hof = 0;

                int cof = 0;
                unsigned int i;

                for (i = (unsigned int)of; i < (unsigned int)of + 16 && i < len;
                     i++) {
                        hof += rd_snprintf(hexen + hof, sizeof(hexen) - hof,
                                           "%02x ", p[i] & 0xff);
                        cof +=
                            rd_snprintf(charen + cof, sizeof(charen) - cof,
                                        "%c", isprint((int)p[i]) ? p[i] : '.');
                }
                fprintf(fp, "%08zx: %-48s %-16s\n", of, hexen, charen);
        }
}


void rd_iov_print(const char *what,
                  int iov_idx,
                  const struct iovec *iov,
                  int hexdump) {
        printf("%s:  iov #%i: %" PRIusz "\n", what, iov_idx,
               (size_t)iov->iov_len);
        if (hexdump)
                rd_hexdump(stdout, what, iov->iov_base, iov->iov_len);
}


void rd_msghdr_print(const char *what, const struct msghdr *msg, int hexdump) {
        int i;
        size_t len = 0;

        printf("%s: iovlen %" PRIusz "\n", what, (size_t)msg->msg_iovlen);

        for (i = 0; i < (int)msg->msg_iovlen; i++) {
                rd_iov_print(what, i, &msg->msg_iov[i], hexdump);
                len += msg->msg_iov[i].iov_len;
        }
        printf("%s: ^ message was %" PRIusz " bytes in total\n", what, len);
}
