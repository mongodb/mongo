// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

DocumentSourceContainer buildDocumentSource(const std::unique_ptr<StageParams>& stageParams,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    tassert(12788400, "stageParams should not be null", stageParams);
    auto it = documentSourceBuildersMap.find(stageParams->getId());
    tassert(12788401,
            str::stream() << "No DocumentSource mapping found for StageParams id "
                          << stageParams->getId(),
            it != documentSourceBuildersMap.end());
    return (it->second)(stageParams, expCtx);
}

}  // namespace mongo
