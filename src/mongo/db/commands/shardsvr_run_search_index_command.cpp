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

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/shardsvr_run_search_index_command_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/search/search_index_common.h"
#include "mongo/db/query/search/search_index_options.h"
#include "mongo/db/service_context.h"
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
