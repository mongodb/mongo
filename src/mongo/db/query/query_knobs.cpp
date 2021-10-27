
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

#include "mongo/db/query/query_knobs.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"

namespace mongo {

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlanEvaluationWorks, int, 10000);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlanEvaluationCollFraction, double, 0.3);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlanEvaluationMaxResults, int, 101);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryCacheSize, int, 5000);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryCacheFeedbacksStored, int, 20);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryCacheMaxSizeBytesBeforeStripDebugInfo,
                              long long,
                              512 * 1024 * 1024)
    ->withValidator([](const long long& newVal) {
        if (newVal < 0) {
            return Status(
                ErrorCodes::Error(4036100),
                "internalQueryCacheMaxSizeBytesBeforeStripDebugInfo must be non-negative");
        }
        return Status::OK();
    });

MONGO_EXPORT_SERVER_PARAMETER(internalQueryCacheEvictionRatio, double, 10.0);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlannerMaxIndexedSolutions, int, 64);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryEnumerationPreferLockstepOrEnumeration, bool, false);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryEnumerationMaxOrSolutions, int, 10);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryEnumerationMaxIntersectPerAnd, int, 3);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryForceIntersectionPlans, bool, false);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlannerEnableIndexIntersection, bool, true);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlannerEnableHashIntersection, bool, false);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlanOrChildrenIndependently, bool, true);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryMaxScansToExplode, int, 200);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryExecMaxBlockingSortBytes, int, 32 * 1024 * 1024);

// Yield every 128 cycles or 10ms.
MONGO_EXPORT_SERVER_PARAMETER(internalQueryExecYieldIterations, int, 128);
MONGO_EXPORT_SERVER_PARAMETER(internalQueryExecYieldPeriodMS, int, 10);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryFacetBufferSizeBytes, int, 100 * 1024 * 1024);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryFacetMaxOutputDocSizeBytes, long long, 100 * 1024 * 1024)
    ->withValidator([](const long long& newVal) {
        if (newVal <= 0) {
            return Status(ErrorCodes::BadValue,
                          "internalQueryFacetMaxOutputDocSizeBytes must be positive");
        }
        return Status::OK();
    });

MONGO_EXPORT_SERVER_PARAMETER(internalLookupStageIntermediateDocumentMaxSizeBytes,
                              long long,
                              100 * 1024 * 1024)
    ->withValidator([](const long long& newVal) {
        if (newVal < BSONObjMaxInternalSize) {
            return Status(ErrorCodes::BadValue,
                          "internalLookupStageIntermediateDocumentMaxSizeBytes must be >= " +
                              std::to_string(BSONObjMaxInternalSize));
        }
        return Status::OK();
    });

MONGO_EXPORT_SERVER_PARAMETER(internalInsertMaxBatchSize,
                              int,
                              internalQueryExecYieldIterations.load() / 2);

MONGO_EXPORT_SERVER_PARAMETER(internalDocumentSourceCursorBatchSizeBytes, int, 4 * 1024 * 1024);

MONGO_EXPORT_SERVER_PARAMETER(internalDocumentSourceLookupCacheSizeBytes, int, 100 * 1024 * 1024);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlannerGenerateCoveredWholeIndexScans, bool, false);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryIgnoreUnknownJSONSchemaKeywords, bool, false);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryProhibitBlockingMergeOnMongoS, bool, false);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryMaxPushBytes, int, 100 * 1024 * 1024)
    ->withValidator([](const int& newVal) {
        if (newVal <= 0) {
            return Status(ErrorCodes::BadValue, "internalQueryMaxPushBytes must be positive");
        }
        return Status::OK();
    });

MONGO_EXPORT_SERVER_PARAMETER(internalQueryMaxAddToSetBytes, int, 100 * 1024 * 1024)
    ->withValidator([](const int& newVal) {
        if (newVal <= 0) {
            return Status(ErrorCodes::BadValue, "internalQueryMaxAddToSetBytes must be positive");
        }
        return Status::OK();
    });

MONGO_EXPORT_SERVER_PARAMETER(internalQueryMaxRangeBytes, int, 100 * 1024 * 1024)
    ->withValidator([](const int& newVal) {
        if (newVal <= 0) {
            return Status(ErrorCodes::BadValue, "internalQueryMaxRangeBytes must be positive");
        }
        return Status::OK();
    });

MONGO_EXPORT_SERVER_PARAMETER(internalQueryExplainSizeThresholdBytes, int, 10 * 1024 * 1024)
    ->withValidator([](const int& newVal) {
        if (newVal <= 0) {
            return Status(ErrorCodes::BadValue,
                          "internalQueryExplainSizeThresholdBytes must be positive");
        } else if (newVal > BSONObjMaxInternalSize) {
            return Status(ErrorCodes::BadValue,
                          "internalQueryExplainSizeThresholdBytes cannot exceed max BSON size");
        } else {
            return Status::OK();
        }
    });
}  // namespace mongo
