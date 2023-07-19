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

#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/pipeline/document_source_query_settings_gen.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/query_settings_manager.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(querySettings,
                                           DocumentSourceQuerySettings::LiteParsed::parse,
                                           DocumentSourceQuerySettings::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           feature_flags::gFeatureFlagQuerySettings);

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceQuerySettings::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(7746801,
            "$querySettings stage expects a document as argument",
            elem.type() == BSONType::Object);
    auto spec = DocumentSourceQuerySettingsSpec::parse(IDLParserContext("$querySettings"),
                                                       elem.embeddedObject());
    std::list<boost::intrusive_ptr<DocumentSource>> stages;

    // Populate the first stage of the pipeline with a queue of all QueryShapeConfigurations by
    // reading from query settings parameter.
    auto& querySettingsManager = query_settings::QuerySettingsManager::get(expCtx->opCtx);
    auto settingsArray =
        querySettingsManager.getAllQueryShapeConfigurations(expCtx->opCtx, expCtx->ns.tenantId());
    std::deque<DocumentSource::GetNextResult> queuedElements;
    for (auto&& queryShapeConfig : settingsArray) {
        queuedElements.emplace_back(Document{queryShapeConfig.toBSON()});
    }
    stages.push_back(make_intrusive<DocumentSourceQueue>(std::move(queuedElements), expCtx));

    // Append $addFields stage which will produce an extra field 'debugQueryShape' with the
    // DebugQueryShape value of the 'representativeQuery'.
    if (spec.getShowDebugQueryShape()) {
        // TODO: SERVER-77790 Implement $_internalDebugQueryShape expression.
    }
    return stages;
}

}  // namespace mongo
