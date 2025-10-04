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

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/otel/telemetry_context.h"

#include <memory>

namespace mongo {
namespace otel {

/**
 * TelemetryContextHolder is a decoration on OperationContext that holds the current
 * TelemetryContext. TelemetryContext is a wrapper for OpenTelemetry's Context that is used to
 * propagate parent / child relationships between Spans as well as hold metadata to correlate
 * various telemetry data.
 */
class TelemetryContextHolder {
public:
    static TelemetryContextHolder& get(OperationContext* opCtx);

    const std::shared_ptr<TelemetryContext>& get() {
        return _current;
    }
    void set(std::shared_ptr<TelemetryContext> context) {
        _current = std::move(context);
    }
    void clear() {
        _current.reset();
    }

private:
    std::shared_ptr<TelemetryContext> _current;
};

}  // namespace otel
}  // namespace mongo
