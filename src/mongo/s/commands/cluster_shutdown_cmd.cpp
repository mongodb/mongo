// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/duration.h"

#include <memory>
#include <string>

namespace mongo {
namespace {

class ClusterShutdownCmd : public CmdShutdown<ClusterShutdownCmd> {
public:
    std::string help() const override {
        return "Shuts down the mongos. Must be run against the admin database and either (1) run "
               "from localhost or (2) run while authenticated with the shutdown privilege. Spends "
               "'timeoutSecs' in quiesce mode, where the mongos continues to allow operations to "
               "run, but directs clients to route new operations to other mongos nodes.";
    }

    static void beginShutdown(OperationContext* opCtx, bool force, Milliseconds timeout) {}
};
MONGO_REGISTER_COMMAND(ClusterShutdownCmd).forRouter();

}  // namespace
}  // namespace mongo
