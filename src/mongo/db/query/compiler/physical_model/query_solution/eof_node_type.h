// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {
namespace eof_node {
enum EOFType { NonExistentNamespace, PredicateEvalsToFalse };
static std::string typeStr(const eof_node::EOFType& type) {
    switch (type) {
        case eof_node::EOFType::NonExistentNamespace:
            return "nonExistentNamespace";
        case eof_node::EOFType::PredicateEvalsToFalse:
            return "predicateEvalsToFalse";
    }
    MONGO_UNREACHABLE;
}
}  // namespace eof_node
}  // namespace mongo
