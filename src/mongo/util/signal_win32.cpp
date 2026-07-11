// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/str.h"  // IWYU pragma: keep

#include <string>  // IWYU pragma: keep

namespace mongo {

// Generate windows event name for shutdown signal
std::string getShutdownSignalName(int processId) {
    const char* strEventNamePrefix = "Global\\Mongo_";

    return str::stream() << strEventNamePrefix << processId;
}

}  // namespace mongo
