// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo::initialize_server_global_state {

/**
 * Returns whether the specified socket path is a directory.
 */
bool checkSocketPath();

/**
 * Attempts to write the PID file (if specified) and returns whether it was successful.
 */
bool writePidFile();

/**
 * Forks and detaches the server, on platforms that support it, if serverGlobalParams.doFork is
 * true.
 *
 * Call after processing the command line but before running mongo initializers.
 */
void forkServerOrDie();

/**
 * Notify the parent that we forked from that we have successfully completed basic
 * initialization so it can stop waiting and exit.
 */
void signalForkSuccess();

}  // namespace mongo::initialize_server_global_state
