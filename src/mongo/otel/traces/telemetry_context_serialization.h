// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/otel/telemetry_context.h"
#include "mongo/rpc/telemetry_context_section_gen.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] otel {
namespace traces {

#ifdef MONGO_CONFIG_OTEL

/**
 * Converts a TelemetryContext to and from its BSON and OpMsg telemetry-section representations.
 */
class TelemetryContextSerializer {
public:
    // TODO(SERVER-133103): Remove toBSON and fromBSON.
    static std::shared_ptr<TelemetryContext> fromBSON(const BSONObj& bson);
    static BSONObj toBSON(const std::shared_ptr<TelemetryContext>& context);
    static std::shared_ptr<TelemetryContext> fromSection(
        const boost::optional<mongo::TelemetryContextSection>& section);
    static boost::optional<mongo::TelemetryContextSection> toSection(
        const std::shared_ptr<TelemetryContext>& context);
};

/**
 * Converts a TelemetryContext to the wire-format type for use in OpMsg's telemetry section.
 * Returns boost::none if ctx is null or if no active span is present in ctx.
 */
boost::optional<TelemetryContextSection> toWireType(const TelemetryContext* ctx);

#else

/**
 * Converts a TelemetryContext to and from its BSON and OpMsg telemetry-section representations.
 */
class TelemetryContextSerializer {
public:
    // TODO(SERVER-133103): Remove toBSON and fromBSON.
    static std::shared_ptr<TelemetryContext> fromBSON(const BSONObj& bson) {
        return std::make_shared<TelemetryContext>();
    }
    static BSONObj toBSON(const std::shared_ptr<TelemetryContext>& context) {
        return BSONObj();
    }
    static std::shared_ptr<TelemetryContext> fromSection(
        const boost::optional<mongo::TelemetryContextSection>& section) {
        return std::make_shared<TelemetryContext>();
    }
    static boost::optional<mongo::TelemetryContextSection> toSection(
        const std::shared_ptr<TelemetryContext>& context) {
        return boost::none;
    }
};

inline boost::optional<::mongo::TelemetryContextSection> toWireType(const TelemetryContext*) {
    return boost::none;
}

#endif

}  // namespace traces
}  // namespace otel
}  // namespace mongo
