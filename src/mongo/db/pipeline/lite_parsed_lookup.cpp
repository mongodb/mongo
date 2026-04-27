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

#include "mongo/db/pipeline/lite_parsed_lookup.h"

#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/document_source_lookup.h"      // parseLookupFromAndResolveNamespace
#include "mongo/db/pipeline/document_source_lookup_gen.h"  // DocumentSourceLookupSpec IDL
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/stage_params_to_document_source_registry.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(lookup,
                                     LiteParsedLookUp::parse,
                                     AllowedWithApiStrict::kConditionally);

LiteParsedLookUp::LiteParsedLookUp(const BSONElement& spec,
                                   NamespaceString foreignNss,
                                   boost::optional<LiteParsedPipeline> pipeline,
                                   std::vector<BSONObj> rawPipeline,
                                   std::string as,
                                   BSONObj letVariables,
                                   boost::optional<std::string> localField,
                                   boost::optional<std::string> foreignField,
                                   boost::optional<BSONObj> unwindSpec,
                                   bool hasForeignDB,
                                   bool isHybridSearch)
    : LiteParsedDocumentSourceNestedPipelines(spec, std::move(foreignNss), std::move(pipeline)),
      _rawPipeline(std::move(rawPipeline)),
      _as(std::move(as)),
      _letVariables(std::move(letVariables)),
      _localField(std::move(localField)),
      _foreignField(std::move(foreignField)),
      _unwindSpec(std::move(unwindSpec)),
      _hasForeignDB(hasForeignDB),
      _isHybridSearch(isHybridSearch) {}

std::unique_ptr<LiteParsedLookUp> LiteParsedLookUp::parse(const NamespaceString& nss,
                                                          const BSONElement& spec,
                                                          const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the $lookup stage specification must be an object, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::object);

    auto specObj = spec.Obj();
    auto lookupSpec = DocumentSourceLookupSpec::parse(specObj, IDLParserContext(kStageName));

    NamespaceString fromNss;
    bool hasForeignDB = false;
    if (lookupSpec.getFrom().has_value()) {
        auto fromElem = lookupSpec.getFrom().value().getElement();
        fromNss = parseLookupFromAndResolveNamespace(
            fromElem, nss.dbName(), options.allowGenericForeignDbLookup);
        if (fromElem.type() == BSONType::object) {
            auto specAsNs = NamespaceSpec::parse(fromElem.embeddedObject(),
                                                 IDLParserContext{fromElem.fieldNameStringData()});
            hasForeignDB = specAsNs.getDb().has_value();
        }
    } else {
        uassert(ErrorCodes::FailedToParse,
                "must specify 'pipeline' when 'from' is empty",
                lookupSpec.getPipeline().has_value());
        validateLookupCollectionlessPipeline(lookupSpec.getPipeline().value());
        fromNss = NamespaceString::makeCollectionlessAggregateNSS(nss.dbName());
    }
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "invalid $lookup namespace: " << fromNss.toStringForErrorMsg(),
            fromNss.isValid());

    boost::optional<LiteParsedPipeline> liteParsedPipeline;
    std::vector<BSONObj> rawPipeline;
    if (lookupSpec.getPipeline().has_value()) {
        rawPipeline = lookupSpec.getPipeline().value();
        auto optsCopy = options;
        optsCopy.makeSubpipelineOwned = true;
        liteParsedPipeline = LiteParsedPipeline(fromNss, rawPipeline, false, optsCopy);
    }

    std::string as = std::string{lookupSpec.getAs()};
    BSONObj letVariables =
        lookupSpec.getLetVars().has_value() ? lookupSpec.getLetVars()->getOwned() : BSONObj();

    boost::optional<std::string> localField;
    boost::optional<std::string> foreignField;
    if (lookupSpec.getLocalField().has_value()) {
        localField = std::string{*lookupSpec.getLocalField()};
    }
    if (lookupSpec.getForeignField().has_value()) {
        foreignField = std::string{*lookupSpec.getForeignField()};
    }

    boost::optional<BSONObj> unwindSpec;
    if (lookupSpec.getUnwindSpec().has_value()) {
        unwindSpec = lookupSpec.getUnwindSpec()->getOwned();
    }

    const bool isHybridSearch = lookupSpec.getIsHybridSearch().value_or(false);

    return std::make_unique<LiteParsedLookUp>(spec,
                                              std::move(fromNss),
                                              std::move(liteParsedPipeline),
                                              std::move(rawPipeline),
                                              std::move(as),
                                              std::move(letVariables),
                                              std::move(localField),
                                              std::move(foreignField),
                                              std::move(unwindSpec),
                                              hasForeignDB,
                                              isHybridSearch);
}

