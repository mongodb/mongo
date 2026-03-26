/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/pipeline/lite_parsed_union_with.h"

#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/document_source_documents.h"       // for kStageName in validation
#include "mongo/db/pipeline/document_source_queue.h"           // for kStageName in validation
#include "mongo/db/pipeline/document_source_union_with_gen.h"  // UnionWithSpec IDL
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
                                         boost::optional<LiteParsedPipeline> pipeline,
                                         std::vector<BSONObj> rawPipeline,
                                         bool hasForeignDB,
                                         bool isHybridSearch)
    : LiteParsedDocumentSourceNestedPipelines(spec, std::move(foreignNss), std::move(pipeline)),
      _rawPipeline(std::move(rawPipeline)),
      _hasForeignDB(hasForeignDB),
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
    boost::optional<LiteParsedPipeline> liteParsedPipeline;
    std::vector<BSONObj> rawPipeline;
    bool hasForeignDb = false;
    bool isHybridSearch = false;
    if (spec.type() == BSONType::string) {
        unionNss = NamespaceStringUtil::deserialize(nss.dbName(), spec.valueStringData());
    } else {
        auto unionWithSpec =
            UnionWithSpec::parse(spec.embeddedObject(), IDLParserContext(kStageName));
        if (unionWithSpec.getColl()) {
            if (unionWithSpec.getDb()) {
                // For LiteParsing, we just assume this is not a view definition, and thus do not
                // assert when 'db' is specified.
                const auto tenantId = nss.dbName().tenantId();
                auto dbName = DatabaseNameUtil::deserialize(
                    tenantId, *unionWithSpec.getDb(), SerializationContext::stateDefault());
                unionNss = NamespaceStringUtil::deserialize(dbName, *unionWithSpec.getColl());
                hasForeignDb = true;
            } else {
                unionNss = NamespaceStringUtil::deserialize(nss.dbName(), *unionWithSpec.getColl());
            }
        } else {
            // If no collection specified, it must have $documents as first field in pipeline.
            validateUnionWithCollectionlessPipeline(unionWithSpec.getPipeline());
            unionNss = NamespaceString::makeCollectionlessAggregateNSS(nss.dbName());
        }

        // Recursively lite parse the nested pipeline, if one exists.
        if (auto pipeline = unionWithSpec.getPipeline()) {
            // The pipeline returned to us by the IDL is owned by us, but since it is a local
            // variable, it will not be saved after parse() returns. We call makeOwned() so that the
            // LiteParsedPipeline will own the BSON after this point.
            auto optsCopy = options;
            optsCopy.makeSubpipelineOwned = true;
            liteParsedPipeline = LiteParsedPipeline(unionNss, *pipeline, false, optsCopy);
            rawPipeline = *pipeline;
        }

        isHybridSearch = unionWithSpec.getIsHybridSearch().value_or(false);
    }

    return std::make_unique<LiteParsedUnionWith>(spec,
                                                 std::move(unionNss),
                                                 std::move(liteParsedPipeline),
                                                 rawPipeline,
                                                 hasForeignDb,
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
    if (_pipelines.empty() || !_pipelines[0].startsWithInitialSource()) {
        Privilege::addPrivilegeToPrivilegeVector(
            &requiredPrivileges,
            Privilege(ResourcePattern::forExactNamespace(*_foreignNss), ActionType::find));
    }

    // Add the sub-pipeline privileges, if one was specified.
    if (!_pipelines.empty()) {
        const LiteParsedPipeline& pipeline = _pipelines[0];
        Privilege::addPrivilegesToPrivilegeVector(
            &requiredPrivileges, pipeline.requiredPrivileges(isMongos, bypassDocumentValidation));
    }
    return requiredPrivileges;
}

std::unique_ptr<StageParams> LiteParsedUnionWith::getStageParams() const {
    boost::optional<LiteParsedPipeline> lpp;
    if (!_pipelines.empty()) {
        lpp = _pipelines[0].clone();
    }
    return std::make_unique<UnionWithStageParams>(*_foreignNss,
                                                  _rawPipeline,
                                                  _hasForeignDB,
                                                  _isHybridSearch,
                                                  getOriginalBson(),
                                                  std::move(lpp));
}

bool LiteParsedUnionWith::hasExtensionVectorSearchStage() const {
    return !_pipelines.empty() && _pipelines[0].hasExtensionVectorSearchStage();
}

bool LiteParsedUnionWith::hasExtensionSearchStage() const {
    return !_pipelines.empty() && _pipelines[0].hasExtensionSearchStage();
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
