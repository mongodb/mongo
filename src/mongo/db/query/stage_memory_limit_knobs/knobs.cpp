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

#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"

#include "mongo/db/query/stage_memory_limit_knobs/query_knob_descriptors.h"
#include "mongo/db/query/stage_memory_limit_knobs/stage_memory_limit_knobs_gen.h"

#include <string_view>

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

void appendStageMemoryLimitsToExplain(BSONObjBuilder& bob) {
    // 'appendNumber' has no 'int64_t'-exact overload (on platforms where 'int64_t' is 'long' it is
    // ambiguous between the 'int' and 'long long' overloads), so cast the resolved value.
    auto append = [&](std::string_view name, StageMemoryLimit stage) {
        bob.appendNumber(name, static_cast<long long>(loadMemoryLimit(stage).get()));
    };
    append("internalQueryFacetBufferSizeBytes", StageMemoryLimit::QueryFacetBufferSizeBytes);
    append("internalDocumentSourceGroupMaxMemoryBytes",
           StageMemoryLimit::DocumentSourceGroupMaxMemoryBytes);
    append("internalQueryMaxBlockingSortMemoryUsageBytes",
           StageMemoryLimit::QueryMaxBlockingSortMemoryUsageBytes);
    append("internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
           StageMemoryLimit::DocumentSourceSetWindowFieldsMaxMemoryBytes);
}

}  // namespace mongo
