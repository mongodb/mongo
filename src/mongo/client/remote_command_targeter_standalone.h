// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

#include <vector>

namespace mongo {

/**
 * Implements a standalone instance remote command targeter, which always returns the same
 * host regardless of the read preferences.
 */
class RemoteCommandTargeterStandalone final : public RemoteCommandTargeter {
public:
    explicit RemoteCommandTargeterStandalone(const HostAndPort& hostAndPort);

    ConnectionString connectionString() override;

    StatusWith<HostAndPort> findHost(OperationContext* opCtx,
                                     const ReadPreferenceSetting& readPref,
                                     const TargetingMetadata& targetingMetadata) override;

    SemiFuture<HostAndPort> findHost(const ReadPreferenceSetting& readPref,
                                     const CancellationToken& cancelToken,
                                     const TargetingMetadata& targetingMetadata) override;


    SemiFuture<std::vector<HostAndPort>> findHosts(const ReadPreferenceSetting& readPref,
                                                   const TargetingMetadata& targetingMetadata,
                                                   const CancellationToken& cancelToken) override;

    void markHostNotPrimary(const HostAndPort& host, const Status& status) override;

    void markHostUnreachable(const HostAndPort& host, const Status& status) override;

    void markHostShuttingDown(const HostAndPort& host, const Status& status) override;

private:
    const HostAndPort _hostAndPort;
};

}  // namespace mongo
