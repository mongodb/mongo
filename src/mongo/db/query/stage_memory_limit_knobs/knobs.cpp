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

#include "mongo/db/query/stage_memory_limit_knobs/knobs_gen.h"

namespace mongo {

namespace {

AtomicWord<long long>& getMemoryLimitKnob(StageMemoryLimit stage) {
    switch (stage) {
        case StageMemoryLimit::DocumentSourceLookupCacheSizeBytes:
            return internalDocumentSourceLookupCacheSizeBytes;
        case StageMemoryLimit::DocumentSourceGraphLookupMaxMemoryBytes:
            return internalDocumentSourceGraphLookupMaxMemoryBytes;
        case StageMemoryLimit::DocumentSourceGroupMaxMemoryBytes:
            return internalDocumentSourceGroupMaxMemoryBytes;
        case StageMemoryLimit::DocumentSourceSetWindowFieldsMaxMemoryBytes:
            return internalDocumentSourceSetWindowFieldsMaxMemoryBytes;
        case StageMemoryLimit::DocumentSourceBucketAutoMaxMemoryBytes:
            return internalDocumentSourceBucketAutoMaxMemoryBytes;
        case StageMemoryLimit::DocumentSourceDensifyMaxMemoryBytes:
            return internalDocumentSourceDensifyMaxMemoryBytes;
        case StageMemoryLimit::QueryFacetBufferSizeBytes:
            return internalQueryFacetBufferSizeBytes;
        case StageMemoryLimit::TextOrStageMaxMemoryBytes:
            return internalTextOrStageMaxMemoryBytes;
        case StageMemoryLimit::QuerySBELookupApproxMemoryUseInBytesBeforeSpill:
            return internalQuerySBELookupApproxMemoryUseInBytesBeforeSpill;
        case StageMemoryLimit::QuerySBEAggApproxMemoryUseInBytesBeforeSpill:
            return internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill;
        case StageMemoryLimit::QueryMaxSpoolMemoryUsageBytes:
            return internalQueryMaxSpoolMemoryUsageBytes;
        case StageMemoryLimit::QueryMaxBlockingSortMemoryUsageBytes:
            return internalQueryMaxBlockingSortMemoryUsageBytes;
        case StageMemoryLimit::OrStageMaxMemoryBytes:
            return internalOrStageMaxMemoryBytes;
        case StageMemoryLimit::NearStageMaxMemoryBytes:
            return internalNearStageMaxMemoryBytes;
    };
    MONGO_UNREACHABLE_TASSERT(10869600);
}

}  // namespace

long long loadMemoryLimit(StageMemoryLimit stage) {
    return getMemoryLimitKnob(stage).loadRelaxed();
}

void appendStageMemoryLimitsToExplain(BSONObjBuilder& bob) {
    bob.appendNumber("internalQueryFacetBufferSizeBytes",
                     loadMemoryLimit(StageMemoryLimit::QueryFacetBufferSizeBytes));
    bob.appendNumber("internalDocumentSourceGroupMaxMemoryBytes",
                     loadMemoryLimit(StageMemoryLimit::DocumentSourceGroupMaxMemoryBytes));
    bob.appendNumber("internalQueryMaxBlockingSortMemoryUsageBytes",
                     loadMemoryLimit(StageMemoryLimit::QueryMaxBlockingSortMemoryUsageBytes));
    bob.appendNumber(
        "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
        loadMemoryLimit(StageMemoryLimit::DocumentSourceSetWindowFieldsMaxMemoryBytes));
}

}  // namespace mongo
