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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/lock_manager/dump_lock_manager_impl.h"
#include "mongo/db/local_catalog/lock_manager/lock_info_gen.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/service_context.h"
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
