// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
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

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

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
