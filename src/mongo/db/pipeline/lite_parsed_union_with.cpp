// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_union_with.h"

#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/document_source_documents.h"       // for kStageName in validation
#include "mongo/db/pipeline/document_source_queue.h"           // for kStageName in validation
#include "mongo/db/pipeline/document_source_union_with_gen.h"  // UnionWithSpec IDL
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params_to_document_source_registry.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#include <iterator>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(unionWith,
                                     LiteParsedUnionWith::parse,
                                     AllowedWithApiStrict::kAlways);

LiteParsedUnionWith::LiteParsedUnionWith(const BSONElement& spec,
                                         NamespaceString foreignNss,
                                         boost::optional<OwnedLiteParsedPipeline> pipeline,
                                         std::vector<BSONObj> rawPipeline,
                                         bool isHybridSearch)
    : LiteParsedDocumentSourceNestedPipelines(spec, std::move(foreignNss), std::move(pipeline)),
      _rawPipeline(std::move(rawPipeline)),
      _isHybridSearch(isHybridSearch) {}

std::unique_ptr<LiteParsedUnionWith> LiteParsedUnionWith::parse(const NamespaceString& nss,
                                                                const BSONElement& spec,
                                                                const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream()
                << "the $unionWith stage specification must be an object or string, but found "
                << typeName(spec.type()),
            spec.type() == BSONType::object || spec.type() == BSONType::string);

    NamespaceString unionNss;
    boost::optional<OwnedLiteParsedPipeline> ownedPipeline;
    std::vector<BSONObj> rawPipeline;
    bool isHybridSearch = false;
    if (spec.type() == BSONType::string) {
        unionNss = NamespaceStringUtil::deserialize(nss.dbName(), spec.valueStringData());
    } else {
        auto unionWithSpec =
            UnionWithSpec::parse(spec.embeddedObject(), IDLParserContext(kStageName));
        if (unionWithSpec.getColl()) {
            unionNss = NamespaceStringUtil::deserialize(nss.dbName(), *unionWithSpec.getColl());
        } else {
            // If no collection specified, it must have $documents as first field in pipeline.
            validateUnionWithCollectionlessPipeline(unionWithSpec.getPipeline());
            unionNss = NamespaceString::makeCollectionlessAggregateNSS(nss.dbName());
        }

        // Recursively lite-parse the nested pipeline, threading the IFR context: a hybrid stage
        // here LP-desugars and the view machinery resolves the inner $unionWith shard-safely,
        // and extension stages need the context for their view-policy checks.
        if (auto pipeline = unionWithSpec.getPipeline()) {
            ownedPipeline = OwnedLiteParsedPipeline(unionNss, *pipeline, options);
            rawPipeline = *pipeline;
        }

        isHybridSearch = unionWithSpec.getIsHybridSearch().value_or(false);
    }

    return std::make_unique<LiteParsedUnionWith>(spec,
                                                 std::move(unionNss),
                                                 std::move(ownedPipeline),
                                                 std::move(rawPipeline),
                                                 isHybridSearch);
}

bool LiteParsedUnionWith::requiresAuthzChecks() const {
    return false;
}

PrivilegeVector LiteParsedUnionWith::requiredPrivileges(bool isMongos,
                                                        bool bypassDocumentValidation) const {
    PrivilegeVector requiredPrivileges;
    tassert(11282960,
            str::stream() << "$unionWith only supports 1 subpipeline, got " << _pipelines.size(),
            _pipelines.size() <= 1);
    tassert(11282959, "Missing foreignNss", _foreignNss);
    // If no pipeline is specified, then assume that we're reading directly from the collection.
    // Otherwise check whether the pipeline starts with an "initial source" indicating that we don't
    // require the "find" privilege.
    if (_pipelines.empty() || !_pipelines[0]->startsWithInitialSource()) {
        Privilege::addPrivilegeToPrivilegeVector(
            &requiredPrivileges,
            Privilege(ResourcePattern::forExactNamespace(*_foreignNss), ActionType::find));
    }

    // Add the sub-pipeline privileges, if one was specified.
    if (!_pipelines.empty()) {
        const LiteParsedPipeline& pipeline = *_pipelines[0];
        Privilege::addPrivilegesToPrivilegeVector(
            &requiredPrivileges, pipeline.requiredPrivileges(isMongos, bypassDocumentValidation));
    }
    return requiredPrivileges;
}

std::unique_ptr<StageParams> LiteParsedUnionWith::getStageParams() const {
    boost::optional<StageParamsPipeline> subParams;
    if (!_pipelines.empty()) {
        subParams = _pipelines[0]->getStageParams();
    }
    return std::make_unique<UnionWithStageParams>(*_foreignNss,
                                                  _rawPipeline,
                                                  _isHybridSearch,
                                                  getOriginalBson().wrap(),
                                                  std::move(subParams),
                                                  getResolvedBackingNss());
}

bool LiteParsedUnionWith::hasExtensionVectorSearchStage() const {
    return !_pipelines.empty() && _pipelines[0]->hasExtensionVectorSearchStage();
}

bool LiteParsedUnionWith::hasExtensionSearchStage() const {
    return !_pipelines.empty() && _pipelines[0]->hasExtensionSearchStage();
}

void LiteParsedUnionWith::validateUnionWithCollectionlessPipeline(
    const boost::optional<std::vector<mongo::BSONObj>>& pipeline) {
    const auto errMsg =
        "$unionWith stage without explicit collection must have a pipeline with $documents as "
        "first stage";

    uassert(ErrorCodes::FailedToParse, errMsg, pipeline && pipeline->size() > 0);
    const auto firstStageBson = (*pipeline)[0];
    LOGV2_DEBUG(5909700,
                4,
                "$unionWith validating collectionless pipeline",
                "pipeline"_attr = Pipeline::serializePipelineForLogging(*pipeline),
                "first"_attr = redact(firstStageBson));
    uassert(ErrorCodes::FailedToParse,
            errMsg,
            (firstStageBson.hasField(DocumentSourceDocuments::kStageName) ||
             firstStageBson.hasField(DocumentSourceQueue::kStageName)));
}

}  // namespace mongo
