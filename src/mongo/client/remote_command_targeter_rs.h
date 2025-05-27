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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

class ReplicaSetMonitor;

/**
 * Implements a replica-set backed remote command targeter, which monitors the specified
 * replica set and responds to state changes.
 */
class RemoteCommandTargeterRS final : public RemoteCommandTargeter {
public:
    /**
     * Instantiates a new targeter for the specified replica set and seed hosts. The RS name
     * and the seed hosts must match.
     */
    RemoteCommandTargeterRS(const std::string& rsName, const std::vector<HostAndPort>& seedHosts);

    ConnectionString connectionString() override;

    StatusWith<HostAndPort> findHost(OperationContext* opCtx,
                                     const ReadPreferenceSetting& readPref) override;

    SemiFuture<std::vector<HostAndPort>> findHosts(const ReadPreferenceSetting& readPref,
                                                   const CancellationToken& cancelToken) override;

    SemiFuture<HostAndPort> findHost(const ReadPreferenceSetting& readPref,
                                     const CancellationToken& cancelToken) override;

    void markHostNotPrimary(const HostAndPort& host, const Status& status) override;

    void markHostUnreachable(const HostAndPort& host, const Status& status) override;

    void markHostShuttingDown(const HostAndPort& host, const Status& status) override;

private:
    /**
     * Returns true if the local host must be targeted, i.e. this is a shardsvr mongod targeting the
     * replica set it is in and the readPreference has been pre-targeted by the client connected to
     * it.
     */
    bool _mustTargetLocalHost(const ReadPreferenceSetting& readPref) const;

    ServiceContext* const _serviceContext;

    // Name of the replica set which this targeter maintains
    const std::string _rsName;

    // Monitor for this replica set
    std::shared_ptr<ReplicaSetMonitor> _rsMonitor;

    // Set to true if this is shardsvr mongod targeting the replica set it is in.
    bool _isTargetingLocalRS = false;
};

}  // namespace mongo
