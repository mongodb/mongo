// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/otel/traces/tracing_utils.h"
#include "mongo/rpc/telemetry_context_section_gen.h"
#include "mongo/util/modules.h"

#include <opentelemetry/context/propagation/text_map_propagator.h>

namespace mongo {
namespace otel {
namespace traces {

// TODO SERVER-112886: Once this ticket is resolved this should be changed back to an empty string.
constexpr OtelStringView kMissingKeyReturnValue = " ";

using opentelemetry::nostd::function_ref;

/**
 * OpenTelemetry Carrier (https://opentelemetry.io/docs/specs/otel/context/api-propagators/#carrier)
 * providing interoperability between BSONObj and OpenTelemetry TextMap Propagators
 * (https://opentelemetry.io/docs/specs/otel/context/api-propagators/#textmap-propagator).
 */
class [[MONGO_MOD_PARENT_PRIVATE]] BSONTextMapCarrier : public TextMapCarrier {
public:
    constexpr static auto kTraceParentKey = "traceparent";

    /**
     * Default constructor with an empty initial BSONObj. Intended to be used when using a
     * Propagator to Inject
     * (https://opentelemetry.io/docs/specs/otel/context/api-propagators/#inject) data into a
     * BSONObj, which can then be retreived with toBSON().
     */
    BSONTextMapCarrier() = default;

    /**
     * Constructor which wraps an existing BSONObj. Intended to be used when using a Propagator
     * to Extract (https://opentelemetry.io/docs/specs/otel/context/api-propagators/#extract) data
     * from a BSONObj into a SpanContext.
     * Note that BSONTextMapCarrier only supports fields of type String. Fields of any other type
     * present in the bson argument will be ignored.
     */
    BSONTextMapCarrier(const BSONObj& bson);

    /**
     * Constructor which wraps an existing TelemetryContextSection. Intended to be used when using
     * a Propagator to Extract
     * (https://opentelemetry.io/docs/specs/otel/context/api-propagators/#extract) data from a
     * TelemetryContextSection into a SpanContext.
     */
    BSONTextMapCarrier(const TelemetryContextSection& telemetryContext);

    /**
     * Gets a value from the underlying BSONObj.
     * Part of TextMapCarrier's API, see:
     * https://opentelemetry.io/docs/specs/otel/context/api-propagators/#get.
     */
    OtelStringView Get(OtelStringView key) const noexcept override;

    /**
     * Sets a value in the underlying BSONObj.
     * Part of TextMapCarrier's API, see:
     * https://opentelemetry.io/docs/specs/otel/context/api-propagators/#set.
     */
    void Set(OtelStringView key, OtelStringView value) noexcept override;

    /**
     * Calls the provided callback once for each key present in the underlying BSONObj. The callback
     * can return false to stop iterating early. Keys() itself will return false if the callback
     * ever returns false, and true otherwise. This is done so that the caller can make decisions
     * regarding allocations; see getKeySet() in tracing_utils.h for an example of usage.
     * Part of TextMapCarrier's API, see:
     * https://opentelemetry.io/docs/specs/otel/context/api-propagators/#keys.
     */
    bool Keys(function_ref<bool(OtelStringView)> callback) const noexcept override;

    /**
     * Returns the underlying BSONObj. Intended to be used to retrieve values injected by a TextMap
     * Propagator as BSON. See also the comment on the default constructor.
     */
    BSONObj toBSON() const;

private:
    stdx::unordered_map<std::string, std::string> _values;
};

}  // namespace traces
}  // namespace otel
}  // namespace mongo
