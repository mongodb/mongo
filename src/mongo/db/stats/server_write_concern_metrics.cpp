/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/stats/server_write_concern_metrics.h"

#include "mongo/db/commands/server_status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"

namespace mongo {

// opWriteConcernCounters are not tracked unless 'reportOpWriteConcernCountersInServerStatus' is
// true. This is false by default because the mutex acquisition impacts performance when SSL is
// disabled. (When SSL is enabled, other mutexes dominate.)
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(reportOpWriteConcernCountersInServerStatus, bool, false);

namespace {
const auto ServerWriteConcernMetricsDecoration =
    ServiceContext::declareDecoration<ServerWriteConcernMetrics>();
}  // namespace

ServerWriteConcernMetrics* ServerWriteConcernMetrics::get(ServiceContext* service) {
    return &ServerWriteConcernMetricsDecoration(service);
}

ServerWriteConcernMetrics* ServerWriteConcernMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void ServerWriteConcernMetrics::recordWriteConcernForInserts(
    const WriteConcernOptions& writeConcernOptions, size_t numInserts) {
    if (!reportOpWriteConcernCountersInServerStatus) {
        return;
    }

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _insertMetrics.recordWriteConcern(writeConcernOptions, numInserts);
}

void ServerWriteConcernMetrics::recordWriteConcernForUpdate(
    const WriteConcernOptions& writeConcernOptions) {
    if (!reportOpWriteConcernCountersInServerStatus) {
        return;
    }

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _updateMetrics.recordWriteConcern(writeConcernOptions);
}

void ServerWriteConcernMetrics::recordWriteConcernForDelete(
    const WriteConcernOptions& writeConcernOptions) {
    if (!reportOpWriteConcernCountersInServerStatus) {
        return;
    }

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _deleteMetrics.recordWriteConcern(writeConcernOptions);
}

BSONObj ServerWriteConcernMetrics::toBSON() const {
    if (!reportOpWriteConcernCountersInServerStatus) {
        return BSONObj();
    }

    stdx::lock_guard<stdx::mutex> lg(_mutex);

    BSONObjBuilder builder;

    BSONObjBuilder insertBuilder(builder.subobjStart("insert"));
    _insertMetrics.toBSON(&insertBuilder);
    insertBuilder.done();

    BSONObjBuilder updateBuilder(builder.subobjStart("update"));
    _updateMetrics.toBSON(&updateBuilder);
    updateBuilder.done();

    BSONObjBuilder deleteBuilder(builder.subobjStart("delete"));
    _deleteMetrics.toBSON(&deleteBuilder);
    deleteBuilder.done();

    return builder.obj();
}

void ServerWriteConcernMetrics::WriteConcernMetricsForOperationType::recordWriteConcern(
    const WriteConcernOptions& writeConcernOptions, size_t numOps) {
    if (writeConcernOptions.usedDefaultW) {
        noWCount += numOps;
        return;
    }

    if (!writeConcernOptions.wMode.empty()) {
        if (writeConcernOptions.wMode == WriteConcernOptions::kMajority) {
            wMajorityCount += numOps;
            return;
        }

        wTagCounts[writeConcernOptions.wMode] += numOps;
        return;
    }

    wNumCounts[writeConcernOptions.wNumNodes] += numOps;
}

void ServerWriteConcernMetrics::WriteConcernMetricsForOperationType::toBSON(
    BSONObjBuilder* builder) const {
    builder->append("wmajority", wMajorityCount);

    BSONObjBuilder wNumBuilder(builder->subobjStart("wnum"));
    for (auto const& pair : wNumCounts) {
        wNumBuilder.append(std::to_string(pair.first), pair.second);
    }
    wNumBuilder.done();

    BSONObjBuilder wTagBuilder(builder->subobjStart("wtag"));
    for (auto const& pair : wTagCounts) {
        wTagBuilder.append(pair.first, pair.second);
    }
    wTagBuilder.done();

    builder->append("none", noWCount);
}

namespace {
class OpWriteConcernCountersSSS : public ServerStatusSection {
public:
    OpWriteConcernCountersSSS() : ServerStatusSection("opWriteConcernCounters") {}

    ~OpWriteConcernCountersSSS() override = default;

    bool includeByDefault() const override {
        // When 'reportOpWriteConcernCountersInServerStatus' is false, do not include this section
        // unless requested by the user. Even if the user requests the section, it will not be
        // included because an empty BSONObj is generated for the section.
        return reportOpWriteConcernCountersInServerStatus;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        return ServerWriteConcernMetrics::get(opCtx)->toBSON();
    }

} opWriteConcernCountersSSS;
}  // namespace

}  // namespace mongo
