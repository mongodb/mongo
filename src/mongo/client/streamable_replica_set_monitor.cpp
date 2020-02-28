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

#include "mongo/platform/basic.h"

#include "mongo/client/streamable_replica_set_monitor.h"

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

StreamableReplicaSetMonitor::StreamableReplicaSetMonitor(const MongoURI& uri) {}
void StreamableReplicaSetMonitor::init() {}

void StreamableReplicaSetMonitor::drop() {}

SemiFuture<HostAndPort> StreamableReplicaSetMonitor::getHostOrRefresh(
    const ReadPreferenceSetting& readPref, Milliseconds maxWait) {
    MONGO_UNREACHABLE;
}

SemiFuture<std::vector<HostAndPort>> StreamableReplicaSetMonitor::getHostsOrRefresh(
    const ReadPreferenceSetting& readPref, Milliseconds maxWait) {
    MONGO_UNREACHABLE;
}

HostAndPort StreamableReplicaSetMonitor::getMasterOrUassert() {
    MONGO_UNREACHABLE;
}
void StreamableReplicaSetMonitor::failedHost(const HostAndPort& host, const Status& status) {}
bool StreamableReplicaSetMonitor::isPrimary(const HostAndPort& host) const {
    MONGO_UNREACHABLE;
}

bool StreamableReplicaSetMonitor::isHostUp(const HostAndPort& host) const {
    MONGO_UNREACHABLE;
}

int StreamableReplicaSetMonitor::getMinWireVersion() const {
    MONGO_UNREACHABLE;
}

int StreamableReplicaSetMonitor::getMaxWireVersion() const {
    MONGO_UNREACHABLE;
}

std::string StreamableReplicaSetMonitor::getName() const {
    MONGO_UNREACHABLE;
}

std::string StreamableReplicaSetMonitor::getServerAddress() const {
    MONGO_UNREACHABLE;
}

const MongoURI& StreamableReplicaSetMonitor::getOriginalUri() const {
    MONGO_UNREACHABLE;
};
bool StreamableReplicaSetMonitor::contains(const HostAndPort& server) const {
    MONGO_UNREACHABLE;
}

void StreamableReplicaSetMonitor::appendInfo(BSONObjBuilder& b, bool forFTDC) const {
    MONGO_UNREACHABLE;
}

bool StreamableReplicaSetMonitor::isKnownToHaveGoodPrimary() const {
    MONGO_UNREACHABLE;
}

void StreamableReplicaSetMonitor::runScanForMockReplicaSet() {
    MONGO_UNREACHABLE;
}

}  // namespace mongo
