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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/lite_parsed_pipeline.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/stats/counters.h"

namespace mongo {

ReadConcernSupportResult LiteParsedPipeline::supportsReadConcern(
    repl::ReadConcernLevel level,
    bool isImplicitDefault,
    boost::optional<ExplainOptions::Verbosity> explain,
    bool enableMajorityReadConcern) const {
    // Start by assuming that we will support both readConcern and cluster-wide default.
    ReadConcernSupportResult result = ReadConcernSupportResult::allSupportedAndDefaultPermitted();

    // 1. Determine whether the given read concern must be rejected for any pipeline-global reasons.
    if (!hasChangeStream() && !enableMajorityReadConcern &&
        (level == repl::ReadConcernLevel::kMajorityReadConcern)) {
        // Reject non change stream aggregation queries that try to use "majority" read concern when
        // enableMajorityReadConcern=false.
        result.readConcernSupport = {
            ErrorCodes::ReadConcernMajorityNotEnabled,
            "Only change stream aggregation queries support 'majority' read concern when "
            "enableMajorityReadConcern=false"};
    } else if (explain && level != repl::ReadConcernLevel::kLocalReadConcern) {
        // Reject non-local read concern when the pipeline is being explained.
        result.readConcernSupport = {
            ErrorCodes::InvalidOptions,
            str::stream() << "Explain for the aggregate command cannot run with a readConcern "
                          << "other than 'local'. Current readConcern level: "
                          << repl::readConcernLevels::toString(level)};
    }

    // 2. Determine whether the default read concern must be denied for any pipeline-global reasons.
    if (explain) {
        result.defaultReadConcernPermit = {
            ErrorCodes::InvalidOptions,
            "Explain for the aggregate command does not permit default readConcern to be "
            "applied."};
    }

    // 3. If either the specified or default readConcern have not already been rejected, determine
    // whether the pipeline stages support them. If not, we record the first error we encounter.
    result.merge(sourcesSupportReadConcern(level, isImplicitDefault));

    return result;
}

ReadConcernSupportResult LiteParsedPipeline::sourcesSupportReadConcern(
    repl::ReadConcernLevel level, bool isImplicitDefault) const {
    // Start by assuming that we will support both readConcern and cluster-wide default.
    ReadConcernSupportResult result = ReadConcernSupportResult::allSupportedAndDefaultPermitted();

    for (auto&& spec : _stageSpecs) {
        // If both result statuses are already not OK, stop checking further stages.
        if (!result.readConcernSupport.isOK() && !result.defaultReadConcernPermit.isOK()) {
            break;
        }
        result.merge(spec->supportsReadConcern(level, isImplicitDefault));
    }

    return result;
}

void LiteParsedPipeline::assertSupportsMultiDocumentTransaction(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "Operation not permitted in transaction :: caused by :: Explain for the aggregate "
            "command cannot run within a multi-document transaction",
            !explain);

    for (auto&& spec : _stageSpecs) {
        spec->assertSupportsMultiDocumentTransaction();
    }
}

void LiteParsedPipeline::assertSupportsReadConcern(
    OperationContext* opCtx, boost::optional<ExplainOptions::Verbosity> explain) const {
    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    auto readConcernSupport = supportsReadConcern(readConcernArgs.getLevel(),
                                                  readConcernArgs.isImplicitDefault(),
                                                  explain,
                                                  serverGlobalParams.enableMajorityReadConcern);
    if (readConcernArgs.hasLevel()) {
        if (!readConcernSupport.readConcernSupport.isOK()) {
            uassertStatusOK(readConcernSupport.readConcernSupport.withContext(
                "Operation does not support this transaction's read concern"));
        }
    }
}

void LiteParsedPipeline::verifyIsSupported(
    OperationContext* opCtx,
    const std::function<bool(OperationContext*, const NamespaceString&)> isSharded,
    const boost::optional<ExplainOptions::Verbosity> explain,
    bool enableMajorityReadConcern) const {
    // Verify litePipe can be run in a transaction.
    const bool inMultiDocumentTransaction = opCtx->inMultiDocumentTransaction();
    if (inMultiDocumentTransaction) {
        assertSupportsMultiDocumentTransaction(explain);
        assertSupportsReadConcern(opCtx, explain);
    }
    // Verify that no involved namespace is sharded unless allowed by the pipeline.
    for (const auto& nss : getInvolvedNamespaces()) {
        uassert(28769,
                str::stream() << nss.toStringForErrorMsg() << " cannot be sharded",
                allowShardedForeignCollection(nss, inMultiDocumentTransaction) ||
                    !isSharded(opCtx, nss));
    }
}

void LiteParsedPipeline::tickGlobalStageCounters() const {
    for (auto&& stage : _stageSpecs) {
        // Tick counter corresponding to current stage.
        aggStageCounters.stageCounterMap.find(stage->getParseTimeName())
            ->second->counter.increment(1);

        // Recursively step through any sub-pipelines.
        for (auto&& subPipeline : stage->getSubPipelines()) {
            subPipeline.tickGlobalStageCounters();
        }
    }
}

void LiteParsedPipeline::validate(const OperationContext* opCtx,
                                  bool performApiVersionChecks) const {

    for (auto&& stage : _stageSpecs) {
        const auto& stageName = stage->getParseTimeName();
        const auto& stageInfo = LiteParsedDocumentSource::getInfo(stageName);

        // Validate that the stage is API version compatible.
        if (performApiVersionChecks) {

            std::function<void(const APIParameters&)> sometimesCallback =
                [&](const APIParameters& apiParameters) {
                    tassert(5807600,
                            "Expected callback only if allowed 'sometimes'",
                            stageInfo.allowedWithApiStrict == AllowedWithApiStrict::kConditionally);
                    stage->assertPermittedInAPIVersion(apiParameters);
                };
            assertLanguageFeatureIsAllowed(opCtx,
                                           stageName,
                                           stageInfo.allowedWithApiStrict,
                                           stageInfo.allowedWithClientType,
                                           sometimesCallback);
        }

        for (auto&& subPipeline : stage->getSubPipelines()) {
            subPipeline.validate(opCtx, performApiVersionChecks);
        }
    }
}

}  // namespace mongo
