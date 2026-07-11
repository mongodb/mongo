// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/assert_util.h"

#include <array>
#include <string_view>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

// Adding a new AssertionKind triggers a -Wswitch warning here, which we treat as an error.
std::string_view kindToAttributeValue(AssertionKind kind) {
    switch (kind) {
        case AssertionKind::kRegular:
            return "regular"sv;
        case AssertionKind::kMsg:
            return "msg"sv;
        case AssertionKind::kUser:
            return "user"sv;
        case AssertionKind::kTripwire:
            return "tripwire"sv;
    }
    MONGO_UNREACHABLE_TASSERT(12846000);
}

constexpr std::array<std::string_view, 4> kKindAttributeValues = {
    "regular"sv,
    "msg"sv,
    "user"sv,
    "tripwire"sv,
};

// Single OTel counter that mirrors the per-type counters under `serverStatus.asserts.{kind}`. The
// legacy AssertionCount struct keeps the BSON serverStatus shape intact; this counter mirrors each
// increment with a `kind` attribute so consumers can slice by assertion kind.
//
// TODO (SERVER-129133): extend with a `command` attribute (the design discussion's
// "(c)" approach). That requires threading the running command through the assertion path plus a
// runtime knob to toggle the higher-cardinality variant.
auto& gAssertsCounter =
    otel::metrics::MetricsService::instance().createInt64Counter<std::string_view>(
        otel::metrics::MetricNames::kAsserts,
        "Number of assertion failures",
        otel::metrics::MetricUnit::kEvents,
        otel::metrics::AttributeDefinition<std::string_view>{
            .name = "kind",
            .values = std::vector<std::string_view>(kKindAttributeValues.begin(),
                                                    kKindAttributeValues.end()),
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
