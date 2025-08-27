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

#include "mongo/db/pipeline/document_source_coll_stats.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/serialization_context.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

using boost::intrusive_ptr;

namespace mongo {

REGISTER_DOCUMENT_SOURCE(collStats,
                         DocumentSourceCollStats::LiteParsed::parse,
                         DocumentSourceCollStats::createFromBson,
                         AllowedWithApiStrict::kConditionally);
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

const char* DocumentSourceCollStats::getSourceName() const {
    return kStageName.data();
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

Value DocumentSourceCollStats::serialize(const SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), _collStatsSpec.toBSON(opts)}});
}

}  // namespace mongo
