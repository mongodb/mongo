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
#include "mongo/db/commands/shardsvr_resolve_view_command_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {


class ShardSvrResolveViewCommand : public TypedCommand<ShardSvrResolveViewCommand> {
public:
    using Request = ShardsvrResolveView;
    using Response = ShardsvrResolveViewReply;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "resolves views as to allow shards to run search index command issued by a mongos "
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
            auto cmd = request();
            auto nss = cmd.getNss();
            auto catalog = CollectionCatalog::get(opCtx);

            // In sharded clusters, it's possible that the router has stale information
            // regarding where the primary shard is. Being that this shard holds the view
            // catalog (necessary for resolving the view), we must check that the databaseVersion
            // attached by the router is valid and establish a storage engine snapshot consistent
            // with it.
            AutoGetDbForReadMaybeLockFree lock(opCtx, cmd.getDbName());

            auto resolvedView = uassertStatusOK(
                view_catalog_helpers::resolveView(opCtx, catalog, nss, boost::none));
            Response res;
            res.setResolvedView(resolvedView);
            if (auto uuid = catalog->lookupUUIDByNSS(opCtx, resolvedView.getResolvedNamespace())) {
                res.setCollectionUUID(
                    catalog->lookupUUIDByNSS(opCtx, resolvedView.getResolvedNamespace()));
            }

            return res;
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }


        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };
};
MONGO_REGISTER_COMMAND(ShardSvrResolveViewCommand).forShard();

}  // namespace

}  // namespace mongo
