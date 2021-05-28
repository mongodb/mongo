/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_monitor_stats.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/is_mongos.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/timer.h"

namespace mongo {

/**
 * Interface for the ReplicaSetMonitorTransport. Implementations of this interface obtain a respone
 * to the 'hello' command with a provided timeout.
 */
class ReplicaSetMonitorTransport {
public:
    /**
     * Runs the 'hello' command on 'host' and returns the result. If the timeout value is reached,
     * the Future result will contain the error.
     */
    virtual Future<BSONObj> sayHello(HostAndPort host,
                                     const std::string& setName,
                                     const MongoURI& setUri,
                                     Milliseconds timeout,
                                     ReplicaSetMonitorStats* stats) noexcept = 0;
    virtual ~ReplicaSetMonitorTransport();
};
using ReplicaSetMonitorTransportPtr = std::unique_ptr<ReplicaSetMonitorTransport>;

/**
 * This class uses the DBClient object to make hello requests. It is utilized in unit tests
 * to accomodate mocking the hello responses.
 */
class ReplicaSetMonitorDbClientTransport : public ReplicaSetMonitorTransport {
public:
    Future<BSONObj> sayHello(HostAndPort host,
                             const std::string& setName,
                             const MongoURI& setUri,
                             Milliseconds timeout,
                             ReplicaSetMonitorStats* stats) noexcept override;
};

/**
 * This class is used in the production version of the server to obtain hello responses.
 * It uses an executor to run the hello commands with an appropriate timeout value. The
 * executor should be multithreaded to accomodate TCP requests that do not get a reply in a
 * timely fashion.
 */
class ReplicaSetMonitorExecutorTransport : public ReplicaSetMonitorTransport {
public:
    explicit ReplicaSetMonitorExecutorTransport(executor::TaskExecutor* executor);
    Future<BSONObj> sayHello(HostAndPort host,
                             const std::string& setName,
                             const MongoURI& setUri,
                             Milliseconds timeout,
                             ReplicaSetMonitorStats* stats) noexcept override;

private:
    void _haltIfIncompatibleServer(Status status);

    struct HelloRequest {
        HelloRequest(HostAndPort h, Promise<BSONObj>&& p)
            : host(h), timer(Timer()), promise(std::move(p)) {}
        HostAndPort host;
        Timer timer;
        Promise<BSONObj> promise;
    };

    executor::TaskExecutor* const _executor;
};
}
