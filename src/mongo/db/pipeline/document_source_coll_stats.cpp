// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_coll_stats.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/query/allowed_contexts.h"
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
