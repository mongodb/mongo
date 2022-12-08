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

#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/utils/utils.h"


namespace mongo::optimizer {

template <class ToAddType, class ToRemoveType>
static void addRemoveProjectionsToProperties(properties::PhysProps& properties,
                                             const ToAddType& toAdd,
                                             const ToRemoveType& toRemove) {
    ProjectionNameOrderPreservingSet& projections =
        properties::getProperty<properties::ProjectionRequirement>(properties).getProjections();
    for (const auto& varName : toRemove) {
        projections.erase(varName);
    }
    for (const auto& varName : toAdd) {
        projections.emplace_back(varName);
    }
}

template <class ToAddType>
static void addProjectionsToProperties(properties::PhysProps& properties, const ToAddType& toAdd) {
    addRemoveProjectionsToProperties(properties, toAdd, ToAddType{});
}

/**
 * Extracts the "latest" logical plan. Starting from the root group, we follow the last logical
 * nodes.
 */
ABT extractLatestPlan(const cascades::Memo& memo, GroupIdType rootGroupId);

/**
 * Extracts a complete physical plan by inlining references to MemoPhysicalPlanNode.
 */
std::pair<ABT, NodeToGroupPropsMap> extractPhysicalPlan(MemoPhysicalNodeId id,
                                                        const Metadata& metadata,
                                                        const RIDProjectionsMap& ridProjections,
                                                        const cascades::Memo& memo);

}  // namespace mongo::optimizer
