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

#include <memory>
#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"

namespace mongo {
namespace {

class CreateCmd final : public CreateCmdVersion1Gen<CreateCmd> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return false;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassertStatusOK(auth::checkAuthForCreate(
                opCtx, AuthorizationSession::get(opCtx->getClient()), request(), true));
        }

        CreateCommandReply typedRun(OperationContext* opCtx) final {
            auto cmd = request();
            auto dbName = cmd.getDbName();

            if (cmd.getClusteredIndex()) {
                clustered_util::checkCreationOptions(cmd);
            } else {
                uassert(ErrorCodes::InvalidOptions,
                        "specify size:<n> when capped is true",
                        !cmd.getCapped() || cmd.getSize());
            }

            uassert(ErrorCodes::InvalidOptions,
                    "the 'temp' field is an invalid option",
                    !cmd.getTemp());

            auto nss = cmd.getNamespace();
            ShardsvrCreateCollection shardsvrCollCommand(nss);

            auto cmdObj = cmd.toBSON({});
            // Creating the ShardsvrCreateCollectionRequest by parsing the {create..} bsonObj
            // guaratees to propagate the apiVersion and apiStrict paramers. Note that
            // shardsvrCreateCollection as internal command will skip the apiVersionCheck.
            // However in case of view, the create command might run an aggregation. Having those
            // fields propagated guaratees the api version check will keep working within the
            // aggregation framework
            auto request =
                ShardsvrCreateCollectionRequest::parse(IDLParserContext("create"), cmdObj);

            request.setUnsplittable(true);
            request.setShardKey(BSON("_id" << 1));

            shardsvrCollCommand.setShardsvrCreateCollectionRequest(request);
            shardsvrCollCommand.setDbName(nss.dbName());

            cluster::createCollection(opCtx, shardsvrCollCommand);
            return CreateCommandReply();
        }
    };
};
MONGO_REGISTER_COMMAND(CreateCmd).forRouter();

}  // namespace
}  // namespace mongo
