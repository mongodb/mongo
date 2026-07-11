// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/otel/traces/trace_sampling_parameters_gen.h"

namespace mongo::otel::traces {
namespace {

Status applyExternalConfig(const BSONObj& obj) {
    try {
        IDLParserContext ctx("openTelemetryExternalTracing");
        auto config = OpenTelemetryTracingExternalConfig::parse(obj, ctx);
        const auto& rateLimit = config.getTokenBucketRateLimit();
        TracingSampler::get().updateExternalConfig(
            {rateLimit.getRefillRate(), rateLimit.getMaxTokens()});
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status applySamplingConfig(const BSONObj& obj) {
    try {
        IDLParserContext ctx("openTelemetryTracingSampling");
        auto config = OpenTelemetryTracingSamplingConfig::parse(obj, ctx);

        const auto& defaultSampling = config.getDefaultSampling();
        const auto& defaultRateLimit = defaultSampling.getTokenBucketRateLimit();
        SamplingParameters defaultSpans = {
            .factor = defaultSampling.getSamplingFactor(),
            .rateLimits = {defaultRateLimit.getRefillRate(), defaultRateLimit.getMaxTokens()},
        };

        StringMap<SamplingParameters> perSpanOverrides;
        if (const auto& samples = config.getSamples()) {
            for (const auto& sample : *samples) {
                const auto& strategy =
                    sample.getSamplingStrategy().value_or(config.getDefaultSampling());
                const auto& rateLimit = strategy.getTokenBucketRateLimit();
                perSpanOverrides[sample.getSpanSelection().getName()] = {
                    .factor = strategy.getSamplingFactor(),
                    .rateLimits = {rateLimit.getRefillRate(), rateLimit.getMaxTokens()},
                };
            }
        }

        TracingSampler::get().updateInternalConfig(defaultSpans, perSpanOverrides);
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

}  // namespace

Status OpenTelemetryTracingSamplingServerParameter::set(const BSONElement& newValueElement,
                                                        const boost::optional<TenantId>&) {
    if (newValueElement.type() != BSONType::object) {
        return Status(ErrorCodes::BadValue, "openTelemetryTracingSampling must be a BSON document");
    }
    return applySamplingConfig(newValueElement.Obj());
}

Status OpenTelemetryTracingSamplingServerParameter::setFromString(
    std::string_view str, const boost::optional<TenantId>&) {
    try {
        return applySamplingConfig(fromjson(str));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void OpenTelemetryTracingSamplingServerParameter::append(OperationContext*,
                                                         BSONObjBuilder* bob,
                                                         std::string_view name,
                                                         const boost::optional<TenantId>&) {
    auto config = TracingSampler::get().getConfig();

    OpenTelemetryTracingSamplingStrategy defaultStrategy;
    defaultStrategy.setSamplingFactor(config.defaultSpans.factor);
    defaultStrategy.getTokenBucketRateLimit().setRefillRate(
        config.defaultSpans.rateLimits.refillRate);
    defaultStrategy.getTokenBucketRateLimit().setMaxTokens(
        config.defaultSpans.rateLimits.maxTokens);

    OpenTelemetryTracingSamplingConfig idlConfig;
    idlConfig.setDefaultSampling(std::move(defaultStrategy));

    if (!config.perSpanOverrides.empty()) {
        std::vector<OpenTelemetryTracingSample> samples;
        samples.reserve(config.perSpanOverrides.size());
        for (const auto& [spanName, params] : config.perSpanOverrides) {
            OpenTelemetryTracingSpanSelection selection;
            selection.setName(spanName);

            OpenTelemetryTracingSamplingStrategy strategy;
            strategy.setSamplingFactor(params.factor);
            strategy.getTokenBucketRateLimit().setRefillRate(params.rateLimits.refillRate);
            strategy.getTokenBucketRateLimit().setMaxTokens(params.rateLimits.maxTokens);

            OpenTelemetryTracingSample sample;
            sample.setSpanSelection(std::move(selection));
            sample.setSamplingStrategy(std::move(strategy));
            samples.push_back(std::move(sample));
        }
        idlConfig.setSamples(std::move(samples));
    }

    BSONObjBuilder sub(bob->subobjStart(name));
    idlConfig.serialize(&sub);
}

Status OpenTelemetryExternalTracingServerParameter::set(const BSONElement& newValueElement,
                                                        const boost::optional<TenantId>&) {
    if (newValueElement.type() != BSONType::object) {
        return Status(ErrorCodes::BadValue, "openTelemetryExternalTracing must be a BSON document");
    }
    return applyExternalConfig(newValueElement.Obj());
}

Status OpenTelemetryExternalTracingServerParameter::setFromString(
    std::string_view str, const boost::optional<TenantId>&) {
    try {
        return applyExternalConfig(fromjson(str));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void OpenTelemetryExternalTracingServerParameter::append(OperationContext*,
                                                         BSONObjBuilder* bob,
                                                         std::string_view name,
                                                         const boost::optional<TenantId>&) {
    auto rateLimits = TracingSampler::get().getConfig().externalRateLimits;
    OpenTelemetryTracingTokenBucketRateLimit rateLimit;
    rateLimit.setRefillRate(rateLimits.refillRate);
    rateLimit.setMaxTokens(rateLimits.maxTokens);
    OpenTelemetryTracingExternalConfig config;
    config.setTokenBucketRateLimit(std::move(rateLimit));
    BSONObjBuilder sub(bob->subobjStart(name));
    config.serialize(&sub);
}

}  // namespace mongo::otel::traces
