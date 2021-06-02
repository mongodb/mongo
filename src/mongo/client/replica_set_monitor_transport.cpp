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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/client/replica_set_monitor_transport.h"

#include "mongo/client/connpool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/future.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
const int kLogLevel = 2;
}

ReplicaSetMonitorTransport::~ReplicaSetMonitorTransport() {}


Future<BSONObj> ReplicaSetMonitorDbClientTransport::sayHello(
    HostAndPort host,
    const std::string& setName,
    const MongoURI& setUri,
    Milliseconds timeout,
    ReplicaSetMonitorStats* stats) noexcept {
    MongoURI targetURI;
    const auto& hostStr = host.toString();
    Timer timer;
    try {
        if (setUri.isValid()) {
            targetURI = setUri.cloneURIForServer(host, "");
            targetURI.setUser("");
            targetURI.setPassword("");
        } else {
            targetURI = MongoURI(ConnectionString(host));
        }

        LOG(kLogLevel) << "ReplicaSetMonitor " << setName << " sending hello request to "
                       << hostStr;
        const auto statsCollector = stats->collectHelloStats();
        ScopedDbConnection conn(targetURI, durationCount<Seconds>(timeout));
        bool ignoredOutParam = false;
        BSONObj reply;
        conn->isMaster(ignoredOutParam, &reply);
        conn.done();  // return to pool on success.
        LOG(kLogLevel) << "ReplicaSetMonitor " << setName << " received reply from " << hostStr
                       << ": " << reply.toString() << "("
                       << durationCount<Milliseconds>(Microseconds{timer.micros()}) << " ms)";
        return Future<BSONObj>(reply);
    } catch (const DBException& ex) {
        severe() << "ReplicaSetMonitor " << setName << " recieved error while monitoring "
                 << hostStr << ": " << ex.toStatus().toString() << "("
                 << durationCount<Milliseconds>(Microseconds{timer.micros()}) << " ms)";
        return Future<BSONObj>(ex.toStatus());
    }
}


ReplicaSetMonitorExecutorTransport::ReplicaSetMonitorExecutorTransport(
    executor::TaskExecutor* executor)
    : _executor(executor) {}

Future<BSONObj> ReplicaSetMonitorExecutorTransport::sayHello(
    HostAndPort host,
    const std::string& setName,
    const MongoURI& setUri,
    Milliseconds timeout,
    ReplicaSetMonitorStats* stats) noexcept {
    try {
        auto pf = makePromiseFuture<BSONObj>();
        BSONObjBuilder bob;
        bob.append("isMaster", 1);

        const auto& wireSpec = WireSpec::instance();
        if (wireSpec.isInternalClient) {
            WireSpec::appendInternalClientWireVersion(wireSpec.outgoing, &bob);
        }

        auto request = executor::RemoteCommandRequest(host, "admin", bob.obj(), nullptr, timeout);
        request.sslMode = setUri.getSSLMode();

        LOG(kLogLevel) << "Replica set monitor " << setName << " sending hello request to " << host;
        auto swCbHandle = _executor->scheduleRemoteCommand(std::move(request), [
            this,
            setName,
            requestState = std::make_shared<HelloRequest>(host, std::move(pf.promise)),
            statsCollector = stats->collectHelloStats()
        ](const executor::TaskExecutor::RemoteCommandCallbackArgs& result) mutable {
            LOG(kLogLevel) << "Replica set monitor " << setName << " received reply from "
                           << requestState->host.toString() << ": "
                           << (result.response.isOK() ? result.response.data.toString()
                                                      : result.response.status.toString())
                           << "(" << durationCount<Milliseconds>(
                                         Microseconds{requestState->timer.micros()})
                           << " ms)";

            _haltIfIncompatibleServer(result.response.status);

            if (result.response.isOK()) {
                requestState->promise.emplaceValue(result.response.data);
            } else {
                requestState->promise.setError(result.response.status);
            }
        });

        if (!swCbHandle.isOK()) {
            severe() << "Replica set monitor " << setName << " error while scheduling request to "
                     << host << ": " << swCbHandle.getStatus();
            return swCbHandle.getStatus();
        }

        return std::move(pf.future);
    } catch (const DBException& ex) {
        severe() << "ReplicaSetMonitor " << setName << " unexpected error while monitoring " << host
                 << ": " << ex.toString();
        return Status(ErrorCodes::InternalError, ex.toString());
    }
}

void ReplicaSetMonitorExecutorTransport::_haltIfIncompatibleServer(Status status) {
    if (mongo::isMongos() && status == ErrorCodes::IncompatibleWithUpgradedServer) {
        severe() << "This mongos server must be upgraded. It is attempting to "
                    "communicate with an upgraded cluster with which it is "
                    "incompatible. "
                 << "Error: '" << status << "' "
                 << "Crashing in order to bring attention to the incompatibility, "
                    "rather than erroring endlessly.";
        fassertNoTrace(5685401, false);
    }
}
}
