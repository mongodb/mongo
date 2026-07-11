// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/platform/process_id.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <csignal>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>

namespace mongo {

class Scope;

namespace shell_utils {

// Scoped management of mongo program instances.  Simple implementation:
// destructor kills all mongod instances created by the shell.
struct MongoProgramScope {
    MongoProgramScope() {}  // Avoid 'unused variable' warning.
    ~MongoProgramScope();
};
int KillMongoProgramInstances(int signal = SIGTERM);

// Returns true if there are running child processes.
std::vector<ProcessId> getRunningMongoChildProcessIds();

// Returns a list of all process IDs, dead or alive.
std::vector<ProcessId> getRegisteredPidsHistory();

void installShellUtilsLauncher(Scope& scope);

}  // namespace shell_utils
}  // namespace mongo
