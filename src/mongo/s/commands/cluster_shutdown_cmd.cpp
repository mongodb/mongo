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
