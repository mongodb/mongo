// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/dump_lock_manager_impl.h"
#include "mongo/db/shard_role/lock_manager/lock_info_gen.h"
#include "mongo/db/shard_role/lock_manager/lock_manager.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/storage/storage_engine.h"

#include <string>

namespace mongo {
namespace {

class CmdLockInfo : public TypedCommand<CmdLockInfo> {
public:
    using Request = LockInfoCommand;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "show all lock info on the server";
    }

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::serverStatus));
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            AutoStatsTracker statsTracker(opCtx,
                                          ns(),
                                          Top::LockType::NotLocked,
                                          AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                          DatabaseProfileSettings::get(opCtx->getServiceContext())
                                              .getDatabaseProfileLevel(request().getDbName()));

            auto lockToClientMap = getLockerIdToClientMap(opCtx->getServiceContext());

            auto lockManager = LockManager::get(opCtx->getServiceContext());
            auto result = reply->getBodyBuilder();
            auto lockInfoArr = BSONArrayBuilder(result.subarrayStart("lockInfo"));
            lockManager->getLockInfoArray(lockToClientMap, false, lockManager, &lockInfoArr);
            const auto& includeStorageEngineDump = request().getIncludeStorageEngineDump();
            if (includeStorageEngineDump) {
                opCtx->getServiceContext()->getStorageEngine()->dump();
            }
        }
    };
};
MONGO_REGISTER_COMMAND(CmdLockInfo).forShard();

}  // namespace
}  // namespace mongo
