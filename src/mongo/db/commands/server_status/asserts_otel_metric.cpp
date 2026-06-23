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

#include "mongo/base/init.h"
#include "mongo/base/string_data.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/assert_util.h"

#include <array>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

// Adding a new AssertionKind triggers a -Wswitch warning here, which we treat as an error.
StringData kindToAttributeValue(AssertionKind kind) {
    switch (kind) {
        case AssertionKind::kRegular:
            return "regular"_sd;
        case AssertionKind::kMsg:
            return "msg"_sd;
        case AssertionKind::kUser:
            return "user"_sd;
        case AssertionKind::kTripwire:
            return "tripwire"_sd;
    }
    MONGO_UNREACHABLE_TASSERT(12846000);
}

constexpr std::array<StringData, 4> kKindAttributeValues = {
    "regular"_sd,
    "msg"_sd,
    "user"_sd,
    "tripwire"_sd,
};

// Single OTel counter that mirrors the per-type counters under `serverStatus.asserts.{kind}`. The
// legacy AssertionCount struct keeps the BSON serverStatus shape intact; this counter mirrors each
// increment with a `kind` attribute so consumers can slice by assertion kind.
//
// TODO (SERVER-129133): extend with a `command` attribute (the design discussion's
// "(c)" approach). That requires threading the running command through the assertion path plus a
// runtime knob to toggle the higher-cardinality variant.
auto& gAssertsCounter = otel::metrics::MetricsService::instance().createInt64Counter<StringData>(
    otel::metrics::MetricNames::kAsserts,
    "Number of assertion failures",
    otel::metrics::MetricUnit::kEvents,
    otel::metrics::AttributeDefinition<StringData>{
        .name = "kind",
        .values = std::vector<StringData>(kKindAttributeValues.begin(), kKindAttributeValues.end()),
    });

// Installs the observer that mirrors legacy AssertionCount increments into `gAssertsCounter`. The
// lambda is `noexcept` and wraps `add()` in a try/catch so an OTel-side failure cannot corrupt the
// in-flight assertion path. Assertions firing before this initializer runs (during static init of
// other TUs) skip the OTel side; the legacy AssertionCount field is still bumped unconditionally.
MONGO_INITIALIZER(InstallAssertsOtelObserver)(InitializerContext*) {
    setAssertionIncrementObserver([](AssertionKind kind) noexcept {
        try {
            gAssertsCounter.add(1, {kindToAttributeValue(kind)});
        } catch (const std::exception& e) {
            // gAssertsCounter.add() is not expected to throw. The catch itself is mandatory because
            // the observer is `noexcept` and must not let an exception escape the in-flight
            // assertion path.
            LOGV2_ERROR(12846001,
                        "Asserts OTel observer suppressed an unexpected exception",
                        "error"_attr = e.what());
        }
    });
}

}  // namespace
}  // namespace mongo
