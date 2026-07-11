// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/db/auth/action_type.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/public/extension_agg_stage_static_properties_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/util/modules.h"

/**
 * Utilities for mapping Extensions Public API enums to host types (e.g. ActionType,
 * StageConstraints, ViewPolicy).
 */
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

inline StageConstraints::StreamType toStreamType(MongoExtensionStreamTypeEnum streamType) {
    switch (streamType) {
        case MongoExtensionStreamTypeEnum::kStreaming:
            return StageConstraints::StreamType::kStreaming;
        case MongoExtensionStreamTypeEnum::kBlocking:
            return StageConstraints::StreamType::kBlocking;
    }
    MONGO_UNREACHABLE_TASSERT(12006800);
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
        case MongoExtensionHostTypeRequirementEnum::kReceivingHostOnly:
            return StageConstraints::HostTypeRequirement::kReceivingHostOnly;
        case MongoExtensionHostTypeRequirementEnum::kCollectionlessSourceRunOnceAnyNode:
            return StageConstraints::HostTypeRequirement::kCollectionlessSourceRunOnceAnyNode;
        case MongoExtensionHostTypeRequirementEnum::kTargetedShards:
            return StageConstraints::HostTypeRequirement::kTargetedShards;
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

namespace view_util {
inline FirstStageViewApplicationPolicy toFirstStageApplicationPolicy(
    MongoExtensionFirstStageViewApplicationPolicy policy) {
    switch (policy) {
        case MongoExtensionFirstStageViewApplicationPolicy::kDefaultPrepend:
            return FirstStageViewApplicationPolicy::kDefaultPrepend;
        case MongoExtensionFirstStageViewApplicationPolicy::kDoNothing:
            return FirstStageViewApplicationPolicy::kDoNothing;
    }
    MONGO_UNREACHABLE_TASSERT(11507600);
}
}  // namespace view_util

namespace descriptor_util {
inline AllowedWithClientType toAllowedWithClientType(MongoExtensionClientType clientType) {
    switch (clientType) {
        case MongoExtensionClientType::kMongoExtensionClientTypeAny:
            return AllowedWithClientType::kAny;
        case MongoExtensionClientType::kMongoExtensionClientTypeInternal:
            return AllowedWithClientType::kInternal;
    }
    MONGO_UNREACHABLE_TASSERT(11436401);
}
}  // namespace descriptor_util
}  // namespace mongo::extension::host
