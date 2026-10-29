// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings_comment.h"

namespace mongo::query_settings {
std::strong_ordering Comment::operator<=>(const Comment& other) const {
    return getElement().woCompare(other.getElement()) <=> 0;
}

bool Comment::operator==(const Comment& other) const {
    return getElement().woCompare(other.getElement()) == 0;
}
}  // namespace mongo::query_settings
