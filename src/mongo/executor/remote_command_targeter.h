/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include <vector>

namespace mongo {
namespace executor {
namespace remote_command_runner {

class RemoteCommandHostTargeter {
public:
    RemoteCommandHostTargeter() = default;

    virtual ~RemoteCommandHostTargeter() = default;

    /*
     * Returns a collection of possible Hosts on which the command may run based on the specific
     * settings (ReadPreference, etc.) of the targeter.
     */
    virtual SemiFuture<std::vector<HostAndPort>> resolve(CancellationToken t) = 0;

    /*
     * Informs the RemoteHostTargeter that an error happened when trying to run a command on a
     * HostAndPort. Allows the targeter to update its view of the cluster's topology if network
     * or shutdown errors are recieved.
     */
    virtual void onRemoteCommandError(HostAndPort h, Status s) = 0;
};

class RemoteCommandLocalHostTargeter : public RemoteCommandHostTargeter {
public:
    RemoteCommandLocalHostTargeter() = default;

    SemiFuture<std::vector<HostAndPort>> resolve(CancellationToken t) override final {
        HostAndPort h = HostAndPort("localhost", serverGlobalParams.port);
        std::vector<HostAndPort> hostList{h};

        return SemiFuture<std::vector<HostAndPort>>::makeReady(hostList);
    }

    void onRemoteCommandError(HostAndPort h, Status s) override final {}
};
}  // namespace remote_command_runner
}  // namespace executor
}  // namespace mongo
