// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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

            auto cmdObj = cmd.toBSON();
            // Creating the ShardsvrCreateCollectionRequest by parsing the {create..} bsonObj
            // guaratees to propagate the apiVersion and apiStrict paramers. Note that
            // shardsvrCreateCollection as internal command will skip the apiVersionCheck.
            // However in case of view, the create command might run an aggregation. Having those
            // fields propagated guaratees the api version check will keep working within the
            // aggregation framework
            auto request =
                ShardsvrCreateCollectionRequest::parse(cmdObj, IDLParserContext("create"));

            request.setUnsplittable(true);
            request.setShardKey(BSON("_id" << 1));

            shardsvrCollCommand.setShardsvrCreateCollectionRequest(request);
            shardsvrCollCommand.setDbName(nss.dbName());

            cluster::createCollection(opCtx, std::move(shardsvrCollCommand));
            return CreateCommandReply();
        }
    };
};
MONGO_REGISTER_COMMAND(CreateCmd).forRouter();

}  // namespace
}  // namespace mongo
