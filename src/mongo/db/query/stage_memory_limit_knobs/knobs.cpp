// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"

#include "mongo/db/query/stage_memory_limit_knobs/query_knob_descriptors.h"
#include "mongo/db/query/stage_memory_limit_knobs/stage_memory_limit_knobs_gen.h"

namespace mongo {

namespace {

const QueryKnob<long long>& getMemoryLimitKnob(StageMemoryLimit stage) {
    switch (stage) {
        case StageMemoryLimit::DocumentSourceLookupCacheSizeBytes:
            return query_knobs::kDocumentSourceLookupCacheSizeBytes;
        case StageMemoryLimit::DocumentSourceGraphLookupMaxMemoryBytes:
            return query_knobs::kDocumentSourceGraphLookupMaxMemoryBytes;
        case StageMemoryLimit::DocumentSourceGroupMaxMemoryBytes:
            return query_knobs::kDocumentSourceGroupMaxMemoryBytes;
        case StageMemoryLimit::DocumentSourceSetWindowFieldsMaxMemoryBytes:
            return query_knobs::kDocumentSourceSetWindowFieldsMaxMemoryBytes;
        case StageMemoryLimit::DocumentSourceBucketAutoMaxMemoryBytes:
            return query_knobs::kDocumentSourceBucketAutoMaxMemoryBytes;
        case StageMemoryLimit::DocumentSourceDensifyMaxMemoryBytes:
            return query_knobs::kDocumentSourceDensifyMaxMemoryBytes;
        case StageMemoryLimit::QueryFacetBufferSizeBytes:
            return query_knobs::kQueryFacetBufferSizeBytes;
        case StageMemoryLimit::TextOrStageMaxMemoryBytes:
            return query_knobs::kTextOrStageMaxMemoryBytes;
        case StageMemoryLimit::QuerySBELookupApproxMemoryUseInBytesBeforeSpill:
            return query_knobs::kSbeHashLookupApproxMemoryUseInBytesBeforeSpill;
        case StageMemoryLimit::QuerySBEAggApproxMemoryUseInBytesBeforeSpill:
            return query_knobs::kSbeHashAggApproxMemoryUseInBytesBeforeSpill;
        case StageMemoryLimit::QueryMaxSpoolMemoryUsageBytes:
            return query_knobs::kQueryMaxSpoolMemoryUsageBytes;
        case StageMemoryLimit::QueryMaxBlockingSortMemoryUsageBytes:
            return query_knobs::kQueryMaxBlockingSortMemoryUsageBytes;
        case StageMemoryLimit::OrStageMaxMemoryBytes:
            return query_knobs::kOrStageMaxMemoryBytes;
        case StageMemoryLimit::NearStageMaxMemoryBytes:
            return query_knobs::kNearStageMaxMemoryBytes;
        case StageMemoryLimit::MergeSortStageMaxMemoryBytes:
            return query_knobs::kMergeSortStageMaxMemoryBytes;
        case StageMemoryLimit::IndexScanStageMaxMemoryBytes:
            return query_knobs::kIndexScanStageMaxMemoryBytes;
        case StageMemoryLimit::SBEUniqueStageMaxMemoryBytes:
            return query_knobs::kSbeUniqueStageMaxMemoryBytes;
        case StageMemoryLimit::QuerySBEHashJoinApproxMemoryUseInBytesBeforeSpill:
            return query_knobs::kSbeHashJoinApproxMemoryUseInBytesBeforeSpill;
        case StageMemoryLimit::UpdateStageMaxMemoryBytes:
            return query_knobs::kUpdateStageMaxMemoryBytes;
        case StageMemoryLimit::CountScanStageMaxMemoryBytes:
            return query_knobs::kCountScanStageMaxMemoryBytes;
        case StageMemoryLimit::SBEMergeJoinStageMaxMemoryBytes:
            return query_knobs::kSbeMergeJoinStageMaxMemoryBytes;
        case StageMemoryLimit::SBEAndHashStageMaxMemoryBytes:
            return query_knobs::kSbeAndHashStageMaxMemoryBytes;
    };
    MONGO_UNREACHABLE_TASSERT(10869600);
}

}  // namespace

MemoryUsageLimit loadMemoryLimit(StageMemoryLimit stage) {
    return MemoryUsageLimit{getMemoryLimitKnob(stage)};
}

}  // namespace mongo
