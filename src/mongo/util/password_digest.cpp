// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/password_digest.h"

#include "mongo/util/md5.h"

#include <string_view>

namespace mongo {

std::string createPasswordDigest(std::string_view username, std::string_view clearTextPassword) {
    md5digest d;
    {
        md5_state_t st;
        md5_init_state_deprecated(&st);
        md5_append_deprecated(&st, (const md5_byte_t*)username.data(), username.size());
        md5_append_deprecated(&st, (const md5_byte_t*)":mongo:", 7);
        md5_append_deprecated(
            &st, (const md5_byte_t*)clearTextPassword.data(), clearTextPassword.size());
        md5_finish_deprecated(&st, d);
    }
    return digestToString(d);
}

}  // namespace mongo
