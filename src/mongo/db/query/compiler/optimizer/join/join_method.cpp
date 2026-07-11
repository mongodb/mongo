// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/join_method.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::join_ordering {

std::string joinMethodToString(JoinMethod method) {
    switch (method) {
        case JoinMethod::HJ:
            return "HJ";
        case JoinMethod::NLJ:
            return "NLJ";
        case JoinMethod::INLJ:
            return "INLJ";
    }

    MONGO_UNREACHABLE_TASSERT(11336901);
}

JoinMethod joinMethodFromString(const std::string& mode) {
    if (mode == "HJ") {
        return JoinMethod::HJ;
    } else if (mode == "NLJ") {
        return JoinMethod::NLJ;
    }
    uassert(12016300, str::stream() << "Unexpected join method " << mode, mode == "INLJ");
    return JoinMethod::INLJ;
}

}  // namespace mongo::join_ordering
