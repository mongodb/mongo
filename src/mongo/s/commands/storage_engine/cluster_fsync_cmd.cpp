// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fsync_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <utility>

namespace mongo {
namespace {

class FSyncCommand : public TypedCommand<FSyncCommand> {
public:
    class Response {
    public:
        explicit Response(BSONObj obj) : _obj{std::move(obj)} {}

        void serialize(BSONObjBuilder* builder) const {
            builder->appendElements(_obj);
        }

    private:
        BSONObj _obj;
    };

    using Request = FSyncRequest;

    std::string help() const override {
        return "invoke fsync on all shards belonging to the cluster";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return {};
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            if (!as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                    ActionType::fsync)) {
                uasserted(ErrorCodes::Unauthorized, "unauthorized");
            }
        }

        Response typedRun(OperationContext* opCtx) {
            if (request().getLock()) {
                request().setForBackup(true);
            }

            setReadWriteConcern(opCtx, request(), this);
            auto shardResults = scatterGatherUnversionedTargetConfigServerAndShards(
                opCtx,
                request().getDbName(),
                CommandHelpers::filterCommandRequestForPassthrough(request().toBSON()),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kIdempotent);

            std::string errmsg;
            BSONObjBuilder rawResult;
            const auto response = appendRawResponses(opCtx, &errmsg, &rawResult, shardResults);

            BSONObjBuilder result;
            // This field has had dummy value since MMAP went away. It is undocumented.
            // Maintaining it so as not to cause unnecessary user pain across upgrades.
            result.append("numFiles", 1);
            result.append("all", rawResult.obj());
            if (!response.responseOK) {
                if (request().getLock()) {
                    CommandHelpers::runCommandDirectly(
                        opCtx,
                        OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(opCtx),
                                                    request().getDbName(),
                                                    BSON("fsyncUnlock" << 1)));
                }
                CommandHelpers::appendSimpleCommandStatus(result, false, errmsg);
            }

            return Response{result.obj()};
        }
    };
};
MONGO_REGISTER_COMMAND(FSyncCommand).forRouter();

}  // namespace
}  // namespace mongo
