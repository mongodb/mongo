/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <tomcrypt.h>

#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/md5.h"

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

        md5_init_state(&state);
        md5_append(&state, (const md5_byte_t*)test[i], strlen(test[i]));
        md5_finish(&state, digest);
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
}  // namespace mongo
