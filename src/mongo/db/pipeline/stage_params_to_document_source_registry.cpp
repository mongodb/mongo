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

#include "mongo/db/pipeline/stage_params_to_document_source_registry.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

stdx::unordered_map<StageParams::Id, StageParamsToDocumentSourceFn> documentSourceBuildersMap;

}  // namespace

void registerStageParamsToDocumentSourceFn(StageParams::Id stageParamsId,
                                           StageParamsToDocumentSourceFn fn) {
    const auto [itr_ignored, inserted] =
        documentSourceBuildersMap.insert(std::make_pair(stageParamsId, fn));
    tassert(11458700,
            "Attempted to insert a duplicate in the StageParams to DocumentSource mapping",
            inserted);
}

// Populate 'StageParams' to 'DocumentSource' mapping function registry after every
// 'StageParams' subclass got its unique 'Id' assigned and after the legacy parserMap is populated.
// The dependency on EndDocumentSourceRegistration ensures we can check isInParserMap() during
// registration to validate that stages are not registered in both registries.
MONGO_INITIALIZER_GROUP(BeginStageParamsToDocumentSourceRegistration,
                        ("EndStageIdAllocation", "EndDocumentSourceRegistration"),
                        ())
MONGO_INITIALIZER_GROUP(EndStageParamsToDocumentSourceRegistration,
                        ("BeginStageParamsToDocumentSourceRegistration"),
                        ())

DocumentSourceContainer buildDocumentSource(const LiteParsedDocumentSource& liteParsed,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto stageParams = liteParsed.getStageParams();

    tassert(11458702, "stageParams should not be null", stageParams);

    if (auto it = documentSourceBuildersMap.find(stageParams->getId());
        it != documentSourceBuildersMap.end()) {
        return (it->second)(stageParams, expCtx);
    }

    tasserted(11434300,
              str::stream() << "Stage '" << liteParsed.getParseTimeName()
                            << "' does not exist in the parser map");
}

}  // namespace mongo
