/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>
#include <memory>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/search/search_index_command_testing_helper.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/commands/query_cmd/cluster_replicate_search_index_command_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

/**
 *
 * Requires the 'enableTestCommands' server parameter to be set. See docs/test_commands.md.
 * This command is called exclusively by server's mongot e2e test infrastructure.
 *
 */
class CmdReplicateSearchIndex : public TypedCommand<CmdReplicateSearchIndex> {
public:
    using Request = ReplicateSearchIndex;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto& cmd = request();
            auto& userCmd = cmd.getUserCmd();
            search_index_testing_helper::_replicateSearchIndexCommandOnAllMongodsForTesting(
                opCtx, cmd.getNamespace(), userCmd);
            return;
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        // No auth needed because a test-only command is exclusively enabled via command line.
        void doCheckAuthorization(OperationContext* opCtx) const override {}
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "Abort an in-progress move collection operation for this collection.";
    }
};
MONGO_REGISTER_COMMAND(CmdReplicateSearchIndex).testOnly().forRouter();

}  // namespace

}  // namespace mongo
