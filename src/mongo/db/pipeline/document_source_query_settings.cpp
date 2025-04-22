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

#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/pipeline/document_source_query_settings_gen.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/query_settings/query_settings_service.h"

namespace mongo {

using namespace query_settings;

DocumentSourceQuerySettings::DocumentSourceQuerySettings(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::vector<QueryShapeConfiguration> configs,
    bool showDebugQueryShape)
    : DocumentSource(kStageName, expCtx),
      _configs(std::move(configs)),
      _iterator(_configs.begin()),
      _showDebugQueryShape(showDebugQueryShape) {}

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(querySettings,
                                           DocumentSourceQuerySettings::LiteParsed::parse,
                                           DocumentSourceQuerySettings::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           feature_flags::gFeatureFlagQuerySettings);
ALLOCATE_DOCUMENT_SOURCE_ID(querySettings, DocumentSourceQuerySettings::id)

namespace {
BSONObj createDebugQueryShape(const BSONObj& representativeQuery,
                              OperationContext* opCtx,
                              const boost::optional<TenantId>& tenantId) {
    // Get the serialized query shape by creating the representative information for the given
    // representative query.
    const auto representativeInfo =
        query_settings::createRepresentativeInfo(opCtx, representativeQuery, tenantId);
    return representativeInfo.serializedQueryShape;
}

DocumentSource::GetNextResult createResult(OperationContext* opCtx,
                                           const boost::optional<TenantId>& tenantId,
                                           const QueryShapeConfiguration& configuration,
                                           bool includeDebugQueryShape) try {
    BSONObjBuilder bob;
    configuration.serialize(&bob);
    if (includeDebugQueryShape && configuration.getRepresentativeQuery()) {
        bob.append(DocumentSourceQuerySettings::kDebugQueryShapeFieldName,
                   createDebugQueryShape(*configuration.getRepresentativeQuery(), opCtx, tenantId));
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

DocumentSource::GetNextResult DocumentSourceQuerySettings::doGetNext() {
    if (_iterator == _configs.end()) {
        return DocumentSource::GetNextResult::makeEOF();
    }

    return createResult(getContext()->getOperationContext(),
                        getContext()->getNamespaceString().tenantId(),
                        *_iterator++,
                        _showDebugQueryShape);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceQuerySettings::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(7746801,
            "$querySettings stage expects a document as argument",
            elem.type() == BSONType::Object);

    // Resolve whether to include the debug query shape or not.
    bool showDebugQueryShape = DocumentSourceQuerySettingsSpec::parse(
                                   IDLParserContext("$querySettings"), elem.embeddedObject())
                                   .getShowDebugQueryShape();

    // Get all query shape configurations owned by 'tenantId'.
    auto tenantId = expCtx->getNamespaceString().tenantId();
    auto&& configs =
        query_settings::getAllQueryShapeConfigurations(expCtx->getOperationContext(), tenantId)
            .queryShapeConfigurations;

    return make_intrusive<DocumentSourceQuerySettings>(
        expCtx, std::move(configs), showDebugQueryShape);
}

Value DocumentSourceQuerySettings::serialize(const SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << DOC("showDebugQueryShape" << Value(_showDebugQueryShape))));
}
}  // namespace mongo
