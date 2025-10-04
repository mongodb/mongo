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


#include "mongo/executor/egress_connection_closer_manager.h"

#include "mongo/util/decorable.h"
#include "mongo/util/net/hostandport.h"

#include <mutex>
#include <utility>

#include <absl/container/node_hash_set.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor


namespace mongo {
namespace executor {

const auto egressConnectionCloserManagerDecoration =
    ServiceContext::declareDecoration<EgressConnectionCloserManager>();

EgressConnectionCloserManager& EgressConnectionCloserManager::get(ServiceContext* svc) {
    return egressConnectionCloserManagerDecoration(svc);
}

void EgressConnectionCloserManager::add(EgressConnectionCloser* ecc) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _egressConnectionClosers.insert(ecc);
}

void EgressConnectionCloserManager::remove(EgressConnectionCloser* ecc) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _egressConnectionClosers.erase(ecc);
}

void EgressConnectionCloserManager::dropConnections(const Status& status) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    for (auto ecc : _egressConnectionClosers) {
        ecc->dropConnections(status);
    }
}

void EgressConnectionCloserManager::dropConnections() {
    dropConnections(
        Status(ErrorCodes::PooledConnectionsDropped, "Dropping all egress connections"));
}

void EgressConnectionCloserManager::dropConnections(const HostAndPort& target,
                                                    const Status& status) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    for (auto ecc : _egressConnectionClosers) {
        ecc->dropConnections(target, status);
    }
}

void EgressConnectionCloserManager::dropConnections(const HostAndPort& target) {
    dropConnections(
        target,
        Status(ErrorCodes::PooledConnectionsDropped, "Drop all egress connections with a target"));
}

void EgressConnectionCloserManager::setKeepOpen(const HostAndPort& hostAndPort, bool keepOpen) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    for (auto ecc : _egressConnectionClosers) {
        ecc->setKeepOpen(hostAndPort, keepOpen);
    }
}

}  // namespace executor
}  // namespace mongo
