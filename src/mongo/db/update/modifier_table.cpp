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

#include "mongo/db/update/modifier_table.h"

#include "mongo/db/update/addtoset_node.h"
#include "mongo/db/update/arithmetic_node.h"
#include "mongo/db/update/bit_node.h"
#include "mongo/db/update/compare_node.h"
#include "mongo/db/update/conflict_placeholder_node.h"
#include "mongo/db/update/current_date_node.h"
#include "mongo/db/update/pop_node.h"
#include "mongo/db/update/pull_node.h"
#include "mongo/db/update/pullall_node.h"
#include "mongo/db/update/push_node.h"
#include "mongo/db/update/rename_node.h"
#include "mongo/db/update/set_node.h"
#include "mongo/db/update/unset_node.h"
#include "mongo/db/update/update_node.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>

namespace mongo::modifiertable {
namespace {
struct OrderByName {
    constexpr StringData lens(StringData x) const {
        return x;
    }
    template <typename... T>
    constexpr StringData lens(const std::pair<T...>& x) const {
        return lens(x.first);
    }

    template <typename A, typename B>
    constexpr bool operator()(const A& a, const B& b) const {
        return std::less<>{}(lens(a), lens(b));
    }
};

constexpr auto names = std::to_array<std::pair<StringData, ModifierType>>({
    {"$addToSet"_sd, MOD_ADD_TO_SET},
    {"$bit"_sd, MOD_BIT},
    {"$currentDate"_sd, MOD_CURRENTDATE},
    {"$inc"_sd, MOD_INC},
    {"$max"_sd, MOD_MAX},
    {"$min"_sd, MOD_MIN},
    {"$mul"_sd, MOD_MUL},
    {"$pop"_sd, MOD_POP},
    {"$pull"_sd, MOD_PULL},
    {"$pullAll"_sd, MOD_PULL_ALL},
    {"$push"_sd, MOD_PUSH},
    {"$rename"_sd, MOD_RENAME},
    {"$set"_sd, MOD_SET},
    {"$setOnInsert"_sd, MOD_SET_ON_INSERT},
    {"$unset"_sd, MOD_UNSET},
});
static_assert(std::is_sorted(names.begin(), names.end(), OrderByName{}), "Must be sorted by name");

}  // namespace

ModifierType getType(StringData typeStr) {
    auto [it1, it2] = std::equal_range(names.begin(), names.end(), typeStr, OrderByName{});
    return it1 == it2 ? MOD_UNKNOWN : it1->second;
}

std::unique_ptr<UpdateLeafNode> makeUpdateLeafNode(ModifierType modType) {
    switch (modType) {
        case MOD_ADD_TO_SET:
            return std::make_unique<AddToSetNode>();
        case MOD_BIT:
            return std::make_unique<BitNode>();
        case MOD_CONFLICT_PLACEHOLDER:
            return std::make_unique<ConflictPlaceholderNode>();
        case MOD_CURRENTDATE:
            return std::make_unique<CurrentDateNode>();
        case MOD_INC:
            return std::make_unique<ArithmeticNode>(ArithmeticNode::ArithmeticOp::kAdd);
        case MOD_MAX:
            return std::make_unique<CompareNode>(CompareNode::CompareMode::kMax);
        case MOD_MIN:
            return std::make_unique<CompareNode>(CompareNode::CompareMode::kMin);
        case MOD_MUL:
            return std::make_unique<ArithmeticNode>(ArithmeticNode::ArithmeticOp::kMultiply);
        case MOD_POP:
            return std::make_unique<PopNode>();
        case MOD_PULL:
            return std::make_unique<PullNode>();
        case MOD_PULL_ALL:
            return std::make_unique<PullAllNode>();
        case MOD_PUSH:
            return std::make_unique<PushNode>();
        case MOD_RENAME:
            return std::make_unique<RenameNode>();
        case MOD_SET:
            return std::make_unique<SetNode>();
        case MOD_SET_ON_INSERT:
            return std::make_unique<SetNode>(UpdateNode::Context::kInsertOnly);
        case MOD_UNSET:
            return std::make_unique<UnsetNode>();
        default:
            return nullptr;
    }
}

}  // namespace mongo::modifiertable
