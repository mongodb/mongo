/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <algorithm>
#include <boost/optional.hpp>
#include <iterator>
#include <memory>
#include <utility>

#include "mongo/db/cst/c_node_disambiguation.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/visit_helper.h"

namespace mongo::c_node_disambiguation {
namespace {

enum class ProjectionType : char { inclusion, exclusion, inconsistent };

boost::optional<CNode> replaceCNode(const CNode& cst,
                                    boost::optional<ProjectionType>& currentProjType) {
    auto updateProjectionTypeCreateReplacement =
        [&](auto&& isExclusion, auto&& makeExclusive, auto&& makeInclusive) {
            auto seenProjType = isExclusion ? ProjectionType::exclusion : ProjectionType::inclusion;
            if (!currentProjType)
                currentProjType = seenProjType;
            else if (*currentProjType != seenProjType)
                currentProjType = ProjectionType::inconsistent;
            return boost::make_optional(isExclusion ? makeExclusive() : makeInclusive());
        };

    // This is done without mutation so we can cancel the operation without side effects.
    return stdx::visit(
        visit_helper::Overloaded{
            [&](const CNode::ObjectChildren& children) -> boost::optional<CNode> {
                auto newChildren = CNode::ObjectChildren{};
                for (auto&& [fieldname, fieldValue] : children)
                    if (!stdx::holds_alternative<UserFieldname>(fieldname))
                        return boost::none;
                    else if (auto newNode = replaceCNode(fieldValue, currentProjType); !newNode)
                        return boost::none;
                    else
                        newChildren.emplace_back(fieldname, *newNode);
                return CNode{std::move(newChildren)};
            },
            [&](const UserInt& userInt) {
                return updateProjectionTypeCreateReplacement(
                    userInt == UserInt{0},
                    [] { return CNode{KeyValue::intZeroKey}; },
                    [&] { return CNode{NonZeroKey{userInt}}; });
            },
            [&](const UserLong& userLong) {
                return updateProjectionTypeCreateReplacement(
                    userLong == UserLong{0ll},
                    [] { return CNode{KeyValue::longZeroKey}; },
                    [&] { return CNode{NonZeroKey{userLong}}; });
            },
            [&](const UserDouble& userDouble) {
                return updateProjectionTypeCreateReplacement(
                    userDouble == UserDouble{0.0},
                    [] { return CNode{KeyValue::doubleZeroKey}; },
                    [&] { return CNode{NonZeroKey{userDouble}}; });
            },
            [&](const UserDecimal& userDecimal) {
                return updateProjectionTypeCreateReplacement(
                    userDecimal == UserDecimal{0.0},
                    [] { return CNode{KeyValue::decimalZeroKey}; },
                    [&] { return CNode{NonZeroKey{userDecimal}}; });
            },
            [&](const UserBoolean& userBoolean) {
                return updateProjectionTypeCreateReplacement(
                    userBoolean == UserBoolean{false},
                    [] { return CNode{KeyValue::falseKey}; },
                    [] { return CNode{KeyValue::trueKey}; });
            },
            [&](auto &&) -> boost::optional<CNode> { return boost::none; }},
        cst.payload);
}

}  // namespace

CNode disambiguateCompoundProjection(CNode project) {
    auto projectionType = boost::optional<ProjectionType>{};
    auto cNode = replaceCNode(project, projectionType);
    if (!cNode)
        return std::move(project);
    switch (*projectionType) {
        case ProjectionType::inclusion:
            return CNode{CompoundInclusionKey{std::make_unique<CNode>(*std::move(cNode))}};
        case ProjectionType::exclusion:
            return CNode{CompoundExclusionKey{std::make_unique<CNode>(*std::move(cNode))}};
        case ProjectionType::inconsistent:
            return CNode{CompoundInconsistentKey{std::make_unique<CNode>(*std::move(cNode))}};
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo::c_node_disambiguation
