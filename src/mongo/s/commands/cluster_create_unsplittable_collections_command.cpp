/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/commands/shard_collection_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {
Status checkAuth(OperationContext* opCtx,
                 AuthorizationSession* authSession,
                 const CreateUnsplittableCollection& cmd,
                 bool isMongos) {
    auto ns = cmd.getNamespace();

    const bool hasCreateCollectionAction =
        authSession->isAuthorizedForActionsOnNamespace(ns, ActionType::createCollection);

    // To create a regular collection, ActionType::createCollection or ActionType::insert are
    // both acceptable.
    if (hasCreateCollectionAction ||
        authSession->isAuthorizedForActionsOnNamespace(ns, ActionType::insert)) {
        return Status::OK();
    }

    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

class CreateUnsplittableCollectionCommand final
    : public TypedCommand<CreateUnsplittableCollectionCommand> {
public:
    using Request = CreateUnsplittableCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto nss = ns();
            const auto& req = request();

            uassert(7876301,
                    "The tracking of unsharded collections doesn't support time series buckets "
                    "collection yet",
                    !nss.isTimeseriesBucketsCollection());

            uassert(
                7876302,
                "The tracking of unsharded collections doesn't support Queryable Encryption state "
                "collection yet",
                !nss.isFLE2StateCollection());

            ShardsvrCreateCollection shardsvrCollRequest(nss);
            auto svrRequest = ShardsvrCreateCollectionRequest::parse(
                IDLParserContext("createUnsplittableCollection"), req.toBSON({}));
            svrRequest.setShardKey(BSON("_id" << 1));
            svrRequest.setUnsplittable(true);
            svrRequest.setDataShard(req.getDataShard());
            svrRequest.setTimeseries(req.getTimeseries());
            svrRequest.setIsFromCreateUnsplittableCollectionTestCommand(true);
            shardsvrCollRequest.setDbName(nss.dbName());
            shardsvrCollRequest.setShardsvrCreateCollectionRequest(svrRequest);
            cluster::createCollection(opCtx, shardsvrCollRequest);
        }

    private:
        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassertStatusOK(
                checkAuth(opCtx, AuthorizationSession::get(opCtx->getClient()), request(), true));
        }
    };


    std::string help() const override {
        return "Internal command for testing resharding collection cloning";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool allowedInTransactions() const final {
        return true;
    }
};

MONGO_REGISTER_COMMAND(CreateUnsplittableCollectionCommand).testOnly().forRouter();

}  // namespace
}  // namespace mongo
