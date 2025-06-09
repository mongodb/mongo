/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/stats/api_version_metrics.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/validate_api_parameters.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <memory>
#include <mutex>
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

void APIVersionMetrics::update(StringData appName, const APIParameters& apiParams) {
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
    void appendTo(BSONObjBuilder& b, StringData leafName) const {
        BSONObjBuilder bob{b.subobjStart(leafName)};
        auto&& instance = APIVersionMetrics::get(getGlobalServiceContext());
        instance.appendAPIVersionMetricsInfo(&bob);
    }
};
auto& apiVersionMetricsSSM = *CustomMetricBuilder<ApiVersionsMetricPolicy>{"apiVersions"};

}  // namespace
}  // namespace mongo
