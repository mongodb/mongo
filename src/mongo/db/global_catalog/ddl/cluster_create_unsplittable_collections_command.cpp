// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/ddl/shard_collection_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"

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
                req.toBSON(), IDLParserContext("createUnsplittableCollection"));
            svrRequest.setShardKey(BSON("_id" << 1));
            svrRequest.setUnsplittable(true);
            svrRequest.setDataShard(req.getDataShard());
            svrRequest.setTimeseries(req.getTimeseries());
            svrRequest.setIsFromCreateUnsplittableCollectionTestCommand(true);
            shardsvrCollRequest.setDbName(nss.dbName());
            shardsvrCollRequest.setShardsvrCreateCollectionRequest(svrRequest);
            cluster::createCollection(opCtx, std::move(shardsvrCollRequest));
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
