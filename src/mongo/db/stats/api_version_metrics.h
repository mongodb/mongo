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

#include "mongo/db/api_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/time_support.h"

#include <ctime>

namespace mongo {

/**
 * A service context decoration that stores metrics related to the API version used by applications.
 */
class APIVersionMetrics {
public:
    using APIVersionMetricsMap =
        stdx::unordered_map<std::string, stdx::unordered_map<std::string, Date_t>>;

    static APIVersionMetrics& get(ServiceContext* svc);

    APIVersionMetrics() = default;

    // Update the timestamp for the API version used by the application.
    void update(std::string appName, const APIParameters& apiParams);

    void appendAPIVersionMetricsInfo(BSONObjBuilder* b);

    APIVersionMetricsMap getAPIVersionMetrics_forTest();

    class APIVersionMetricsSSM;

private:
    void _removeStaleTimestamps(WithLock lk, Date_t now);

    mutable Mutex _mutex = MONGO_MAKE_LATCH("APIVersionMetrics::_mutex");

    // Map of maps for API version metrics. For every application, for each API version, we store
    // the most recent timestamp that a command was invoked with that API version.
    APIVersionMetricsMap _apiVersionMetrics;
};

}  // namespace mongo
