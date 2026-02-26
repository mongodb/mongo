/*
 * Copyright (c) 2020 [Ribose Inc](https://www.ribose.com).
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

#include "rnp_tests.h"

TEST_F(rnp_tests, test_utils_hex2bin)
{
    // with 0x prefix
    {
        uint8_t buf[4];
        assert_int_equal(rnp::hex_decode("0xfeedbeef", buf, sizeof(buf)), 4);
        assert_int_equal(0, memcmp(buf, "\xfe\xed\xbe\xef", 4));
    }
    // with 0X prefix, capital
    {
        uint8_t buf[4];
        assert_int_equal(rnp::hex_decode("0XFEEDBEEF", buf, sizeof(buf)), 4);
        assert_int_equal(0, memcmp(buf, "\xfe\xed\xbe\xef", 4));
    }
    // without 0x prefix
    {
        uint8_t buf[4];
        assert_int_equal(rnp::hex_decode("feedbeef", buf, sizeof(buf)), 4);
        assert_int_equal(0, memcmp(buf, "\xfe\xed\xbe\xef", 4));
    }
    // keyid with spaces
    {
        uint8_t buf[PGP_KEY_ID_SIZE];
        assert_int_equal(rnp::hex_decode("4be1 47bb 22df 1e60", buf, sizeof(buf)),
                         PGP_KEY_ID_SIZE);
        assert_int_equal(0, memcmp(buf, "\x4b\xe1\x47\xbb\x22\xdf\x1e\x60", PGP_KEY_ID_SIZE));
    }
    // keyid with spaces and tab
    {
        uint8_t buf[PGP_KEY_ID_SIZE];
        assert_int_equal(rnp::hex_decode("    4be147bb\t22df1e60   ", buf, sizeof(buf)),
                         PGP_KEY_ID_SIZE);
        assert_int_equal(0, memcmp(buf, "\x4b\xe1\x47\xbb\x22\xdf\x1e\x60", PGP_KEY_ID_SIZE));
    }
    // buffer is too small
    {
        uint8_t buf[4];
        assert_int_equal(rnp::hex_decode("4be147bb22df1e60   ", buf, sizeof(buf)), 0);
    }
    // wrong hex length
    {
        uint8_t buf[2];
        assert_int_equal(rnp::hex_decode("0xA", buf, sizeof(buf)), 0);
    }
    // wrong hex chars
    {
        uint8_t buf[2];
        assert_int_equal(rnp::hex_decode("0xYY", buf, sizeof(buf)), 0);
    }
    // too small buffer for encoding
    {
        uint8_t buf[2] = {0xAB, 0xCD};
        char    hex[4];
        assert_false(
          rnp::hex_encode(buf, sizeof(buf), hex, sizeof(hex), rnp::HexFormat::Lowercase));
    }
}
