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
#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"

#include <algorithm>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using Parser = LiteParsedDocumentSource::Parser;
using ParserMap = LiteParsedDocumentSource::ParserMap;

namespace {

// Empty vector used by LiteParsedDocumentSources which do not have a sub pipeline.
inline static std::vector<LiteParsedPipeline> kNoSubPipeline = {};

ParserMap parserMap;

// Metrics are added to aggStageCounters upon LiteParsedDocumentSource registration. Considering
// that LiteParsedDocumentSources can be unregistered in tests and metrics cannot be removed from
// aggStageCounters, we need a data structure to track which metrics have already been registered in
// order to prevent duplicate registration.
StringSet metricsAlreadyRegistered;

}  // namespace

const LiteParsedDocumentSource::LiteParserInfo&
LiteParsedDocumentSource::LiteParserRegistration::getParserInfo() const {
    // If no fallback is set, use the primary parser. This is the standard case for most stages.
    if (!_fallbackIsSet) {
        tassert(11395400, "Primary parser must be set if no fallback parser exists", _primaryIsSet);
        return _primaryParser;
    }

    // If no primary is set, use the fallback parser. This typically occurs when an extension has
    // not been loaded. See aggregation_stage_fallback_parsers.json.
    if (!_primaryIsSet) {
        tassert(
            11395401, "Fallback parser must be set if no primary parser exists", _fallbackIsSet);
        return _fallbackParser;
    }

    // Both a primary and fallback parser have been set. Check the value of the associated feature
    // flag to evaluate which parser we should use.
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
    LiteParserInfo&& lpi, IncrementalRolloutFeatureFlag* ff, bool isStub) {
    _fallbackParser = std::move(lpi);
    _primaryParserFeatureFlag = ff;
    _fallbackIsSet = true;
    _isStub = isStub;
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
    if (!metricsAlreadyRegistered.contains(name)) {
        // Initialize a counter for this document source to track how many times it is used.
        aggStageCounters.addMetric(name);
        metricsAlreadyRegistered.insert(name);
    }

    // Retrieve an existing or create a new registration.
    auto& registration = parserMap[name];

    if (registration.isPrimarySet()) {
        LOGV2_FATAL(11534800,
                    "Cannot override primary parser on aggregation stage.",
                    "stageName"_attr = name);
    }
    registration.setPrimaryParser({parser, allowedWithApiStrict, allowedWithClientType});
}

void LiteParsedDocumentSource::registerFallbackParser(const std::string& name,
                                                      Parser parser,
                                                      FeatureFlag* parserFeatureFlag,
                                                      AllowedWithApiStrict allowedWithApiStrict,
                                                      AllowedWithClientType allowedWithClientType,
                                                      bool isStub) {
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
    metricsAlreadyRegistered.insert(name);

    // Create a new registration and save the parser as the fallback parser.
    auto& registration = parserMap[name];

    IncrementalRolloutFeatureFlag* ifrFeatureFlag = nullptr;
    // If parserFeatureFlag is not set, we are adding a fallback parser for a stub stage that isn't
    // associated with a feature flag.
    if (parserFeatureFlag != nullptr) {
        // TODO SERVER-114028 Remove the following dynamic cast and tassert when fallback parsing
        // supports all feature flags.
        ifrFeatureFlag = dynamic_cast<IncrementalRolloutFeatureFlag*>(parserFeatureFlag);
        tassert(11395101,
                "Fallback parsing only supports IncrementalRolloutFeatureFlags.",
                ifrFeatureFlag != nullptr);
    }

    registration.setFallbackParser(
        {parser, allowedWithApiStrict, allowedWithClientType}, ifrFeatureFlag, isStub);
}

void LiteParsedDocumentSource::unregisterParser_forTest(const std::string& name) {
    parserMap.erase(name);
}

const LiteParsedDocumentSource::LiteParserInfo& LiteParsedDocumentSource::getParserInfo_forTest(
    const std::string& name) {
    return parserMap.find(name)->second.getParserInfo();
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

    auto lpInfo = it->second.getParserInfo();
    auto lpds = lpInfo.parser(nss, specElem, options);
    lpds->setApiStrict(lpInfo.allowedWithApiStrict);
    lpds->setClientType(lpInfo.allowedWithClientType);
    return lpds;
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

ViewInfo::ViewInfo(NamespaceString pViewName,
                   NamespaceString pResolvedNss,
                   std::vector<BSONObj> pViewPipeBson,
                   const LiteParserOptions& pOptions)
    : viewName(std::move(pViewName)),
      resolvedNss(std::move(pResolvedNss)),
      _ownedOriginalBsonPipeline(std::move(pViewPipeBson)) {
    viewPipeline.reserve(_ownedOriginalBsonPipeline.size());
    for (const auto& stage : _ownedOriginalBsonPipeline) {
        viewPipeline.push_back(LiteParsedDocumentSource::parse(viewName, stage, pOptions));
    }
}

std::vector<BSONObj> ViewInfo::getOriginalBson() const {
    return _ownedOriginalBsonPipeline;
}

ViewInfo ViewInfo::clone() const {
    return ViewInfo{viewName, resolvedNss, getOriginalBson()};
}

DisallowViewsPolicy::DisallowViewsPolicy()
    : ViewPolicy(
          kFirstStageApplicationPolicy::kDoNothing, [](const ViewInfo&, StringData stageName) {
              uasserted(ErrorCodes::CommandNotSupportedOnView,
                        std::string(str::stream() << stageName << " is not supported on views."));
          }) {}

DisallowViewsPolicy::DisallowViewsPolicy(ViewPolicyCallbackFn&& fn)
    : ViewPolicy(kFirstStageApplicationPolicy::kDoNothing, std::move(fn)) {}


}  // namespace mongo
