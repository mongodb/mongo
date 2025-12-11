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

#include "mongo/base/string_data.h"
#include "mongo/config.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/meter.h>


namespace mongo::otel::metrics {

/**
 * The MetricsService is the external interface by which API consumers can create Instruments. The
 * global MeterProvider must be set before ServiceContext construction to ensure that the meter can
 * be properly initialized.
 */
class MONGO_MOD_PUBLIC MetricsService {
public:
    static constexpr StringData kMeterName = "mongodb";

    static MetricsService& get(ServiceContext*);

    MetricsService();

    // TODO SERVER-114945 Remove this method once we can validate meter construction succeeded via
    // the Instruments it produces
    opentelemetry::metrics::Meter* getMeter_forTest() const {
        return _meter.get();
    }

    // TODO SERVER-114945 Add MetricsService::createUInt64Counter method
    // TODO SERVER-114954 Implement MetricsService::createUInt64Gauge
    // TODO SERVER-114955 Implement MetricsService::createDoubleGauge
    // TODO SERVER-115164 Implement MetricsService::createHistogram method

private:
    std::shared_ptr<opentelemetry::metrics::Meter> _meter{nullptr};
};
}  // namespace mongo::otel::metrics
#else
namespace mongo::otel::metrics {
class MONGO_MOD_PUBLIC MetricsService {
public:
    static constexpr StringData kMeterName = "mongodb";

    static MetricsService& get(ServiceContext*);

    MetricsService();
};
}  // namespace mongo::otel::metrics
#endif
