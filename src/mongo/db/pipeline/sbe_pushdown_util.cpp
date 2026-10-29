// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/sbe_pushdown_util.h"

#include "mongo/db/pipeline/document_source_group_base.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
SbeCompatibility getMinRequiredSbeCompatibility(QueryFrameworkControlEnum currentQueryKnobFramework,
                                                bool sbeFullEnabled) {
    if (sbeFullEnabled) {
        return SbeCompatibility::requiresTrySbe;
    } else if (currentQueryKnobFramework == QueryFrameworkControlEnum::kTrySbeEngine) {
        return SbeCompatibility::requiresTrySbe;
    }
    return SbeCompatibility::noRequirements;
}

bool isGroupSbeCompatible(const DocumentSourceGroupBase& group,
                          SbeCompatibility minRequiredCompatibility) {
    return !group.doingMerge() && group.sbeCompatibility() >= minRequiredCompatibility;
}
}  // namespace mongo
