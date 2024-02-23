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

#include "document_source_query_settings.h"

#include <algorithm>
#include <iterator>

#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/pipeline/document_source_query_settings_gen.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"

namespace mongo {

using namespace query_settings;

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(querySettings,
                                           DocumentSourceQuerySettings::LiteParsed::parse,
                                           DocumentSourceQuerySettings::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           feature_flags::gFeatureFlagQuerySettings);

namespace {
BSONObj createDebugQueryShape(const BSONObj& representativeQuery,
                              OperationContext* opCtx,
                              const boost::optional<TenantId>& tenantId) {
    // Get the serialized query shape by creating the representative information for the given
    // representative query.
    const auto representativeInfo =
        query_settings::createRepresentativeInfo(representativeQuery, opCtx, tenantId);
    // Erase the nested 'tenantId' so that information doesn't get leaked to the user.
    MutableDocument mutableDocument(Document{representativeInfo.serializedQueryShape});
    mutableDocument.setNestedField("cmdNs.tenantId", Value());
    return mutableDocument.freeze().toBson();
}

DocumentSource::GetNextResult createResult(OperationContext* opCtx,
                                           const boost::optional<TenantId>& tenantId,
                                           QueryShapeConfiguration&& configuration,
                                           bool includeDebugQueryShape) try {
    BSONObjBuilder bob;
    configuration.serialize(&bob);
    if (includeDebugQueryShape) {
        bob.append(DocumentSourceQuerySettings::kDebugQueryShapeFieldName,
                   createDebugQueryShape(configuration.getRepresentativeQuery(), opCtx, tenantId));
    }
    return Document{bob.obj()};
} catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
    uasserted(ErrorCodes::BSONObjectTooLarge,
              str::stream() << "query settings object exceeds " << BSONObjMaxInternalSize
                            << " bytes"
                            << (includeDebugQueryShape
                                    ? "; consider not setting 'showDebugQueryShape' to true"
                                    : ""));
}
}  // namespace

boost::intrusive_ptr<DocumentSource> DocumentSourceQuerySettings::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(7746801,
            "$querySettings stage expects a document as argument",
            elem.type() == BSONType::Object);

    // Resolve whether to include the debug query shape or not.
    bool includeDebugQueryShape = DocumentSourceQuerySettingsSpec::parse(
                                      IDLParserContext("$querySettings"), elem.embeddedObject())
                                      .getShowDebugQueryShape();

    // Alias this stage to a 'DocumentSourceQueue'. The queue must be initialized in a deferred
    // manner in order to avoid creating a infinite recursion loop.
    //
    // Creating a debug query shape requires re-parsing the representative query, then serializing
    // it back with debug options. Doing so triggers in turn a call to
    // 'DocumentSourceQuerySettings::createFromBson()' hence triggering the infinite recursion.
    DocumentSourceQueue::DeferredQueue deferredQueue{[expCtx, includeDebugQueryShape]() {
        // Get all query shape configurations owned by 'tenantId' and map them over a queue of
        // results.
        auto tenantId = expCtx->ns.tenantId();
        auto& manager = QuerySettingsManager::get(expCtx->opCtx);
        auto settingsArray = manager.getAllQueryShapeConfigurations(expCtx->opCtx, tenantId);
        std::deque<DocumentSource::GetNextResult> queue;
        std::transform(std::make_move_iterator(settingsArray.begin()),
                       std::make_move_iterator(settingsArray.end()),
                       std::back_inserter(queue),
                       [&](auto&& config) {
                           return createResult(
                               expCtx->opCtx, tenantId, std::move(config), includeDebugQueryShape);
                       });
        return queue;
    }};

    // Alias this stage to 'DocumentSourceQueue' with the appropriate stage constraints. Override
    // the serialization such that it gets displayed as '{$querySettings: <options>}'.
    return make_intrusive<DocumentSourceQueue>(std::move(deferredQueue),
                                               expCtx,
                                               /* stageNameOverride */ kStageName,
                                               /* serializeOverride */ Value{elem.wrap()},
                                               /* constraintsOverride */ constraints());
}
}  // namespace mongo
