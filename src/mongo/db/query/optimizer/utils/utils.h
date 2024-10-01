/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#include "mongo/db/query/optimizer/bool_expression.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/const_fold_interface.h"
#include "mongo/db/query/optimizer/utils/physical_plan_builder.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/id_generator.h"
#include "mongo/util/str.h"


namespace mongo::optimizer {
template <typename Builder>
ABT makeBalancedTreeImpl(Builder builder, std::vector<ABT>& leaves, size_t from, size_t until) {
    invariant(from < until);
    if (from + 1 == until) {
        return std::move(leaves[from]);
    } else {
        size_t mid = from + (until - from) / 2;
        auto lhs = makeBalancedTreeImpl(builder, leaves, from, mid);
        auto rhs = makeBalancedTreeImpl(builder, leaves, mid, until);
        return builder(std::move(lhs), std::move(rhs));
    }
}

template <typename Builder>
ABT makeBalancedTree(Builder builder, std::vector<ABT> leaves) {
    return makeBalancedTreeImpl(builder, leaves, 0, leaves.size());
}

ABT makeBalancedBooleanOpTree(Operations logicOp, std::vector<ABT> leaves);

inline void updateHash(size_t& result, const size_t hash) {
    result = 31 * result + hash;
}

inline void updateHashUnordered(size_t& result, const size_t hash) {
    result ^= hash;
}

template <class T,
          class Hasher = std::hash<T>,
          class T1 = std::conditional_t<std::is_arithmetic_v<T>, const T, const T&>>
inline size_t computeVectorHash(const std::vector<T>& v) {
    size_t result = 17;
    for (T1 e : v) {
        updateHash(result, Hasher()(e));
    }
    return result;
}

template <int typeCode, typename... Args>
inline size_t computeHashSeq(const Args&... seq) {
    size_t result = 17 + typeCode;
    (updateHash(result, seq), ...);
    return result;
}

/**
 * Used to access and manipulate the child of a unary node.
 */
template <class NodeType>
struct DefaultChildAccessor {
    const ABT& operator()(const ABT& node) const {
        return node.cast<NodeType>()->getChild();
    }

    ABT& operator()(ABT& node) const {
        return node.cast<NodeType>()->getChild();
    }
};

/**
 * Used to access children of a n-ary node. By default, it accesses the first child.
 */
template <class NodeType>
struct IndexedChildAccessor {
    const ABT& operator()(const ABT& node) const {
        return node.cast<NodeType>()->nodes().at(index);
    }

    ABT& operator()(ABT& node) {
        return node.cast<NodeType>()->nodes().at(index);
    }

    size_t index = 0;
};

/**
 * Used to vend out fresh projection names. The method getNextId receives an optional prefix. If we
 * are generating descriptive names, the variable name we return starts with the prefix and includes
 * a prefix-specific counter. If we are not generating descriptive variable names, the prefix is
 * ignored and instead we use a global counter instead and ignore the prefix.
 */
class PrefixId {
    using IdType = uint64_t;
    using PrefixMapType = opt::unordered_map<std::string, IdType>;

public:
    static PrefixId create(const bool useDescriptiveVarNames) {
        return {useDescriptiveVarNames};
    }
    static PrefixId createForTests() {
        return {true /*useDescriptiveVarNames*/};
    }

    template <size_t N>
    ProjectionName getNextId(const char (&prefix)[N]) {
        return ProjectionName{visit(
            [&]<typename T>(T& v) -> std::string {
                using namespace fmt::literals;
                if constexpr (std::is_same_v<T, IdType>)
                    return "p{}"_format(v++);
                else if constexpr (std::is_same_v<T, PrefixMapType>)
                    return "{}_{}"_format(prefix, v[prefix]++);
            },
            _ids)};
    }

    PrefixId(const PrefixId& other) = delete;
    PrefixId(PrefixId&& other) = default;

    PrefixId& operator=(const PrefixId& other) = delete;
    PrefixId& operator=(PrefixId&& other) = default;

private:
    PrefixId(const bool useDescriptiveVarNames) {
        if (useDescriptiveVarNames) {
            _ids = {PrefixMapType{}};
        } else {
            _ids = {uint64_t{}};
        }
    }

    std::variant<IdType, PrefixMapType> _ids;
};

}  // namespace mongo::optimizer
