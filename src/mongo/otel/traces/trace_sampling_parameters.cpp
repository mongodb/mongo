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

Status applySamplingConfig(const BSONObj& obj) {
    try {
        IDLParserContext ctx("openTelemetryTracingSampling");
        auto config = OpenTelemetryTracingSamplingConfig::parse(obj, ctx);

        const auto& defaultSampling = config.getDefaultSampling();
        const auto& defaultRateLimit = defaultSampling.getTokenBucketRateLimit();
        SamplingConfig samplingConfig{
            .defaultSpans =
                {
                    .factor = defaultSampling.getSamplingFactor(),
                    .refillRate = defaultRateLimit.getRefillRate(),
                    .maxTokens = defaultRateLimit.getMaxTokens(),
                },
        };
        if (const auto& samples = config.getSamples()) {
            for (const auto& sample : *samples) {
                const auto& strategy =
                    sample.getSamplingStrategy().value_or(config.getDefaultSampling());
                const auto& rateLimit = strategy.getTokenBucketRateLimit();
                samplingConfig.perSpanOverrides[sample.getSpanSelection().getName()] = {
                    .factor = strategy.getSamplingFactor(),
                    .refillRate = rateLimit.getRefillRate(),
                    .maxTokens = rateLimit.getMaxTokens(),
                };
            }
        }

        TracingSampler::get().updateConfig(std::move(samplingConfig));
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
    defaultStrategy.getTokenBucketRateLimit().setRefillRate(config.defaultSpans.refillRate);
    defaultStrategy.getTokenBucketRateLimit().setMaxTokens(config.defaultSpans.maxTokens);

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
            strategy.getTokenBucketRateLimit().setRefillRate(params.refillRate);
            strategy.getTokenBucketRateLimit().setMaxTokens(params.maxTokens);

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

}  // namespace mongo::otel::traces
