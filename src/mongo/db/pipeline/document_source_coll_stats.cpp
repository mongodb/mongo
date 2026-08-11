// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_coll_stats.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/metrics_policy_manager.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/serialization_context.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

using boost::intrusive_ptr;

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(collStats,
                                     DocumentSourceCollStats::LiteParsed::parse,
                                     AllowedWithApiStrict::kConditionally);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(collStats,
                                                   DocumentSourceCollStats,
                                                   CollStatsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(collStats, DocumentSourceCollStats::id)

void DocumentSourceCollStats::LiteParsed::assertPermittedInAPIVersion(
    const APIParameters& apiParameters) const {
    if (apiParameters.getAPIVersion() && *apiParameters.getAPIVersion() == "1" &&
        apiParameters.getAPIStrict().value_or(false)) {
        uassert(ErrorCodes::APIStrictError,
                "only the 'count' option to $collStats is supported in API Version 1",
                !_spec.getLatencyStats() && !_spec.getQueryExecStats() && !_spec.getStorageStats());
    }
}

std::string_view DocumentSourceCollStats::getSourceName() const {
    return kStageName;
}

intrusive_ptr<DocumentSource> DocumentSourceCollStats::createFromBson(
    BSONElement specElem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40166,
            str::stream() << "$collStats must take a nested object but found: " << specElem,
            specElem.type() == BSONType::object);

    const auto tenantId = pExpCtx->getNamespaceString().tenantId();
    const auto vts = tenantId
        ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
              *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
        : boost::none;
    auto spec = DocumentSourceCollStatsSpec::parse(
        specElem.embeddedObject(),
        IDLParserContext(
            kStageName,
            vts,
            tenantId,
            SerializationContext::stateCommandReply(pExpCtx->getSerializationContext())));

    // targetAllNodes is not allowed on mongod instance.
    if (spec.getTargetAllNodes().value_or(false)) {
        uassert(ErrorCodes::FailedToParse,
                "$collStats supports targetAllNodes parameter only for sharded clusters",
                pExpCtx->getInRouter() || pExpCtx->getFromRouter());
    }

    // When running on a router:
    // - If filtering is required by the metrics policy, set 'forceFiltered' to true to indicate to
    //   shards that metrics should be filtered. This is required for filtering to work for external
    //   clients since a router is an internal client so requests from it are not subject to metrics
    //   filtering.
    // - Otherwise, make sure that 'forceFiltered' is not set.
    if (pExpCtx->getInRouter()) {
        auto& metricsPolicyManager = MetricsPolicyManager::get(pExpCtx->getOperationContext());
        if (metricsPolicyManager.requiresFiltering(pExpCtx->getOperationContext(),
                                                   MetricsCategoryEnum::kCollStats,
                                                   spec.getForceFiltered().value_or(false))) {
            spec.setForceFiltered(true);
        } else {
            spec.setForceFiltered({});
        }
    }

    // If 'forceFiltered' is set to true and this is a request from a router, assert that this node
    // supports filtering. This is to prevent this node from silently returning unfiltered metrics.
    if (spec.getForceFiltered().value_or(false) && pExpCtx->getFromRouter()) {
        uassert(
            ErrorCodes::IllegalOperation,
            "'forceFiltered' is set by a router but this node does not support metrics filtering",
            gFeatureFlagCollStatsMetricsFiltering.isEnabled());
    }

    return make_intrusive<DocumentSourceCollStats>(pExpCtx, std::move(spec));
}

Value DocumentSourceCollStats::serialize(const query_shape::SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), _collStatsSpec.toBSON(opts)}});
}

boost::intrusive_ptr<DocumentSource> DocumentSourceCollStats::clone(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    tassert(121023, "expCtx passed to clone must not be null", expCtx);

    return make_intrusive<DocumentSourceCollStats>(expCtx, _collStatsSpec);
}
}  // namespace mongo
