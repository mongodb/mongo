// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/api_version_metrics.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/validate_api_parameters.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
static const auto handle = ServiceContext::declareDecoration<std::unique_ptr<APIVersionMetrics>>();

ServiceContext::ConstructorActionRegisterer registerAPIVersionMetrics{
    "CreateAPIVersionMetrics", [](ServiceContext* service) {
        APIVersionMetrics::set(service, std::make_unique<APIVersionMetrics>());
    }};
}  // namespace

APIVersionMetrics& APIVersionMetrics::get(ServiceContext* svc) {
    return *handle(svc);
}

void APIVersionMetrics::set(ServiceContext* service,
                            std::unique_ptr<APIVersionMetrics> apiVersionMetrics) {
    handle(service) = std::move(apiVersionMetrics);
}

void APIVersionMetrics::update(std::string_view appName, const APIParameters& apiParams) {
    Date_t now = getGlobalServiceContext()->getFastClockSource()->now();
    boost::optional<int> parsedVersion;

    auto appNameStr = std::string{appName};

    {
        auto sharedLock = _mutex.readLock();

        if (_apiVersionMetrics.size() >= kMaxNumOfSavedAppNames &&
            !_apiVersionMetrics.count(appNameStr)) {
            return;
        }

        auto versionOpt = apiParams.getAPIVersion();
        parsedVersion = versionOpt ? getAPIVersion(*versionOpt, true /* allowTestVersion */)
                                   : APIVersionMetrics::kVersionMetricsDefaultVersionPosition;

        auto appNameIter = _apiVersionMetrics.find(appNameStr);
        if (MONGO_likely(appNameIter != _apiVersionMetrics.end())) {
            appNameIter->second.timestamps[*parsedVersion].storeRelaxed(now);
            return;
        }
    }

    invariant(!!parsedVersion);

    auto writeLock = _mutex.writeLock();
    auto& timestamps = _apiVersionMetrics[appNameStr];
    timestamps.timestamps[*parsedVersion].storeRelaxed(now);
}

void APIVersionMetrics::_removeStaleTimestamps() {
    Date_t now = getGlobalServiceContext()->getFastClockSource()->now();

    auto timestampNotStale = [&](const Atomic<Date_t>& ts) {
        return ts.loadRelaxed() >= now - kTimestampStaleThreshold;
    };

    std::vector<std::string> staleAppNames;

    {
        auto sharedLock = _mutex.readLock();

        for (const auto& [appName, versionTimestamps] : _apiVersionMetrics) {
            if (MONGO_unlikely(!std::any_of(versionTimestamps.timestamps.begin(),
                                            versionTimestamps.timestamps.end(),
                                            timestampNotStale))) {
                staleAppNames.push_back(appName);
            }
        }
    }


    if (MONGO_unlikely(!staleAppNames.empty())) {
        auto writeLock = _mutex.writeLock();
        for (const auto& appName : staleAppNames) {
            _apiVersionMetrics.erase(appName);
        }
    }
}

void APIVersionMetrics::appendAPIVersionMetricsInfo(BSONObjBuilder* b) {
    _removeStaleTimestamps();
    _appendAPIVersionData(b);
}

void APIVersionMetrics::_appendAPIVersionData(BSONObjBuilder* b) const {
    auto sharedLock = _mutex.readLock();

    int numOfEntries = 0;
    for (const auto& [appName, versionTimestamps] : _apiVersionMetrics) {
        if (numOfEntries++ == kMaxNumOfOutputAppNames) {
            break;
        }

        BSONArrayBuilder subArrBuilder(b->subarrayStart(appName));

        if (versionTimestamps.timestamps[kVersionMetricsDefaultVersionPosition].loadRelaxed() !=
            Date_t::min()) {
            subArrBuilder.append("default");
        }

        if (versionTimestamps.timestamps[kVersionMetricsVersionOnePosition].loadRelaxed() !=
            Date_t::min()) {
            subArrBuilder.append("1");
        }

        // For now, Version 2 is test-only.
        if (versionTimestamps.timestamps[kVersionMetricsVersionTwoPosition].loadRelaxed() !=
            Date_t::min()) {
            subArrBuilder.append("2");
        }

        subArrBuilder.done();
    }
}

void APIVersionMetrics::appendAPIVersionMetricsInfo_forTest(BSONObjBuilder* b) {
    _removeStaleTimestamps();
    _appendAPIVersionData(b);
}

void APIVersionMetrics::cloneAPIVersionMetrics_forTest(
    APIVersionMetrics::VersionMetricsMap& toPopulate) {
    _removeStaleTimestamps();

    toPopulate.clear();

    auto sharedLock = _mutex.readLock();

    for (const auto& [appName, versionTimestamps] : _apiVersionMetrics) {

        auto& clonedVersionTimestamps = toPopulate[appName];
        for (size_t i = 0; i < versionTimestamps.timestamps.size(); ++i) {
            clonedVersionTimestamps.timestamps[i].storeRelaxed(
                versionTimestamps.timestamps[i].loadRelaxed());
        }
    }
}

namespace {
struct ApiVersionsMetricPolicy {
    void appendTo(BSONObjBuilder& b, std::string_view leafName) const {
        BSONObjBuilder bob{b.subobjStart(leafName)};
        auto&& instance = APIVersionMetrics::get(getGlobalServiceContext());
        instance.appendAPIVersionMetricsInfo(&bob);
    }
};
auto& apiVersionMetricsSSM = *CustomMetricBuilder<ApiVersionsMetricPolicy>{"apiVersions"};

}  // namespace
}  // namespace mongo
