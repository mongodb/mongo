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

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/executor/egress_connection_closer.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/transport/session.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/hostandport.h"

#include <functional>

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
class EgressConnectionCloserManager {
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
    stdx::mutex _mutex;
    stdx::unordered_set<EgressConnectionCloser*> _egressConnectionClosers;
};

}  // namespace executor
}  // namespace mongo
