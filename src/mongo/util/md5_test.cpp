// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/md5.h"

#include "mongo/unittest/unittest.h"

#include <limits>

#include <tomcrypt.h>

namespace mongo {

// Test from old Aladdin MD5 library we used to vendor
int do_md5_test(void) {
    static const char* const test[7 * 2] = {
        "",
        "d41d8cd98f00b204e9800998ecf8427e",
        "a",
        "0cc175b9c0f1b6a831c399e269772661",
        "abc",
        "900150983cd24fb0d6963f7d28e17f72",
        "message digest",
        "f96b697d7cb7938d525a2f31aaf161d0",
        "abcdefghijklmnopqrstuvwxyz",
        "c3fcd3d76192e4007dfb496cca67e13b",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
        "d174ab98d277d9f5a5611c2c9f419d9f",
        "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
        "57edf4a22be3c955ac49da2e2107b67a"};
    int i;
    int status = 0;

    for (i = 0; i < 7 * 2; i += 2) {
        md5_state_t state;
        md5_byte_t digest[16];
        char hex_output[16 * 2 + 1];
        int di;

        md5_init_state_deprecated(&state);
        md5_append_deprecated(&state, (const md5_byte_t*)test[i], strlen(test[i]));
        md5_finish_deprecated(&state, digest);
        for (di = 0; di < 16; ++di)
            sprintf(hex_output + di * 2, "%02x", digest[di]);
        if (strcmp(hex_output, test[i + 1]) != 0) {
            printf("MD5 (\"%s\") = ", test[i]);
            puts(hex_output);
            printf("**** ERROR, should be: %s\n", test[i + 1]);
            status = 1;
        }
    }
    return status;
}

TEST(MD5, BuiltIn1) {
    ASSERT_EQUALS(0, do_md5_test());
    // Run tomcrypt test
    ASSERT_EQUALS(0, md5_test());
}

// LibTomCrypt returns an error when internal state is inconsistent (e.g. corrupted curlen or
// length). Our wrappers must surface that as uassert 12220700.
TEST(MD5, Md5FinishFailsWhenCurlenEqualsBlockSize) {
    md5_state_t state;
    md5_init_state_deprecated(&state);
    state.md5.curlen = 64;
    md5_byte_t digest[16];
    ASSERT_THROWS_CODE(md5_finish_deprecated(&state, digest), DBException, 12220700);
}

TEST(MD5, Md5AppendFailsWhenCurlenGreaterThanBlockSize) {
    md5_state_t state;
    md5_init_state_deprecated(&state);
    state.md5.curlen = 65;
    md5_byte_t b = 0;
    ASSERT_THROWS_CODE(md5_append_deprecated(&state, &b, 1), DBException, 12220700);
}

TEST(MD5, Md5AppendFailsOnLengthOverflow) {
    md5_state_t state;
    md5_init_state_deprecated(&state);
    state.md5.length = std::numeric_limits<decltype(state.md5.length)>::max();
    md5_byte_t b = 0;
    ASSERT_THROWS_CODE(md5_append_deprecated(&state, &b, 1), DBException, 12220700);
}
}  // namespace mongo
