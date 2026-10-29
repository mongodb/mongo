// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] async_rpc {

/**
 * This class serves as an adaptor that allows a mongo::RemoteCommandTargeter
 * to be used as a mongo::async_rpc::Targeter, so it can be used with the async_rpc
 * API.
 */
class AsyncRemoteCommandTargeterAdapter : public Targeter {
public:
    AsyncRemoteCommandTargeterAdapter(const ReadPreferenceSetting& readPref,
                                      std::shared_ptr<RemoteCommandTargeter> targeter)
        : _readPref(readPref), _targeter(std::move(targeter)) {}

    SemiFuture<HostAndPort> resolve(CancellationToken t,
                                    const TargetingMetadata& targetingMetadata) final {
        return _targeter->findHost(_readPref, t, targetingMetadata);
    }

    SemiFuture<void> onRemoteCommandError(HostAndPort remoteHost,
                                          Status remoteCommandStatus) final {
        _targeter->updateHostWithStatus(remoteHost, remoteCommandStatus);
        return SemiFuture<void>::makeReady();
    }

private:
    ReadPreferenceSetting _readPref;
    std::shared_ptr<RemoteCommandTargeter> _targeter;
};

}  // namespace async_rpc
}  // namespace mongo
