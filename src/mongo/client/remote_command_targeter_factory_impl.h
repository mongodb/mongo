// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory.h"
#include "mongo/util/modules.h"

#include <memory>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Targeter factory that instantiates remote command targeters based on the type of the
 * connection. It will return RemoteCommandTargeterStandalone for a single node (kStandalone) or
 * custom (kCustom) connection string and RemoteCommandTargeterRS for a kReplicaSet connection
 * string. All other connection strings are not supported and will cause a failed invariant error.
 */
class RemoteCommandTargeterFactoryImpl final : public RemoteCommandTargeterFactory {
public:
    RemoteCommandTargeterFactoryImpl();
    ~RemoteCommandTargeterFactoryImpl() override;

    std::unique_ptr<RemoteCommandTargeter> create(const ConnectionString& connStr) override;
};

}  // namespace mongo
