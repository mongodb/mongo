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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include <fmt/format.h>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/user_management_commands_common.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {

using std::string;
using std::stringstream;
using std::vector;
using namespace fmt::literals;

namespace {

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                // Note: Even though we're setting UNSET here,
                                                // kMajority implies JOURNAL if journaling is
                                                // supported by this mongod.
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kWriteConcernTimeoutSharding);

template <typename Request>
void uassertEmptyReply(BSONObj obj) {
    uassert(ErrorCodes::BadValue,
            "Received unexpected response from {} command: {}"_format(Request::kCommandName,
                                                                      tojson(obj)),
            (obj.nFields() == 1) && obj["ok"]);
}

template <typename Request, typename Reply>
Reply parseUMCReply(BSONObj obj) try {
    return Reply::parse(IDLParserErrorContext(Request::kCommandName), obj);
} catch (const AssertionException& ex) {
    uasserted(ex.code(),
              "Received invalid response from {} command: {}, error: {}"_format(
                  Request::kCommandName, tojson(obj), ex.reason()));
}

struct UserCacheInvalidatorNOOP {
    static constexpr bool kRequireUserName = false;
    static void invalidate(OperationContext*, StringData) {}
};
struct UserCacheInvalidatorUser {
    static constexpr bool kRequireUserName = true;
    static void invalidate(OperationContext* opCtx, const UserName& userName) {
        AuthorizationManager::get(opCtx->getServiceContext())
            ->invalidateUserByName(opCtx, userName);
    }
};
struct UserCacheInvalidatorDB {
    static constexpr bool kRequireUserName = false;
    static void invalidate(OperationContext* opCtx, StringData dbname) {
        AuthorizationManager::get(opCtx->getServiceContext())->invalidateUsersFromDB(opCtx, dbname);
    }
};
struct UserCacheInvalidatorAll {
    static constexpr bool kRequireUserName = false;
    static void invalidate(OperationContext* opCtx, StringData) {
        AuthorizationManager::get(opCtx->getServiceContext())->invalidateUserCache(opCtx);
    }
};

/**
 * Most user management commands follow a very predictable pattern:
 * 1. Proxy command to config servers.
 * 2. Invalidate whatever we were just working on.
 * 3. Panic if anything went wrong.
 */
template <typename RequestT, typename ReplyT, typename InvalidatorT>
class CmdUMCPassthrough : public TypedCommand<CmdUMCPassthrough<RequestT, ReplyT, InvalidatorT>> {
public:
    using Request = RequestT;
    using Reply = ReplyT;
    using TC = TypedCommand<CmdUMCPassthrough<RequestT, ReplyT, InvalidatorT>>;

    class Invocation final : public TC::InvocationBase {
    public:
        using TC::InvocationBase::InvocationBase;
        using TC::InvocationBase::request;

        Reply typedRun(OperationContext* opCtx) {
            const auto& cmd = request();

            BSONObjBuilder builder;
            auto status = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
                opCtx,
                Request::kCommandName,
                cmd.getDbName(),
                applyReadWriteConcern(
                    opCtx,
                    this,
                    CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON({}))),
                &builder);

            if constexpr (InvalidatorT::kRequireUserName) {
                InvalidatorT::invalidate(opCtx,
                                         UserName(cmd.getCommandParameter(), cmd.getDbName()));
            } else {
                InvalidatorT::invalidate(opCtx, cmd.getDbName());
            }

            uassertStatusOK(status);

            if constexpr (std::is_void_v<Reply>) {
                uassertEmptyReply<Request>(builder.obj());
            } else {
                return parseUMCReply<Request, Reply>(builder.obj());
            }
        }

    private:
        bool supportsWriteConcern() const final {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auth::checkAuthForTypedCommand(opCtx->getClient(), request());
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return TC::AllowedOnSecondary::kNever;
    }
};

class CmdCreateUser : public CmdUMCPassthrough<CreateUserCommand, void, UserCacheInvalidatorNOOP> {
public:
    static constexpr StringData kPwdField = "pwd"_sd;
    StringData sensitiveFieldName() const final {
        return kPwdField;
    }
} cmdCreateUser;

class CmdUpdateUser : public CmdUMCPassthrough<UpdateUserCommand, void, UserCacheInvalidatorUser> {
public:
    static constexpr StringData kPwdField = "pwd"_sd;
    StringData sensitiveFieldName() const final {
        return kPwdField;
    }
} cmdUpdateUser;

