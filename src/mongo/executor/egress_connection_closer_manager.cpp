// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
    std::lock_guard<std::mutex> lk(_mutex);

    _egressConnectionClosers.insert(ecc);
}

void EgressConnectionCloserManager::remove(EgressConnectionCloser* ecc) {
    std::lock_guard<std::mutex> lk(_mutex);

    _egressConnectionClosers.erase(ecc);
}

void EgressConnectionCloserManager::dropConnections(const Status& status) {
    std::lock_guard<std::mutex> lk(_mutex);

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
    std::lock_guard<std::mutex> lk(_mutex);

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
    std::lock_guard<std::mutex> lk(_mutex);

    for (auto ecc : _egressConnectionClosers) {
        ecc->setKeepOpen(hostAndPort, keepOpen);
    }
}

}  // namespace executor
}  // namespace mongo
