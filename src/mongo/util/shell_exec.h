// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Execute a shell command and return its output.
 * Returns CommandExecutionFailure on non-zero exit code.
 */
StatusWith<std::string> shellExec(const std::string&,
                                  Milliseconds timeout,
                                  size_t maxlen,
                                  bool ignoreExitCode = false);

}  // namespace mongo
