/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/s/chunk_splitter.h"

namespace mongo {

namespace {
class WaitForOngoingChunksSplitsCommand final : public BasicCommand {
    MONGO_DISALLOW_COPYING(WaitForOngoingChunksSplitsCommand);

public:
    WaitForOngoingChunksSplitsCommand() : BasicCommand("waitForOngoingChunkSplits") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "block until all autosplit chunk splits have completed on a shard. Test command "
               "only.";
    }

    bool requiresAuth() const override {
        return false;
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& db,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        ChunkSplitter::get(opCtx).waitForIdle();

        return true;
    }
};

MONGO_INITIALIZER(RegisterWaitForOngoingChunkSplitsCommand)(InitializerContext* context) {
    if (getTestCommandsEnabled()) {
        // Leaked intentionally: a Command registers itself when constructed.
        new WaitForOngoingChunksSplitsCommand();
    }
    return Status::OK();
}
}
}
