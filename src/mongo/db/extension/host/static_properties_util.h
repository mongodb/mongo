/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/auth/action_type.h"
#include "mongo/db/extension/public/extension_agg_stage_static_properties_gen.h"
#include "mongo/db/pipeline/stage_constraints.h"

namespace mongo::extension::host {
namespace static_properties_util {

inline ActionType toActionType(const MongoExtensionPrivilegeActionEnum& action) {
    switch (action) {
        case MongoExtensionPrivilegeActionEnum::kFind:
            return ActionType::find;
        case MongoExtensionPrivilegeActionEnum::kListIndexes:
            return ActionType::listIndexes;
        case MongoExtensionPrivilegeActionEnum::kListSearchIndexes:
            return ActionType::listSearchIndexes;
        case MongoExtensionPrivilegeActionEnum::kPlanCacheRead:
            return ActionType::planCacheRead;
        case MongoExtensionPrivilegeActionEnum::kCollStats:
            return ActionType::collStats;
        case MongoExtensionPrivilegeActionEnum::kIndexStats:
            return ActionType::indexStats;
        default:
            MONGO_UNREACHABLE_TASSERT(11350601);
    }
}

inline boost::optional<StageConstraints::PositionRequirement> toPositionRequirement(
    MongoExtensionPositionRequirementEnum pos) {
    switch (pos) {
        case MongoExtensionPositionRequirementEnum::kFirst:
            return StageConstraints::PositionRequirement::kFirst;
        case MongoExtensionPositionRequirementEnum::kLast:
            return StageConstraints::PositionRequirement::kLast;
        case MongoExtensionPositionRequirementEnum::kNone:
            return boost::none;
    }
    MONGO_UNREACHABLE_TASSERT(11376900);
}

inline boost::optional<StageConstraints::HostTypeRequirement> toHostTypeRequirement(
    MongoExtensionHostTypeRequirementEnum host) {
    switch (host) {
        case MongoExtensionHostTypeRequirementEnum::kLocalOnly:
            return StageConstraints::HostTypeRequirement::kLocalOnly;
        case MongoExtensionHostTypeRequirementEnum::kRunOnceAnyNode:
            return StageConstraints::HostTypeRequirement::kRunOnceAnyNode;
        case MongoExtensionHostTypeRequirementEnum::kAnyShard:
            return StageConstraints::HostTypeRequirement::kAnyShard;
        case MongoExtensionHostTypeRequirementEnum::kRouter:
            return StageConstraints::HostTypeRequirement::kRouter;
        case MongoExtensionHostTypeRequirementEnum::kAllShardHosts:
            return StageConstraints::HostTypeRequirement::kAllShardHosts;
        case MongoExtensionHostTypeRequirementEnum::kNone:
            return boost::none;
    }
    MONGO_UNREACHABLE_TASSERT(11376901);
}

}  // namespace static_properties_util
}  // namespace mongo::extension::host