PrivilegeVector LiteParsedLookUp::requiredPrivileges(bool isMongos,
                                                     bool bypassDocumentValidation) const {
    PrivilegeVector requiredPrivileges;
    tassert(11282983,
            str::stream() << "$lookup only supports 1 subpipeline, got " << _pipelines.size(),
            _pipelines.size() <= 1);
    tassert(11282982, "Missing foreignNss", _foreignNss);

    if (_pipelines.empty() || !_pipelines[0].startsWithInitialSource()) {
        Privilege::addPrivilegeToPrivilegeVector(
            &requiredPrivileges,
            Privilege(ResourcePattern::forExactNamespace(*_foreignNss), ActionType::find));
    }

    if (!_pipelines.empty()) {
        const LiteParsedPipeline& pipeline = _pipelines[0];
        Privilege::addPrivilegesToPrivilegeVector(
            &requiredPrivileges, pipeline.requiredPrivileges(isMongos, bypassDocumentValidation));
    }

    return requiredPrivileges;
}

Status LiteParsedLookUp::checkShardedForeignCollAllowed(const NamespaceString& nss,
                                                        bool inMultiDocumentTransaction) const {
    const auto fcvSnapshot = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
    if (!inMultiDocumentTransaction ||
        gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot)) {
        return Status::OK();
    }
    auto involvedNss = getInvolvedNamespaces();
    if (involvedNss.find(nss) == involvedNss.end()) {
        return Status::OK();
    }
    return Status(ErrorCodes::NamespaceCannotBeSharded,
                  "Sharded $lookup is not allowed within a multi-document transaction");
}

void LiteParsedLookUp::getForeignExecutionNamespaces(
    stdx::unordered_set<NamespaceString>& nssSet) const {
    tassert(6235100, "Expected foreignNss to be initialized for $lookup", _foreignNss);
    nssSet.emplace(*_foreignNss);
}

bool LiteParsedLookUp::hasExtensionSearchStage() const {
    return !_pipelines.empty() && _pipelines[0].hasExtensionSearchStage();
}

std::unique_ptr<StageParams> LiteParsedLookUp::getStageParams() const {
    boost::optional<LiteParsedPipeline> lpp;
    if (!_pipelines.empty()) {
        lpp = _pipelines[0].clone();
    }
    return std::make_unique<LookUpStageParams>(*_foreignNss,
                                               _as,
                                               _rawPipeline,
                                               _letVariables,
                                               _localField,
                                               _foreignField,
                                               _unwindSpec,
                                               _hasForeignDB,
                                               _isHybridSearch,
                                               getOriginalBson(),
                                               std::move(lpp));
}

void LiteParsedLookUp::validateLookupCollectionlessPipeline(const std::vector<BSONObj>& pipeline) {
    uassert(ErrorCodes::FailedToParse,
            "$lookup stage without explicit collection must have a pipeline with $documents as "
            "first stage",
            pipeline.size() > 0 &&
                !pipeline[0].getField(DocumentSourceDocuments::kStageName).eoo());
}

void LiteParsedLookUp::validateLookupCollectionlessPipeline(const BSONElement& pipeline) {
    uassert(ErrorCodes::FailedToParse, "must specify 'pipeline' when 'from' is empty", pipeline);
    auto parsedPipeline = parsePipelineFromBSON(pipeline);
    validateLookupCollectionlessPipeline(parsedPipeline);
}

}  // namespace mongo
