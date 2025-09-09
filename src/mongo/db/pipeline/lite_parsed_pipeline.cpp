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

#include "mongo/db/pipeline/lite_parsed_pipeline.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <boost/optional/optional.hpp>

namespace mongo {

ReadConcernSupportResult LiteParsedPipeline::supportsReadConcern(repl::ReadConcernLevel level,
                                                                 bool isImplicitDefault,
                                                                 bool explain) const {
    // Start by assuming that we will support both readConcern and cluster-wide default.
    ReadConcernSupportResult result = ReadConcernSupportResult::allSupportedAndDefaultPermitted();
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

void LiteParsedPipeline::assertSupportsMultiDocumentTransaction(bool explain) const {
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "Operation not permitted in transaction :: caused by :: Explain for the aggregate "
            "command cannot run within a multi-document transaction",
            !explain);

    for (auto&& spec : _stageSpecs) {
        spec->assertSupportsMultiDocumentTransaction();
    }
}

void LiteParsedPipeline::assertSupportsReadConcern(OperationContext* opCtx, bool explain) const {
    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    auto readConcernSupport = supportsReadConcern(
        readConcernArgs.getLevel(), readConcernArgs.isImplicitDefault(), explain);
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
    bool explain) const {
    // Verify litePipe can be run in a transaction.
    const bool inMultiDocumentTransaction = opCtx->inMultiDocumentTransaction();
    if (inMultiDocumentTransaction) {
        assertSupportsMultiDocumentTransaction(explain);
        assertSupportsReadConcern(opCtx, explain);
    }
    // Verify that no involved namespace is sharded unless allowed by the pipeline.
    for (const auto& nss : getInvolvedNamespaces()) {
        const auto status = checkShardedForeignCollAllowed(nss, inMultiDocumentTransaction);
        uassert(status.code(),
                str::stream() << nss.toStringForErrorMsg()
                              << " cannot be sharded: " << status.reason(),
                status.isOK() || !isSharded(opCtx, nss));
    }
}

void LiteParsedPipeline::tickGlobalStageCounters() const {
    for (auto&& stage : _stageSpecs) {
        // Tick counter corresponding to current stage.
        aggStageCounters.increment(stage->getParseTimeName(), 1);
        // Recursively step through any sub-pipelines.
        for (auto&& subPipeline : stage->getSubPipelines()) {
            subPipeline.tickGlobalStageCounters();
        }
    }
}

void LiteParsedPipeline::validate(const OperationContext* opCtx,
                                  bool performApiVersionChecks) const {
    for (auto stage_it = _stageSpecs.begin(); stage_it != _stageSpecs.end(); stage_it++) {
        const auto& stage = *stage_it;
        // TODO SERVER-101722: Re-implement this validation with a more generic
        // StageConstraints-like validation.
        uassert(10170100,
                "$rankFusion/$scoreFusion can only be the first stage of an aggregation pipeline.",
                !((stage_it != _stageSpecs.begin()) && stage->isHybridSearchStage() &&
                  !isRunningAgainstView_ForHybridSearch()));

        const auto& stageName = (*stage_it)->getParseTimeName();
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

void LiteParsedPipeline::checkStagesAllowedInViewDefinition() const {
    for (auto stage_it = _stageSpecs.begin(); stage_it != _stageSpecs.end(); stage_it++) {
        const auto& stage = *stage_it;

        uassert(ErrorCodes::OptionNotSupportedOnView,
                "$rankFusion and $scoreFusion is currently unsupported in a view definition",
                !stage->isHybridSearchStage());

        uassert(ErrorCodes::OptionNotSupportedOnView,
                "$score is currently unsupported in a view definition",
                !(stage->getParseTimeName() == "$score"));

        for (auto&& subPipeline : stage->getSubPipelines()) {
            subPipeline.checkStagesAllowedInViewDefinition();
        }
    }
}

}  // namespace mongo
