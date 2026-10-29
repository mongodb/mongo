// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/remote_command_targeter_factory_impl.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_rs.h"
#include "mongo/client/remote_command_targeter_standalone.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <vector>

namespace mongo {

RemoteCommandTargeterFactoryImpl::RemoteCommandTargeterFactoryImpl() = default;

RemoteCommandTargeterFactoryImpl::~RemoteCommandTargeterFactoryImpl() = default;

std::unique_ptr<RemoteCommandTargeter> RemoteCommandTargeterFactoryImpl::create(
    const ConnectionString& connStr) {
    switch (connStr.type()) {
        case ConnectionString::ConnectionType::kStandalone:
        case ConnectionString::ConnectionType::kCustom:
            invariant(connStr.getServers().size() == 1);
            return std::make_unique<RemoteCommandTargeterStandalone>(connStr.getServers().front());
        case ConnectionString::ConnectionType::kReplicaSet:
            return std::make_unique<RemoteCommandTargeterRS>(connStr.getSetName(),
                                                             connStr.getServers());
        // These connections should never be seen
        case ConnectionString::ConnectionType::kInvalid:
        case ConnectionString::ConnectionType::kLocal:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
