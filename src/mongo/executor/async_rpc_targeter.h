// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <vector>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] async_rpc {

class Targeter {
public:
    Targeter() = default;

    virtual ~Targeter() = default;

    /*
     * Returns a collection of possible Hosts on which the command may run based on the specific
     * settings (ReadPreference, etc.) of the targeter. Note that if no targets can be found on
     * which to run the command, the returned future should be set with an error - an empty vector
     * should never be returned and is treated as a programmer error.
     */
    virtual SemiFuture<HostAndPort> resolve(CancellationToken t,
                                            const TargetingMetadata& targetingMetadata) = 0;

    /*
     * Informs the Targeter that an error happened when trying to run a command on a
     * HostAndPort. Allows the targeter to update its view of the cluster's topology if network
     * or shutdown errors are received.
     */
    virtual SemiFuture<void> onRemoteCommandError(HostAndPort h, Status s) = 0;
};

class LocalHostTargeter : public Targeter {
public:
    LocalHostTargeter() = default;

    SemiFuture<HostAndPort> resolve(CancellationToken t, const TargetingMetadata&) final {
        HostAndPort h = HostAndPort("localhost", serverGlobalParams.port);
        return SemiFuture<HostAndPort>::makeReady(h);
    }

    SemiFuture<void> onRemoteCommandError(HostAndPort h, Status s) final {
        return SemiFuture<void>::makeReady();
    }
};

/**
 * Basic Targeter that wraps a single HostAndPort. Use when you need to make a call
 * to the sendCommand function but already know what HostAndPort to target.
 */
class FixedTargeter : public Targeter {
public:
    FixedTargeter(HostAndPort host) : _host(host) {};

    SemiFuture<HostAndPort> resolve(CancellationToken t, const TargetingMetadata&) final {
        return SemiFuture<HostAndPort>::makeReady(_host);
    }

    SemiFuture<void> onRemoteCommandError(HostAndPort h, Status s) final {
        return SemiFuture<void>::makeReady();
    }

private:
    HostAndPort _host;
};

}  // namespace async_rpc
}  // namespace mongo
