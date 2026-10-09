// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/wiredtiger_metric_registry.h"

#include "mongo/base/status.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/otel_metric_name_validation.h"
#include "mongo/util/ctype.h"

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {

using otel::metrics::Counter;
using otel::metrics::DynamicMetricNameMaker;
using otel::metrics::Gauge;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

constexpr std::string_view kOtelNamePrefix = "mongodb.serverStatus.wiredTiger.";

constexpr std::string_view kLegacyStats[] = {
    "cache: eviction calls to get a page found queue empty",
    "cache: evict page attempts by eviction worker threads",
    "cache: evict page failures by eviction worker threads",
    "cache: page evict attempts by application threads",
    "cache: page evict failures by application threads",
    "cache: bytes read into cache",
    "cache: bytes written from cache",
    "cache: pages read into cache",
    "cache: pages requested from the cache",
    "cache: eviction empty score",
    "cache: eviction worker thread active",
    "cache: eviction worker thread stable number",
    "cache: bytes currently in the cache",
    "cache: tracked dirty bytes in the cache",
    "cache: maximum bytes configured",
    "data-handle: connection data handles currently active",
    "checkpoint: most recent time (msecs)",
};

// TODO (SERVER-136509): Remove this exclusion list when WiredTigerMetrics is retired.
bool isHandledByLegacyCollector(std::string_view statName) {
    for (auto legacyStat : kLegacyStats) {
        if (statName == legacyStat) {
            return true;
        }
    }
    return false;
}

/**
 * Keeps the alphanumerics and hyphens of one section of a WiredTiger stat name. Replaces other
 * characters (spaces, parentheses, ...) with a single underscore (unless it results in leading or
 * trailing underscore).
 */
std::string sanitizeSegment(std::string_view section) {
    std::string out;
    bool pendingUnderscore = false;
    for (char c : section) {
        if (ctype::isAlnum(c) || c == '-') {
            if (pendingUnderscore && !out.empty()) {
                out.push_back('_');
            }
            pendingUnderscore = false;
            out.push_back(ctype::toLower(c));
        } else {
            pendingUnderscore = true;
        }
    }
    return out;
}

}  // namespace

std::string wtStatNameToOtelName(std::string_view statName) {
    std::string out{kOtelNamePrefix};
    size_t pos = 0;
    // In theory wiredtiger stat names can have more than one colon.
    while (true) {
        // "cache: bytes read into cache" becomes
        // "cache.bytes_read_into_cache"
        size_t colon = statName.find(':', pos);
        auto segment =
            sanitizeSegment(colon == std::string_view::npos ? statName.substr(pos)
                                                            : statName.substr(pos, colon - pos));
        if (!segment.empty()) {
            if (out.back() != '.') {
                out.push_back('.');
            }
            out += segment;
        }
        if (colon == std::string_view::npos) {
            break;
        }
        pos = colon + 1;
    }
    return out;
}

namespace {

/**
 * Returns the OTel metric name to register 'statName' under, or none if it should not be
 * registered at alle.
 */
boost::optional<std::string> resolveOtelName(const std::string& statName) {
    if (isHandledByLegacyCollector(statName)) {
        return boost::none;
    }

    auto name = wtStatNameToOtelName(statName);
    if (Status s = otel::metrics::validateOtelMetricName(name); !s.isOK()) {
        LOGV2_DEBUG(13501000,
                    2,
                    "Skipping unsupported OTel WiredTiger metric name",
                    "stat"_attr = statName,
                    "error"_attr = s);
        return boost::none;
    }
    return name;
}

}  // namespace

Counter<int64_t>* WiredTigerMetricRegistry::_createCounter(const std::string& statName) {
    const auto name = resolveOtelName(statName);
    if (!name) {
        return nullptr;
    }
    return &MetricsService::instance().createInt64Counter(
        DynamicMetricNameMaker::make(std::string_view{*name}, dyn_metric_passkey()),
        statName,
        MetricUnit::kWiredTiger);
}

Gauge<int64_t>* WiredTigerMetricRegistry::_createGauge(const std::string& statName) {
    const auto name = resolveOtelName(statName);
    if (!name) {
        return nullptr;
    }
    return &MetricsService::instance().createInt64Gauge(
        DynamicMetricNameMaker::make(std::string_view{*name}, dyn_metric_passkey()),
        statName,
        MetricUnit::kWiredTiger);
}

}  // namespace mongo
