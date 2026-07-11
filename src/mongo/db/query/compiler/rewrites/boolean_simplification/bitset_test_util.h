// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_tree.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::boolean_simplification {
inline BitsetTerm makeBitsetTerm(const Minterm& minterm) {
    return minterm;
}

inline BitsetTerm makeBitsetTerm(std::string_view predicates, std::string_view mask) {
    return BitsetTerm{Bitset{std::string{predicates}}, Bitset{std::string{mask}}};
}
}  // namespace mongo::boolean_simplification
