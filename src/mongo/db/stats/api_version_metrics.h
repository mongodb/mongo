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

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/time_support.h"

#include <array>
#include <ctime>
#include <string>

namespace mongo {

/**
 * A service context decoration that stores metrics related to the API version used by applications.
 */
class APIVersionMetrics {
public:
    // To ensure that the BSONObject doesn't exceed the size limit, the 'appName' field has a limit
    // of 128 bytes, which results in an output of approximately 128KB for app names.
    static constexpr int kMaxNumOfOutputAppNames = 1000;

    // To prevent unbounded memory usage, we limit the size of the saved app name to approximately
    // 384KB, as it is stored for 24 hours.
    static constexpr int kMaxNumOfSavedAppNames = kMaxNumOfOutputAppNames * 3;

    static constexpr Days kTimestampStaleThreshold = Days(1);

    static constexpr auto kVersionMetricsDefaultVersionPosition = 0;
    static constexpr auto kVersionMetricsVersionOnePosition = 1;
    static constexpr auto kVersionMetricsVersionTwoPosition = 2;

    struct VersionMetrics {
        std::array<Atomic<Date_t>, 3> timestamps{Date_t::min(), Date_t::min(), Date_t::min()};
    };

    typedef stdx::unordered_map<std::string, VersionMetrics> VersionMetricsMap;

    static APIVersionMetrics& get(ServiceContext* svc);
    static void set(ServiceContext* svc, std::unique_ptr<APIVersionMetrics> apiVersionMetrics);

    APIVersionMetrics() = default;

    // Update the timestamp for the API version used by the application.
    void update(StringData appName, const APIParameters& apiParams);

    void appendAPIVersionMetricsInfo(BSONObjBuilder* b);

    void cloneAPIVersionMetrics_forTest(VersionMetricsMap& toPopulate);

    void appendAPIVersionMetricsInfo_forTest(BSONObjBuilder* b);

private:
    void _removeStaleTimestamps();

    void _appendAPIVersionData(BSONObjBuilder* b) const;

    mutable WriteRarelyRWMutex _mutex;

    // For every application, for each API version, we store the most recent timestamp that a
    // command was invoked with that API version.
    VersionMetricsMap _apiVersionMetrics;
};

}  // namespace mongo
