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

#include "rd.h"
#include "rdgz.h"

#include <zlib.h>


#define RD_GZ_CHUNK 262144

void *rd_gz_decompress(const void *compressed,
                       int compressed_len,
                       uint64_t *decompressed_lenp) {
        int pass           = 1;
        char *decompressed = NULL;

        /* First pass (1): calculate decompressed size.
         *                 (pass-1 is skipped if *decompressed_lenp is
         *                  non-zero).
         * Second pass (2): perform actual decompression.
         */

        if (*decompressed_lenp != 0LLU)
                pass++;

        for (; pass <= 2; pass++) {
                z_stream strm = RD_ZERO_INIT;
                char buf[512];
                char *p;
                int len;
                int r;

                if ((r = inflateInit2(&strm, 15 + 32)) != Z_OK)
                        goto fail;

                strm.next_in  = (void *)compressed;
                strm.avail_in = compressed_len;

                if (pass == 1) {
                        /* Use dummy output buffer */
                        p   = buf;
                        len = sizeof(buf);
                } else {
                        /* Use real output buffer */
                        p   = decompressed;
                        len = (int)*decompressed_lenp;
                }

                do {
                        strm.next_out  = (unsigned char *)p;
                        strm.avail_out = len;

                        r = inflate(&strm, Z_NO_FLUSH);
                        switch (r) {
                        case Z_STREAM_ERROR:
                        case Z_NEED_DICT:
                        case Z_DATA_ERROR:
                        case Z_MEM_ERROR:
                                inflateEnd(&strm);
                                goto fail;
                        }

                        if (pass == 2) {
                                /* Advance output pointer (in pass 2). */
                                p += len - strm.avail_out;
                                len -= len - strm.avail_out;
                        }

                } while (strm.avail_out == 0 && r != Z_STREAM_END);


                if (pass == 1) {
                        *decompressed_lenp = strm.total_out;
                        if (!(decompressed = rd_malloc(
                                  (size_t)(*decompressed_lenp) + 1))) {
                                inflateEnd(&strm);
                                return NULL;
                        }
                        /* For convenience of the caller we nul-terminate
                         * the buffer. If it happens to be a string there
                         * is no need for extra copies. */
                        decompressed[*decompressed_lenp] = '\0';
                }

                inflateEnd(&strm);
        }

        return decompressed;

fail:
        if (decompressed)
                rd_free(decompressed);
        return NULL;
}
