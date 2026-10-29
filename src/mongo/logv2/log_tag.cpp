// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/log_tag.h"

#include "mongo/bson/bsonobjbuilder.h"

#include <string_view>

namespace mongo::logv2 {
using namespace std::literals::string_view_literals;

BSONArray LogTag::toBSONArray() {
    BSONArrayBuilder builder;
    if (_value & kStartupWarnings) {
        builder.append("startupWarnings"sv);
    }
    if (_value & kPlainShell) {
        builder.append("plainShellOutput"sv);
    }

    return builder.arr();
}

}  // namespace mongo::logv2
