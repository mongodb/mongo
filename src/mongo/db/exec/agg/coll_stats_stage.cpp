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

#include "mongo/db/exec/agg/coll_stats_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/version_context.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/time_support.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceCollStatsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* collStatsDS = dynamic_cast<DocumentSourceCollStats*>(documentSource.get());

    tassert(10812301, "expected 'DocumentSourceCollStats' type", collStatsDS);

    // TODO SERVER-105521: Check if '_collStatsSpec' can be moved instead of copied.
    return make_intrusive<exec::agg::CollStatsStage>(
        collStatsDS->kStageName, collStatsDS->getExpCtx(), collStatsDS->_collStatsSpec);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(collStats, DocumentSourceCollStats::id, documentSourceCollStatsToStageFn)

CollStatsStage::CollStatsStage(StringData stageName,
                               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                               DocumentSourceCollStatsSpec collStatsSpec)
    : Stage(stageName, pExpCtx), _collStatsSpec(std::move(collStatsSpec)) {}

GetNextResult CollStatsStage::doGetNext() {
    if (_finished) {
        return GetNextResult::makeEOF();
    }

    _finished = true;

    return {Document(makeStatsForNs(pExpCtx, pExpCtx->getNamespaceString(), _collStatsSpec))};
}

BSONObj CollStatsStage::makeStatsForNs(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       const NamespaceString& nss,
                                       const DocumentSourceCollStatsSpec& spec,
                                       const boost::optional<BSONObj>& filterObj) {
    // The $collStats stage is critical to observability and diagnosability, categorize as immediate
    // priority.
    ScopedAdmissionPriority<ExecutionAdmissionContext> skipAdmissionControl(
        expCtx->getOperationContext(), AdmissionContext::Priority::kExempt);

    BSONObjBuilder builder;

    // We need to use the serialization context from the request when calling
    // NamespaceStringUtil to build the reply.
    builder.append(
        "ns",
        NamespaceStringUtil::serialize(
            nss, SerializationContext::stateCommandReply(spec.getSerializationContext())));

    auto shardName =
        expCtx->getMongoProcessInterface()->getShardName(expCtx->getOperationContext());

    if (!shardName.empty()) {
        builder.append("shard", shardName);
    }

    builder.append(
        "host", prettyHostNameAndPort(expCtx->getOperationContext()->getClient()->getLocalPort()));
    builder.appendDate("localTime", Date_t::now());

    if (spec.getOperationStats()) {
        // operationStats is only allowed when featureFlagCursorBasedTop is enabled.
        uassert(ErrorCodes::FailedToParse,
                "BSON field '$collStats.operationStats' is an unknown field.",
                mongo::feature_flags::gFeatureFlagCursorBasedTop.isEnabled(
                    VersionContext::getDecoration(expCtx->getOperationContext()),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

        expCtx->getMongoProcessInterface()->appendOperationStats(
            expCtx->getOperationContext(), nss, &builder);
    }

    if (auto latencyStatsSpec = spec.getLatencyStats()) {
        // getRequestOnTimeseriesView is set to true if collstats is called on the view.
        auto resolvedNss =
            spec.getRequestOnTimeseriesView() ? nss.getTimeseriesViewNamespace() : nss;
        expCtx->getMongoProcessInterface()->appendLatencyStats(expCtx->getOperationContext(),
                                                               resolvedNss,
                                                               latencyStatsSpec->getHistograms(),
                                                               &builder);
    }

    if (auto storageStats = spec.getStorageStats()) {
        // If the storageStats field exists, it must have been validated as an object when parsing.
        BSONObjBuilder storageBuilder(builder.subobjStart("storageStats"));
        uassertStatusOKWithContext(expCtx->getMongoProcessInterface()->appendStorageStats(
                                       expCtx, nss, *storageStats, &storageBuilder, filterObj),
                                   "Unable to retrieve storageStats in $collStats stage");
        storageBuilder.doneFast();
    }

    if (spec.getCount()) {
        uassertStatusOKWithContext(expCtx->getMongoProcessInterface()->appendRecordCount(
                                       expCtx->getOperationContext(), nss, &builder),
                                   "Unable to retrieve count in $collStats stage");
    }

    if (spec.getQueryExecStats()) {
        uassertStatusOKWithContext(expCtx->getMongoProcessInterface()->appendQueryExecStats(
                                       expCtx->getOperationContext(), nss, &builder),
                                   "Unable to retrieve queryExecStats in $collStats stage");
    }
    return builder.obj();
}

}  // namespace exec::agg
}  // namespace mongo
