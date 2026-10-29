// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
/**
 * Logs a warning message and throws if 'cmd' is not an allowed 'OP_QUERY' command.
 *
 * OP_QUERY commands are no longer serviced by mongos or mongod processes, with the following
 * exceptions.
 *
 * - The isMaster/ismaster/hello OP_QUERY commands are allowed for connection handshake.
 *
 * - The _isSelf/saslContinue/saslStart OP_QUERY commands are allowed for intra-cluster
 * communication in a mixed version cluster. V5.0 nodes may send those commands as OP_QUERY ones.
 */
[[MONGO_MOD_PUBLIC]] void checkAllowedOpQueryCommand(Client& client, std::string_view cmd);

}  // namespace mongo
