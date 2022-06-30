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
#include "mongo/db/commands/server_status.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {
static const auto handle = ServiceContext::declareDecoration<APIVersionMetrics>();
}  // namespace

APIVersionMetrics& APIVersionMetrics::get(ServiceContext* svc) {
    return handle(svc);
}

void APIVersionMetrics::update(std::string appName, const APIParameters& apiParams) {
    Date_t now = getGlobalServiceContext()->getFastClockSource()->now();
    stdx::lock_guard<Latch> lk(_mutex);
    if (apiParams.getAPIVersion()) {
        _apiVersionMetrics[appName][*apiParams.getAPIVersion()] = now;
    } else {
        _apiVersionMetrics[appName]["default"] = now;
    }
}

void APIVersionMetrics::_removeStaleTimestamps(WithLock lk, Date_t now) {
    for (auto appNameIter = _apiVersionMetrics.begin(); appNameIter != _apiVersionMetrics.end();) {
        auto& versionTimestamps = appNameIter->second;

        for (auto versionIter = versionTimestamps.begin();
             versionIter != versionTimestamps.end();) {
            auto timestamp = versionIter->second;

            // Remove any timestamps that are more than one day old.
            if (now - Days(1) > timestamp) {
                versionTimestamps.erase(versionIter++);
            } else {
                ++versionIter;
            }
        }

        // If there are no more timestamps for an application, remove the map associated with
        // application name.
        if (appNameIter->second.empty()) {
            _apiVersionMetrics.erase(appNameIter++);
        } else {
            ++appNameIter;
        }
    }
}

void APIVersionMetrics::appendAPIVersionMetricsInfo(BSONObjBuilder* b) {
    Date_t now = Date_t::now();
    stdx::lock_guard<Latch> lk(_mutex);

    _removeStaleTimestamps(lk, now);

    for (const auto& [appName, versionTimestamps] : _apiVersionMetrics) {
        BSONArrayBuilder subArrBuilder(b->subarrayStart(appName));

        if (versionTimestamps.find("default") != versionTimestamps.end()) {
            subArrBuilder.append("default");
        }

        if (versionTimestamps.find("1") != versionTimestamps.end()) {
            subArrBuilder.append("1");
        }

        // For now, Version 2 is test-only.
        if (versionTimestamps.find("2") != versionTimestamps.end()) {
            subArrBuilder.append("2");
        }

        subArrBuilder.done();
    }
}

APIVersionMetrics::APIVersionMetricsMap APIVersionMetrics::getAPIVersionMetrics_forTest() {
    Date_t now = getGlobalServiceContext()->getFastClockSource()->now();
    stdx::lock_guard<Latch> lk(_mutex);

    _removeStaleTimestamps(lk, now);
    return _apiVersionMetrics;
}

class APIVersionMetrics::APIVersionMetricsSSM : public ServerStatusMetric {
public:
    APIVersionMetricsSSM() : ServerStatusMetric("apiVersions") {}

    void appendAtLeaf(BSONObjBuilder& b) const override {
        BSONObjBuilder apiVersionBob(b.subobjStart(_leafName));
        APIVersionMetrics::get(getGlobalServiceContext())
            .appendAPIVersionMetricsInfo(&apiVersionBob);
        apiVersionBob.done();
    }
};

auto& apiVersionMetricsSSM =
    addMetricToTree(std::make_unique<APIVersionMetrics::APIVersionMetricsSSM>());

}  // namespace mongo
