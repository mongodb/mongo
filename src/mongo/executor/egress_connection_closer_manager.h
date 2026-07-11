// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/executor/egress_connection_closer.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/transport/session.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <functional>
#include <mutex>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace executor {

/**
 * Manager for some number of EgressConnectionClosers, controlling dispatching to the managed
 * resources.
 *
 * The idea is that you own some semi-global EgressConnectionCloserManager which owns a bunch of
 * EgressConnectionClosers (which register themselves with it) and then interact exclusively with
 * the manager.
 */
class [[MONGO_MOD_PUBLIC]] EgressConnectionCloserManager {
public:
    EgressConnectionCloserManager() = default;

    static EgressConnectionCloserManager& get(ServiceContext* svc);

    void add(EgressConnectionCloser* etc);
    void remove(EgressConnectionCloser* etc);

    // Drops all closers' connections, deferring to their dropConnections().
    void dropConnections(const Status& status);
    void dropConnections();

    // Drops all connections associated with the HostAndPort on any closer.
    void dropConnections(const HostAndPort& target, const Status& status);
    void dropConnections(const HostAndPort& target);

    // Mark keep open on all connections associated with a HostAndPort on all closers.
    void setKeepOpen(const HostAndPort& hostAndPort, bool keepOpen);

private:
    std::mutex _mutex;
    stdx::unordered_set<EgressConnectionCloser*> _egressConnectionClosers;
};

}  // namespace executor
}  // namespace mongo
