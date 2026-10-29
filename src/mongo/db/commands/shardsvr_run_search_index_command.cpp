// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/shardsvr_run_search_index_command_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/search/search_index_common.h"
#include "mongo/db/query/search/search_index_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

/**
 * Test-only command for issueing search index commands on a shard (with optional view). Called from
 * search_index_testing_helper exclusively by routers.
 *
 * Requires the 'enableTestCommands' server parameter to be set. See docs/test_commands.md.
 *
 */
class ShardSvrRunSearchIndexCommand : public TypedCommand<ShardSvrRunSearchIndexCommand> {
public:
    using Request = ShardsvrRunSearchIndex;
    using Response = ShardsvrRunSearchIndexCommandReply;
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "allows shards to run search index command issued by a mongos with resolved view "
               "info";
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

public:
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            auto& cmd = request();
            generic_argument_util::prepareRequestForSearchIndexManagerPassthrough(cmd);
            Response res;

            auto alreadyInformedMongot = cmd.getMongotAlreadyInformed();
            bool cmdIsListSearchIx = std::string(cmd.getUserCmd().firstElement().fieldName())
                                         .compare("$listSearchIndexes") == 0;
            // mongos executes $listSearchIndexes on all hosts after multicasting the initial
            // create/update command, to ensure the initial command has completed and the index is
            // queryable. Therefore we want to allow running $listSearchIndexes on the shared mongot
            // in order to help ensure the index is queryable on all mongots.
            if (globalSearchIndexParams.host == alreadyInformedMongot && !cmdIsListSearchIx) {
                // This mongod shares its mongot with a mongos that originally received the user's
                // search index command. We therefore can return early as this mongod's mongot has
                // already been issued the create, drop, or update, search index command.
                res.setOk(1);
                return res;
            }

            auto catalog = CollectionCatalog::get(opCtx);
            auto resolvedNamespace = cmd.getResolvedNss();

            BSONObj manageSearchIndexResponse =
                getSearchIndexManagerResponse(opCtx,
                                              resolvedNamespace,
                                              *catalog->lookupUUIDByNSS(opCtx, resolvedNamespace),
                                              cmd.getUserCmd(),
                                              cmd.getView());

            auto searchIdxResp = SearchIndexManagerResponse::parse(
                manageSearchIndexResponse, IDLParserContext("_shardsvrRunSearchIndexCommand"));
            res.setSearchIndexManagerResponse(searchIdxResp);
            res.setOk(1.0);
            return res;
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        // No auth needed because a test-only command it exclusively enabled via command line.
        void doCheckAuthorization(OperationContext* opCtx) const override {}

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };
};
MONGO_REGISTER_COMMAND(ShardSvrRunSearchIndexCommand).testOnly().forShard();

}  // namespace

}  // namespace mongo