CmdUMCPassthrough<DropUserCommand, void, UserCacheInvalidatorUser> cmdDropUser;
CmdUMCPassthrough<DropAllUsersFromDatabaseCommand,
                  DropAllUsersFromDatabaseReply,
                  UserCacheInvalidatorDB>
    cmdDropAllUsersFromDatabase;
CmdUMCPassthrough<GrantRolesToUserCommand, void, UserCacheInvalidatorUser> cmdGrantRolesToUser;
CmdUMCPassthrough<RevokeRolesFromUserCommand, void, UserCacheInvalidatorUser>
    cmdRevokeRolesFromUser;
CmdUMCPassthrough<CreateRoleCommand, void, UserCacheInvalidatorNOOP> cmdCreateRole;
CmdUMCPassthrough<UpdateRoleCommand, void, UserCacheInvalidatorAll> cmdUpdateRole;
CmdUMCPassthrough<GrantPrivilegesToRoleCommand, void, UserCacheInvalidatorAll>
    cmdGrantPrivilegesToRole;
CmdUMCPassthrough<RevokePrivilegesFromRoleCommand, void, UserCacheInvalidatorAll>
    cmdRevokePrivilegesFromRole;
CmdUMCPassthrough<GrantRolesToRoleCommand, void, UserCacheInvalidatorAll> cmdGrantRolesToRole;
CmdUMCPassthrough<RevokeRolesFromRoleCommand, void, UserCacheInvalidatorAll> cmdRevokeRolesFromRole;
CmdUMCPassthrough<DropRoleCommand, void, UserCacheInvalidatorAll> cmdDropRole;
CmdUMCPassthrough<DropAllRolesFromDatabaseCommand,
                  DropAllRolesFromDatabaseReply,
                  UserCacheInvalidatorAll>
    cmdDropAllRolesFromDatabase;

/**
 * usersInfo and rolesInfo are simpler read-only passthrough commands.
 */
template <typename RequestT, typename ReplyT>
class CmdUMCInfo : public TypedCommand<CmdUMCInfo<RequestT, ReplyT>> {
public:
    using Request = RequestT;
    using Reply = ReplyT;
    using TC = TypedCommand<CmdUMCInfo<RequestT, ReplyT>>;

    class Invocation final : public TC::InvocationBase {
    public:
        using TC::InvocationBase::InvocationBase;
        using TC::InvocationBase::request;

        Reply typedRun(OperationContext* opCtx) {
            const auto& cmd = request();

            BSONObjBuilder builder;
            const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
                opCtx,
                cmd.getDbName().toString(),
                applyReadWriteConcern(
                    opCtx,
                    this,
                    CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON({}))),
                &builder);

            auto result = builder.obj();
            if (!ok) {
                uassertStatusOK(getStatusFromCommandResult(result));
            }

            return parseUMCReply<Request, Reply>(result);
        }

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auth::checkAuthForTypedCommand(opCtx->getClient(), request());
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return TC::AllowedOnSecondary::kOptIn;
    }
};

CmdUMCInfo<UsersInfoCommand, UsersInfoReply> cmdUsersInfo;
CmdUMCInfo<RolesInfoCommand, RolesInfoReply> cmdRolesInfo;

class CmdInvalidateUserCache : public TypedCommand<CmdInvalidateUserCache> {
public:
    using Request = InvalidateUserCacheCommand;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto authzManager = AuthorizationManager::get(opCtx->getServiceContext());
            authzManager->invalidateUserCache(opCtx);
        }

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auth::checkAuthForTypedCommand(opCtx->getClient(), request());
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return true;
    }
} cmdInvalidateUserCache;

/**
 * This command is used only by mongorestore to handle restoring users/roles.  We do this so
 * that mongorestore doesn't do direct inserts into the admin.system.users and
 * admin.system.roles, which would bypass the authzUpdateLock and allow multiple concurrent
 * modifications to users/roles.  What mongorestore now does instead is it inserts all user/role
 * definitions it wants to restore into temporary collections, then this command moves those
 * user/role definitions into their proper place in admin.system.users and admin.system.roles.
 * It either adds the users/roles to the existing ones or replaces the existing ones, depending
 * on whether the "drop" argument is true or false.
 */
class CmdMergeAuthzCollections : public BasicCommand {
public:
    CmdMergeAuthzCollections() : BasicCommand("_mergeAuthzCollections") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }

    std::string help() const override {
        return "Internal command used by mongorestore for updating user/role data";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForMergeAuthzCollectionsCommand(client, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        uassertStatusOK(Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            &result));
        return true;
    }

} cmdMergeAuthzCollections;

}  // namespace
}  // namespace mongo
