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

#include "mongo/db/pipeline/lite_parsed_document_source.h"

#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/string_map.h"

#include <algorithm>

#include <boost/optional/optional.hpp>

namespace mongo {

using Parser = LiteParsedDocumentSource::Parser;
using ParserMap = LiteParsedDocumentSource::ParserMap;

namespace {

// Empty vector used by LiteParsedDocumentSources which do not have a sub pipeline.
inline static std::vector<LiteParsedPipeline> kNoSubPipeline = {};

ParserMap parserMap;

}  // namespace

const LiteParsedDocumentSource::LiteParserInfo&
LiteParsedDocumentSource::LiteParserRegistration::getParser() const {
    // If there's no feature flag toggle, or the feature flag toggle exists and the feature is
    // enabled in this context, use the primary parser. Otherwise, use the fallback parser.
    if (_primaryParserFeatureFlag == nullptr || _primaryParserFeatureFlag->checkEnabled()) {
        return _primaryParser;
    } else {
        return _fallbackParser;
    }
}

void LiteParsedDocumentSource::LiteParserRegistration::setPrimaryParser(LiteParserInfo&& lpi) {
    _primaryParser = std::move(lpi);
    _primaryIsSet = true;
}

void LiteParsedDocumentSource::LiteParserRegistration::setFallbackParser(
    LiteParserInfo&& lpi, IncrementalRolloutFeatureFlag* ff) {
    _fallbackParser = std::move(lpi);
    _primaryParserFeatureFlag = ff;
    _fallbackIsSet = true;
}

bool LiteParsedDocumentSource::LiteParserRegistration::isPrimarySet() const {
    return _primaryIsSet;
}

bool LiteParsedDocumentSource::LiteParserRegistration::isFallbackSet() const {
    return _fallbackIsSet;
}

void LiteParsedDocumentSource::registerParser(const std::string& name,
                                              Parser parser,
                                              AllowedWithApiStrict allowedWithApiStrict,
                                              AllowedWithClientType allowedWithClientType) {
    // It's possible an extension stage is being registered to override an existing server stage
    // (like $vectorSearch), so we should skip re-initializing a counter. We do not assert that
    // this is legal since we do that validation in DocumentSource::registerParser().
    if (!parserMap.contains(name)) {
        // Initialize a counter for this document source to track how many times it is used.
        aggStageCounters.addMetric(name);
    }

    // Retrieve an existing or create a new registration.
    auto& registration = parserMap[name];
    registration.setPrimaryParser({parser, allowedWithApiStrict, allowedWithClientType});
}

void LiteParsedDocumentSource::registerFallbackParser(const std::string& name,
                                                      Parser parser,
                                                      FeatureFlag* parserFeatureFlag,
                                                      AllowedWithApiStrict allowedWithApiStrict,
                                                      AllowedWithClientType allowedWithClientType) {
    if (parserMap.contains(name)) {
        const auto& registration = parserMap.at(name);

        // We require that the fallback parser is always registered prior to the primary parser.
        // At extension load time, it’s then explicit which stages are permitted to be overridden
        // and which cannot.
        tassert(11395100,
                "A stage's fallback parser must be registered before the primary parser",
                registration.isFallbackSet() || !registration.isPrimarySet());

        // Silently skip registration if a fallback parser has already been registered. The first
        // fallback parser registration gets priority.
        return;
    }

    // Initialize a counter for this document source to track how many times it is used.
    aggStageCounters.addMetric(name);

    // Create a new registration and save the parser as the fallback parser.
    auto& registration = parserMap[name];

    // TODO SERVER-114028 Remove the following dynamic cast and tassert when fallback parsing
    // supports all feature flags.
    auto* ifrFeatureFlag = dynamic_cast<IncrementalRolloutFeatureFlag*>(parserFeatureFlag);
    tassert(11395101,
            "Fallback parsing only supports IncrementalRolloutFeatureFlags.",
            ifrFeatureFlag != nullptr);

    registration.setFallbackParser({parser, allowedWithApiStrict, allowedWithClientType},
                                   ifrFeatureFlag);
}

void LiteParsedDocumentSource::unregisterParser_forTest(const std::string& name) {
    parserMap.erase(name);
}

std::unique_ptr<LiteParsedDocumentSource> LiteParsedDocumentSource::parse(
    const NamespaceString& nss, const BSONObj& spec, const LiteParserOptions& options) {
    uassert(40323,
            "A pipeline stage specification object must contain exactly one field.",
            spec.nFields() == 1);
    BSONElement specElem = spec.firstElement();

    auto stageName = specElem.fieldNameStringData();
    const auto it = parserMap.find(stageName);

    uassert(40324,
            str::stream() << "Unrecognized pipeline stage name: '" << stageName << "'",
            it != parserMap.end());

    return it->second.getParser().parser(nss, specElem, options);
}

const LiteParsedDocumentSource::LiteParserInfo& LiteParsedDocumentSource::getInfo(
    const std::string& stageName) {
    const auto it = parserMap.find(stageName);
    uassert(5407200,
            str::stream() << "Unrecognized pipeline stage name: '" << stageName << "'",
            it != parserMap.end());

    return it->second.getParser();
}

const std::vector<LiteParsedPipeline>& LiteParsedDocumentSource::getSubPipelines() const {
    return kNoSubPipeline;
}

LiteParsedDocumentSourceNestedPipelines::LiteParsedDocumentSourceNestedPipelines(
    const BSONElement& spec,
    boost::optional<NamespaceString> foreignNss,
    std::vector<LiteParsedPipeline> pipelines)
    : LiteParsedDocumentSource(spec),
      _foreignNss(std::move(foreignNss)),
      _pipelines(std::move(pipelines)) {}

LiteParsedDocumentSourceNestedPipelines::LiteParsedDocumentSourceNestedPipelines(
    const BSONElement& spec,
    boost::optional<NamespaceString> foreignNss,
    boost::optional<LiteParsedPipeline> pipeline)
    : LiteParsedDocumentSourceNestedPipelines(
          spec, std::move(foreignNss), std::vector<LiteParsedPipeline>{}) {
    if (pipeline)
        _pipelines.emplace_back(std::move(pipeline.value()));
}

stdx::unordered_set<NamespaceString>
LiteParsedDocumentSourceNestedPipelines::getInvolvedNamespaces() const {
    stdx::unordered_set<NamespaceString> involvedNamespaces;
    if (_foreignNss)
        involvedNamespaces.insert(*_foreignNss);

    for (auto&& pipeline : _pipelines) {
        const auto& involvedInSubPipe = pipeline.getInvolvedNamespaces();
        involvedNamespaces.insert(involvedInSubPipe.begin(), involvedInSubPipe.end());
    }
    return involvedNamespaces;
}

void LiteParsedDocumentSourceNestedPipelines::getForeignExecutionNamespaces(
    stdx::unordered_set<NamespaceString>& nssSet) const {
    for (auto&& pipeline : _pipelines) {
        auto nssVector = pipeline.getForeignExecutionNamespaces();
        for (const auto& nssOrUUID : nssVector) {
            tassert(6458500,
                    "nss expected to contain a NamespaceString",
                    nssOrUUID.isNamespaceString());
            nssSet.insert(nssOrUUID.nss());
        }
    }
}

bool LiteParsedDocumentSourceNestedPipelines::isExemptFromIngressAdmissionControl() const {
    return std::any_of(_pipelines.begin(), _pipelines.end(), [](auto&& pipeline) {
        return pipeline.isExemptFromIngressAdmissionControl();
    });
}

Status LiteParsedDocumentSourceNestedPipelines::checkShardedForeignCollAllowed(
    const NamespaceString& nss, bool inMultiDocumentTransaction) const {
    for (auto&& pipeline : _pipelines) {
        if (auto status = pipeline.checkShardedForeignCollAllowed(nss, inMultiDocumentTransaction);
            !status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

ReadConcernSupportResult LiteParsedDocumentSourceNestedPipelines::supportsReadConcern(
    repl::ReadConcernLevel level, bool isImplicitDefault) const {
    // Assume that the document source holding the pipeline has no constraints of its own, so
    // return the strictest of the constraints on the sub-pipelines.
    auto result = ReadConcernSupportResult::allSupportedAndDefaultPermitted();
    for (auto& pipeline : _pipelines) {
        result.merge(pipeline.sourcesSupportReadConcern(level, isImplicitDefault));
        // If both result statuses are already not OK, stop checking.
        if (!result.readConcernSupport.isOK() && !result.defaultReadConcernPermit.isOK()) {
            break;
        }
    }
    return result;
}

PrivilegeVector LiteParsedDocumentSourceNestedPipelines::requiredPrivilegesBasic(
    bool isMongos, bool bypassDocumentValidation) const {
    PrivilegeVector requiredPrivileges;
    for (auto&& pipeline : _pipelines) {
        Privilege::addPrivilegesToPrivilegeVector(
            &requiredPrivileges, pipeline.requiredPrivileges(isMongos, bypassDocumentValidation));
    }
    return requiredPrivileges;
}

const ParserMap& LiteParsedDocumentSource::getParserMap() {
    return parserMap;
}

}  // namespace mongo
