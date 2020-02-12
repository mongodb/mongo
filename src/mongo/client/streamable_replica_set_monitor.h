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

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class StreamableReplicaSetMonitor : public ReplicaSetMonitor {
    StreamableReplicaSetMonitor(const StreamableReplicaSetMonitor&) = delete;
    StreamableReplicaSetMonitor& operator=(const StreamableReplicaSetMonitor&) = delete;

public:
    StreamableReplicaSetMonitor(const MongoURI& uri);

    void init() override;

    void drop() override;

    SemiFuture<HostAndPort> getHostOrRefresh(
        const ReadPreferenceSetting& readPref,
        Milliseconds maxWait = kDefaultFindHostTimeout) override;

    SemiFuture<std::vector<HostAndPort>> getHostsOrRefresh(
        const ReadPreferenceSetting& readPref,
        Milliseconds maxWait = kDefaultFindHostTimeout) override;

    HostAndPort getMasterOrUassert() override;

    void failedHost(const HostAndPort& host, const Status& status) override;

    bool isPrimary(const HostAndPort& host) const override;

    bool isHostUp(const HostAndPort& host) const override;

    int getMinWireVersion() const override;

    int getMaxWireVersion() const override;

    std::string getName() const override;

    std::string getServerAddress() const override;

    const MongoURI& getOriginalUri() const override;

    bool contains(const HostAndPort& server) const override;

    void appendInfo(BSONObjBuilder& b, bool forFTDC = false) const override;

    bool isKnownToHaveGoodPrimary() const override;
};

}  // namespace mongo
