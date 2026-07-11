// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>

namespace mongo::join_ordering {

/**
 * The types of joins we support in JoinPlans.
 */
enum class JoinMethod {
    // Hash join.
    HJ,
    // Index-nested loop join.
    INLJ,
    // Nested loop join.
    NLJ
};

/**
 * Helpers to serialize/deserialize join method. Deserialization uasserts if string is not a valid
 * join method.
 */
std::string joinMethodToString(JoinMethod method);
JoinMethod joinMethodFromString(const std::string& method);

}  // namespace mongo::join_ordering
