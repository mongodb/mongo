/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/write_commands_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/s/commands/query_cmd/cluster_write_cmd.h"

#include <memory>
#include <set>
#include <string>

namespace mongo {
namespace {

struct ClusterInsertCmdS {
    static constexpr StringData kName = "insert"_sd;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static void doCheckAuthorization(AuthorizationSession* authzSession,
                                     bool bypass,
                                     const write_ops::InsertCommandRequest& op) {
        auth::checkAuthForInsertCommand(authzSession, bypass, op);
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
MONGO_REGISTER_COMMAND(ClusterInsertCmdBase<ClusterInsertCmdS>).forRouter();

struct ClusterUpdateCmdS {
    static constexpr StringData kName = "update"_sd;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static void doCheckAuthorization(AuthorizationSession* authzSession,
                                     bool bypass,
                                     const write_ops::UpdateCommandRequest& op) {
        auth::checkAuthForUpdateCommand(authzSession, bypass, op);
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
MONGO_REGISTER_COMMAND(ClusterUpdateCmdBase<ClusterUpdateCmdS>).forRouter();

struct ClusterDeleteCmdS {
    static constexpr StringData kName = "delete"_sd;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static void doCheckAuthorization(AuthorizationSession* authzSession,
                                     bool bypass,
                                     const write_ops::DeleteCommandRequest& op) {
        auth::checkAuthForDeleteCommand(authzSession, bypass, op);
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
MONGO_REGISTER_COMMAND(ClusterDeleteCmdBase<ClusterDeleteCmdS>).forRouter();

}  // namespace
}  // namespace mongo
