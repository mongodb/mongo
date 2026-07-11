// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <string>

namespace mongo {

#ifdef _WIN32
// Generate windows event name for shutdown signal
std::string getShutdownSignalName(int processId);
#endif
}  // namespace mongo
