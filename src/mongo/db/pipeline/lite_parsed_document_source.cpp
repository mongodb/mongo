// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_document_source.h"

#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <string_view>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

std::shared_ptr<ResolvedNamespace> tryGetPreResolvedNamespace(
    const NamespaceString& nss, const ResolvedNamespaceMap& resolvedNamespaces) {
    auto it = resolvedNamespaces.find(nss);
    if (it != resolvedNamespaces.end() && it->second.isInvolvedNamespaceAView() &&
        it->second.getParsedPipeline()) {
        auto view = std::make_shared<ResolvedNamespace>(it->second);
        view->desugarViewPipeline();
        return view;
    }
    return nullptr;
}


using Parser = LiteParsedDocumentSource::Parser;
using ParserMap = LiteParsedDocumentSource::ParserMap;

boost::optional<ConstSubPipelinesView> LiteParsedDocumentSource::getSubPipelines() const {
    if (auto mutableView = const_cast<LiteParsedDocumentSource*>(this)->getMutableSubPipelines()) {
        return ConstSubPipelinesView{*mutableView};
    }
    return boost::none;
}

boost::optional<MutableSubPipelinesView> LiteParsedDocumentSource::getMutableSubPipelines() {
    return boost::none;
}

namespace {

ParserMap parserMap;

// Tracks stage names whose aggStageCounters metric has already been added, since adding a metric
// twice is fatal. This happens when a primary parser is registered after its fallback, and in tests
// that re-register a stage, since unregistering a parser does not remove its metric.
StringSet metricsAlreadyRegistered;

}  // namespace

const LiteParsedDocumentSource::LiteParserInfo&
LiteParsedDocumentSource::LiteParserRegistration::getParserInfo(
    const LiteParserOptions& options) const {
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

    // Both a primary and fallback parser have been set. Consult the feature flag's
    // checkWithContext() so each flag kind applies its own semantics for this operation.
    const bool isFeatureEnabled = !_primaryParserFeatureFlag || [&] {
        // Construct and pass a `defaultIfrContext` for non-IFR flag types.
        IncrementalFeatureRolloutContext defaultIfrContext;
        // (Generic FCV reference): Fall back to kLastLTS when 'vCtx' is not initialized. The
        // VersionContext and the FCVSnapshot params only affect the check for FCV-gated feature
        // flags which are currently unsupported. See TODO(SERVER-114028) for more information.
        return _primaryParserFeatureFlag->checkWithContext(
            kVersionContextIgnored_UNSAFE,
            options.ifrContext ? *options.ifrContext : defaultIfrContext,
            ServerGlobalParams::FCVSnapshot{multiversion::GenericFCV::kLastLTS});
    }();

    return isFeatureEnabled ? _primaryParser : _fallbackParser;
}

void LiteParsedDocumentSource::LiteParserRegistration::setPrimaryParser(LiteParserInfo&& lpi) {
    _primaryParser = std::move(lpi);
    _primaryIsSet = true;
}

void LiteParsedDocumentSource::LiteParserRegistration::setFallbackParser(
    LiteParserInfo&& lpi, FeatureFlag* ff, std::string_view stageName) {
    _fallbackParser = std::move(lpi);
    // TODO(SERVER-114028): To support FCV-gated feature flags, parser selection must supply the
    // operation FCV their checkWithContext() requires. Falling back to the global FCV snapshot
    // would be unsafe.
    tassert(13748300,
            str::stream() << "Fallback parsing only supports IncrementalRolloutFeatureFlag and "
                             "BinaryCompatibleFeatureFlag"
                          << stageName,
            !ff || dynamic_cast<IncrementalRolloutFeatureFlag*>(ff) ||
                dynamic_cast<BinaryCompatibleFeatureFlag*>(ff));
    _primaryParserFeatureFlag = ff;
    _fallbackIsSet = true;
}

bool LiteParsedDocumentSource::LiteParserRegistration::isPrimarySet() const {
    return _primaryIsSet;
}

bool LiteParsedDocumentSource::LiteParserRegistration::isFallbackSet() const {
    return _fallbackIsSet;
}

void LiteParsedDocumentSource::registerParser(const std::string& name, LiteParserInfo parserInfo) {
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
    registration.setPrimaryParser(std::move(parserInfo));
}

void LiteParsedDocumentSource::registerFallbackParser(const std::string& name,
                                                      FeatureFlag* parserFeatureFlag,
                                                      LiteParserInfo parserInfo) {
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
    if (!metricsAlreadyRegistered.contains(name)) {
        aggStageCounters.addMetric(name);
        metricsAlreadyRegistered.insert(name);
    }

    // Create a new registration and save the parser as the fallback parser. A null
    // parserFeatureFlag signifies a stub stage that isn't associated with a feature flag.
    auto& registration = parserMap[name];
    registration.setFallbackParser(std::move(parserInfo), parserFeatureFlag, name);
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

    auto lpInfo = it->second.getParserInfo(options);
    if (options.extensionMetrics && lpInfo.fromExtension) {
        options.extensionMetrics->trackThisRequestAsHavingUsedExtensions();
    }
    auto lpds = lpInfo.parser(nss, specElem, options);
    lpds->setApiStrict(lpInfo.allowedWithApiStrict);
    lpds->setClientType(lpInfo.allowedWithClientType);
    return lpds;
}

const ParserMap& LiteParsedDocumentSource::getParserMap() {
    return parserMap;
}

bool LiteParsedDocumentSource::isRegisteredExtensionStage(std::string_view stageName) {
    const auto it = parserMap.find(stageName);
    if (it == parserMap.end()) {
        return false;
    }

    return it->second.getParserInfo().fromExtension;
}

}  // namespace mongo
