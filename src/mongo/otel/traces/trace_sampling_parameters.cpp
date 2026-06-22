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
        const auto& rateLimit = defaultSampling.getTokenBucketRateLimit();
        TracingSampler::get().updateConfig({
            .defaultFactor = defaultSampling.getSamplingFactor(),
            .defaultRefillRate = rateLimit.getRefillRate(),
            .defaultMaxTokens = rateLimit.getMaxTokens(),
        });
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
    StringData str, const boost::optional<TenantId>&) {
    try {
        return applySamplingConfig(fromjson(str));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void OpenTelemetryTracingSamplingServerParameter::append(OperationContext*,
                                                         BSONObjBuilder* bob,
                                                         StringData name,
                                                         const boost::optional<TenantId>&) {
    OpenTelemetryTracingSamplingConfig config;
    auto samplingConfig = TracingSampler::get().getConfig();
    auto& defaultSampling = config.getDefaultSampling();
    defaultSampling.setSamplingFactor(samplingConfig.defaultFactor);
    defaultSampling.getTokenBucketRateLimit().setRefillRate(samplingConfig.defaultRefillRate);
    defaultSampling.getTokenBucketRateLimit().setMaxTokens(samplingConfig.defaultMaxTokens);
    BSONObjBuilder sub(bob->subobjStart(name));
    config.serialize(&sub);
}

}  // namespace mongo::otel::traces
