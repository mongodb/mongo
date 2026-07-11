// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <string>
#include <string_view>
#include <vector>

namespace mongo {
namespace repl {
/**
 * Parses command line `--replSet <expr>` expressions into the setname and a collection of host:port
 * objects, formatted as <setName>/<host:port>[,<host:port>...]
 *
 * Example: `mySet/host1:10001,host2:10002`
 */
[[MONGO_MOD_PUBLIC]] std::tuple<std::string, std::vector<HostAndPort>> parseReplSetSeedList(
    ReplicationCoordinatorExternalState* externalState, std::string_view replSetString);
}  // namespace repl
}  // namespace mongo
