/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/otel/telemetry_context.h"
#include "mongo/util/modules.h"

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/context/propagation/text_map_propagator.h>
#endif

namespace mongo {
namespace MONGO_MOD_PUBLIC otel {
namespace traces {

#ifdef MONGO_CONFIG_OTEL

/**
 * Converts a TelemetryContext to and from its representation as a BSON object.
 */
class TelemetryContextSerializer {
public:
    static std::shared_ptr<TelemetryContext> fromBSON(const BSONObj& bson);
    static BSONObj toBSON(const std::shared_ptr<TelemetryContext>& context);
    static BSONObj appendTelemetryContext(OperationContext* opCtx, BSONObj bson);
};

namespace detail {
using TextMapPropagator = opentelemetry::context::propagation::TextMapPropagator;
using TextMapCarrier = opentelemetry::context::propagation::TextMapCarrier;
std::shared_ptr<TelemetryContext> fromBSON(const BSONObj& bson, TextMapPropagator& propagator);
BSONObj toBSON(const TelemetryContext& context, TextMapPropagator& propagator);
}  // namespace detail

#else

/**
 * Converts a TelemetryContext to and from its representation as a BSON object.
 */
class TelemetryContextSerializer {
public:
    static std::shared_ptr<TelemetryContext> fromBSON(const BSONObj& bson) {
        return std::make_shared<TelemetryContext>();
    }
    static BSONObj toBSON(const std::shared_ptr<TelemetryContext>& context) {
        return BSONObj();
    }
    static BSONObj appendTelemetryContext(OperationContext* opCtx, BSONObj bson) {
        return bson;
    }
};

#endif

}  // namespace traces
}  // namespace MONGO_MOD_PUBLIC otel
}  // namespace mongo
