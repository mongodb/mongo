// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/int128.h"

#include <absl/strings/str_cat.h>

namespace absl {

std::string toString(const uint128& v) {
    std::string s;
    StrAppend(&s, v);
    return s;
}

std::string toString(const int128& v) {
    std::string s;
    StrAppend(&s, v);
    return s;
}

}  // namespace absl
