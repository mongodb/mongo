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
#include "mongo/otel/traces/tracing_utils.h"
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
class MONGO_MOD_PARENT_PRIVATE BSONTextMapCarrier : public TextMapCarrier {
public:
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
