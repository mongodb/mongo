// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/remote_command_targeter.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Constructs RemoteCommandTargeters based on the specific type of the target (standalone,
 * replica set, etc).
 */
class [[MONGO_MOD_PUBLIC]] RemoteCommandTargeterFactory {
    RemoteCommandTargeterFactory(const RemoteCommandTargeterFactory&) = delete;
    RemoteCommandTargeterFactory& operator=(const RemoteCommandTargeterFactory&) = delete;

public:
    virtual ~RemoteCommandTargeterFactory() = default;

    /**
     * Instantiates a RemoteCommandTargeter for the specified connection string.
     */
    virtual std::unique_ptr<RemoteCommandTargeter> create(const ConnectionString& connStr) = 0;

protected:
    RemoteCommandTargeterFactory() = default;
};

}  // namespace mongo
