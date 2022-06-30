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

#include <boost/optional.hpp>
#include <memory>
#include <numeric>

#include "mongo/db/cst/c_node_disambiguation.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::c_node_disambiguation {
namespace {

ProjectionType disambiguateCNode(const CNode& cst) {
    return stdx::visit(
        OverloadedVisitor{
            [](const CNode::ObjectChildren& children) {
                return *std::accumulate(
                    children.begin(),
                    children.end(),
                    boost::optional<ProjectionType>{},
                    [](auto&& currentProjType, auto&& child) {
                        const auto seenProjType =
                            stdx::holds_alternative<FieldnamePath>(child.first)
                            // This is part of the compound key and must be explored.
                            ? disambiguateCNode(child.second)
                            // This is an arbitrary expression to produce a computed field.
                            : ProjectionType::inclusion;
                        if (!currentProjType)
                            return seenProjType;
                        else if (*currentProjType != seenProjType)
                            return ProjectionType::inconsistent;
                        else
                            return *currentProjType;
                    });
            },
            [&](auto&&) {
                if (auto type = cst.projectionType())
                    // This is a key which indicates the projection type.
                    return *type;
                else
                    // This is a value which will produce a computed field.
                    return ProjectionType::inclusion;
            }},
        cst.payload);
}

}  // namespace

CNode disambiguateCompoundProjection(CNode project) {
    switch (disambiguateCNode(project)) {
        case ProjectionType::inclusion:
            return CNode{CompoundInclusionKey{std::make_unique<CNode>(std::move(project))}};
        case ProjectionType::exclusion:
            return CNode{CompoundExclusionKey{std::make_unique<CNode>(std::move(project))}};
        case ProjectionType::inconsistent:
            return CNode{CompoundInconsistentKey{std::make_unique<CNode>(std::move(project))}};
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo::c_node_disambiguation
