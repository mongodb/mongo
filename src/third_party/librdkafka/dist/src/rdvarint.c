/*
 * librdkafka - The Apache Kafka C/C++ library
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


#include "rdvarint.h"
#include "rdunittest.h"


static int do_test_rd_uvarint_enc_i64(const char *file,
                                      int line,
                                      int64_t num,
                                      const char *exp,
                                      size_t exp_size) {
        char buf[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        size_t sz    = rd_uvarint_enc_i64(buf, sizeof(buf), num);
        size_t r;
        int ir;
        rd_buf_t b;
        rd_slice_t slice, bad_slice;
        int64_t ret_num;

        if (sz != exp_size || memcmp(buf, exp, exp_size))
                RD_UT_FAIL("i64 encode of %" PRId64
                           ": "
                           "expected size %" PRIusz " (got %" PRIusz ")\n",
                           num, exp_size, sz);

        /* Verify with standard decoder */
        r = rd_varint_dec_i64(buf, sz, &ret_num);
        RD_UT_ASSERT(!RD_UVARINT_DEC_FAILED(r),
                     "varint decode failed: %" PRIusz, r);
        RD_UT_ASSERT(ret_num == num,
                     "varint decode returned wrong number: "
                     "%" PRId64 " != %" PRId64,
                     ret_num, num);

        /* Verify with slice decoder */
        rd_buf_init(&b, 1, 0);
        rd_buf_push(&b, buf, sizeof(buf), NULL); /* including trailing 0xff
                                                  * garbage which should be
                                                  * ignored by decoder */
        rd_slice_init_full(&slice, &b);

        /* Should fail for incomplete reads */
        ir = rd_slice_narrow_copy(&slice, &bad_slice, sz - 1);
        RD_UT_ASSERT(ir, "narrow_copy failed");
        ret_num = -1;
        r       = rd_slice_read_varint(&bad_slice, &ret_num);
        RD_UT_ASSERT(RD_UVARINT_DEC_FAILED(r),
                     "varint decode failed should have failed, "
                     "returned %" PRIusz,
                     r);
        r = rd_slice_offset(&bad_slice);
        RD_UT_ASSERT(r == 0,
                     "expected slice position to not change, but got %" PRIusz,
                     r);

        /* Verify proper slice */
        ret_num = -1;
        r       = rd_slice_read_varint(&slice, &ret_num);
        RD_UT_ASSERT(!RD_UVARINT_DEC_FAILED(r),
                     "varint decode failed: %" PRIusz, r);
        RD_UT_ASSERT(ret_num == num,
                     "varint decode returned wrong number: "
                     "%" PRId64 " != %" PRId64,
                     ret_num, num);
        RD_UT_ASSERT(r == sz,
                     "expected varint decoder to read %" PRIusz
                     " bytes, "
                     "not %" PRIusz,
                     sz, r);
        r = rd_slice_offset(&slice);
        RD_UT_ASSERT(r == sz,
                     "expected slice position to change to %" PRIusz
                     ", but got %" PRIusz,
                     sz, r);


        rd_buf_destroy(&b);

        RD_UT_PASS();
}


int unittest_rdvarint(void) {
        int fails = 0;

        fails += do_test_rd_uvarint_enc_i64(__FILE__, __LINE__, 0,
                                            (const char[]) {0}, 1);
        fails += do_test_rd_uvarint_enc_i64(__FILE__, __LINE__, 1,
                                            (const char[]) {0x2}, 1);
        fails += do_test_rd_uvarint_enc_i64(__FILE__, __LINE__, -1,
                                            (const char[]) {0x1}, 1);
        fails += do_test_rd_uvarint_enc_i64(__FILE__, __LINE__, 23,
                                            (const char[]) {0x2e}, 1);
        fails += do_test_rd_uvarint_enc_i64(__FILE__, __LINE__, -23,
                                            (const char[]) {0x2d}, 1);
        fails += do_test_rd_uvarint_enc_i64(__FILE__, __LINE__, 253,
                                            (const char[]) {0xfa, 3}, 2);
        fails += do_test_rd_uvarint_enc_i64(
            __FILE__, __LINE__, 1234567890101112,
            (const char[]) {0xf0, 0x8d, 0xd3, 0xc8, 0xa7, 0xb5, 0xb1, 0x04}, 8);
        fails += do_test_rd_uvarint_enc_i64(
            __FILE__, __LINE__, -1234567890101112,
            (const char[]) {0xef, 0x8d, 0xd3, 0xc8, 0xa7, 0xb5, 0xb1, 0x04}, 8);

        return fails;
}
