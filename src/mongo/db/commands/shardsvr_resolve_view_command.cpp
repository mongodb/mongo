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
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/shardsvr_resolve_view_command_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
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
            if (auto uuid = catalog->lookupUUIDByNSS(opCtx, resolvedView.getNamespace())) {
                res.setCollectionUUID(catalog->lookupUUIDByNSS(opCtx, resolvedView.getNamespace()));
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
