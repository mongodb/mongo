/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/query_request_conversion.h"

#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"

namespace mongo {

namespace query_request_conversion {

AggregateCommandRequest asAggregateCommandRequest(const FindCommandRequest& findCommand) {
    // First, check if this query has options that are not supported in aggregation.
    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Option " << FindCommandRequest::kMinFieldName
                          << " not supported in aggregation.",
            findCommand.getMin().isEmpty());
    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Option " << FindCommandRequest::kMaxFieldName
                          << " not supported in aggregation.",
            findCommand.getMax().isEmpty());
    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Option " << FindCommandRequest::kReturnKeyFieldName
                          << " not supported in aggregation.",
            !findCommand.getReturnKey());
    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Option " << FindCommandRequest::kShowRecordIdFieldName
                          << " not supported in aggregation.",
            !findCommand.getShowRecordId());
    uassert(ErrorCodes::InvalidPipelineOperator,
            "Tailable cursors are not supported in aggregation.",
            !findCommand.getTailable());
    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Option " << FindCommandRequest::kNoCursorTimeoutFieldName
                          << " not supported in aggregation.",
            !findCommand.getNoCursorTimeout());
    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Option " << FindCommandRequest::kAllowPartialResultsFieldName
                          << " not supported in aggregation.",
            !findCommand.getAllowPartialResults());
    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Sort option " << query_request_helper::kNaturalSortField
                          << " not supported in aggregation.",
            !findCommand.getSort()[query_request_helper::kNaturalSortField]);

    // The aggregation command normally does not support the 'singleBatch' option, but we make a
    // special exception if 'limit' is set to 1.
    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Option " << FindCommandRequest::kSingleBatchFieldName
                          << " not supported in aggregation.",
            !findCommand.getSingleBatch() || findCommand.getLimit().value_or(0) == 1LL);
    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Option " << FindCommandRequest::kReadOnceFieldName
                          << " not supported in aggregation.",
            !findCommand.getReadOnce());

    uassert(ErrorCodes::InvalidPipelineOperator,
            str::stream() << "Option " << FindCommandRequest::kAllowSpeculativeMajorityReadFieldName
                          << " not supported in aggregation.",
            !findCommand.getAllowSpeculativeMajorityRead());

    // Some options are disallowed when resharding improvements are disabled.
    if (!resharding::gFeatureFlagReshardingImprovements.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        uassert(ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kRequestResumeTokenFieldName
                              << " not supported in aggregation.",
                !findCommand.getRequestResumeToken());

        uassert(ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kResumeAfterFieldName
                              << " not supported in aggregation.",
                findCommand.getResumeAfter().isEmpty());
    }

    // Now that we've successfully validated this QR, begin building the aggregation command.
    tassert(ErrorCodes::BadValue,
            "Unsupported type UUID for namspace",
            findCommand.getNamespaceOrUUID().isNamespaceString());
    AggregateCommandRequest result{findCommand.getNamespaceOrUUID().nss(),
                                   findCommand.getSerializationContext()};

    // Construct an aggregation pipeline that finds the equivalent documents to this query.
    std::vector<BSONObj> pipeline;
    if (!findCommand.getFilter().isEmpty()) {
        pipeline.push_back(BSON("$match" << findCommand.getFilter()));
    }
    if (!findCommand.getSort().isEmpty()) {
        pipeline.push_back(BSON("$sort" << findCommand.getSort()));
    }
    if (findCommand.getSkip()) {
        pipeline.push_back(BSON("$skip" << *findCommand.getSkip()));
    }
    if (findCommand.getLimit()) {
        pipeline.push_back(BSON("$limit" << *findCommand.getLimit()));
    }
    if (!findCommand.getProjection().isEmpty()) {
        pipeline.push_back(BSON("$project" << findCommand.getProjection()));
    }
    result.setPipeline(std::move(pipeline));

    // The aggregation 'cursor' option is always set, regardless of the presence of batchSize.
    SimpleCursorOptions cursor;
    if (auto batchSize = findCommand.getBatchSize()) {
        // If the find command specifies `singleBatch`, 'limit' is required to be 1 (as checked
        // above). If 'batchSize' is also 1, an open cursor will be returned, contradicting the
        // 'singleBatch' option. We set 'batchSize' to 2 as a workaround to ensure no cursor is
        // returned.
        // TODO SERVER-83077 This workaround will be unnecessary if a full batch of size 1 doesn't
        // open a cursor.
        if (findCommand.getSingleBatch() && *batchSize == 1LL) {
            cursor.setBatchSize(2);
        } else {
            cursor.setBatchSize(*batchSize);
        }
    }
    result.setCursor(std::move(cursor));

    // Other options.
    if (!findCommand.getCollation().isEmpty()) {
        result.setCollation(findCommand.getCollation().getOwned());
    }
    if (auto maxTimeMS = findCommand.getMaxTimeMS(); maxTimeMS.has_value() && *maxTimeMS > 0) {
        result.setMaxTimeMS(*maxTimeMS);
    }
    if (!findCommand.getHint().isEmpty()) {
        result.setHint(findCommand.getHint().getOwned());
    }
    if (findCommand.getQuerySettings()) {
        result.setQuerySettings(findCommand.getQuerySettings());
    }
    if (findCommand.getReadConcern()) {
        result.setReadConcern(findCommand.getReadConcern()->getOwned());
    }
    if (!findCommand.getUnwrappedReadPref().isEmpty()) {
        result.setUnwrappedReadPref(findCommand.getUnwrappedReadPref().getOwned());
    }
    if (findCommand.getAllowDiskUse().has_value()) {
        result.setAllowDiskUse(findCommand.getAllowDiskUse());
    }
    result.setLegacyRuntimeConstants(findCommand.getLegacyRuntimeConstants());
    if (findCommand.getLet()) {
        result.setLet(findCommand.getLet()->getOwned());
    }
    if (resharding::gFeatureFlagReshardingImprovements.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        result.setRequestResumeToken(findCommand.getRequestResumeToken());

        if (!findCommand.getResumeAfter().isEmpty()) {
            result.setResumeAfter(findCommand.getResumeAfter().getOwned());
        }
    }
    result.setIncludeQueryStatsMetrics(findCommand.getIncludeQueryStatsMetrics());

    return result;
}

}  // namespace query_request_conversion
}  // namespace mongo
