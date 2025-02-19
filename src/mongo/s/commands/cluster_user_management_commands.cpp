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


#include <fmt/format.h>
#include <iosfwd>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/user_management_commands_common.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {

using std::string;
using std::stringstream;
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
    return Reply::parse(IDLParserContext(Request::kCommandName), obj);
} catch (const AssertionException& ex) {
    uasserted(ex.code(),
              "Received invalid response from {} command: {}, error: {}"_format(
                  Request::kCommandName, tojson(obj), ex.reason()));
}

struct UserCacheInvalidatorNOOP {
    static constexpr bool kRequireUserName = false;
    static void invalidate(OperationContext*, const DatabaseName&) {}
};
struct UserCacheInvalidatorUser {
    static constexpr bool kRequireUserName = true;
    static void invalidate(OperationContext* opCtx, const UserName& userName) {
        AuthorizationManager::get(opCtx->getService())->invalidateUserByName(userName);
    }
};
struct UserCacheInvalidatorDB {
    static constexpr bool kRequireUserName = false;
    static void invalidate(OperationContext* opCtx, const DatabaseName& dbname) {
        AuthorizationManager::get(opCtx->getService())->invalidateUsersFromDB(dbname);
    }
};
struct UserCacheInvalidatorAll {
    static constexpr bool kRequireUserName = false;
    static void invalidate(OperationContext* opCtx, const DatabaseName&) {
        AuthorizationManager::get(opCtx->getService())->invalidateUserCache();
    }
};

template <typename T>
using HasGetCmdParamOp = std::remove_cv_t<decltype(std::declval<T>().getCommandParameter())>;
template <typename T>
constexpr bool hasGetCmdParamStringData =
    stdx::is_detected_exact_v<StringData, HasGetCmdParamOp, T>;

/**
 * Most user management commands follow a very predictable pattern:
 * 1. Proxy command to config servers.
 * 2. Invalidate whatever we were just working on.
 * 3. Panic if anything went wrong.
 */
template <typename RequestT, typename InvalidatorT>
class CmdUMCPassthrough : public TypedCommand<CmdUMCPassthrough<RequestT, InvalidatorT>> {
public:
    using Request = RequestT;
    using Reply = typename RequestT::Reply;
    using TC = TypedCommand<CmdUMCPassthrough<RequestT, InvalidatorT>>;

    class Invocation final : public TC::InvocationBase {
    public:
        using TC::InvocationBase::InvocationBase;
        using TC::InvocationBase::request;

        Reply typedRun(OperationContext* opCtx) {
            auto& cmd = request();
            setReadWriteConcern(opCtx, cmd, this);

            BSONObjBuilder builder;
            auto status = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
                opCtx,
                Request::kCommandName,
                cmd.getDbName(),
                CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON()),
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
            auth::checkAuthForTypedCommand(opCtx, request());
        }

        NamespaceString ns() const override {
            const auto& cmd = request();
            if constexpr (hasGetCmdParamStringData<RequestT>) {
                return NamespaceStringUtil::deserialize(cmd.getDbName(), cmd.getCommandParameter());
            } else {
                return NamespaceString(cmd.getDbName());
            }
        }
    };

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return TC::AllowedOnSecondary::kNever;
    }
};

class CmdCreateUser : public CmdUMCPassthrough<CreateUserCommand, UserCacheInvalidatorNOOP> {
public:
    static constexpr StringData kPwdField = "pwd"_sd;
    std::set<StringData> sensitiveFieldNames() const final {
        return {kPwdField};
    }
};
MONGO_REGISTER_COMMAND(CmdCreateUser).forRouter();

class CmdUpdateUser : public CmdUMCPassthrough<UpdateUserCommand, UserCacheInvalidatorUser> {
public:
    static constexpr StringData kPwdField = "pwd"_sd;
    std::set<StringData> sensitiveFieldNames() const final {
        return {kPwdField};
    }
};
MONGO_REGISTER_COMMAND(CmdUpdateUser).forRouter();

MONGO_REGISTER_COMMAND(CmdUMCPassthrough<DropUserCommand, UserCacheInvalidatorUser>).forRouter();
MONGO_REGISTER_COMMAND(CmdUMCPassthrough<DropAllUsersFromDatabaseCommand, UserCacheInvalidatorDB>)
    .forRouter();
MONGO_REGISTER_COMMAND(CmdUMCPassthrough<GrantRolesToUserCommand, UserCacheInvalidatorUser>)
    .forRouter();
MONGO_REGISTER_COMMAND(CmdUMCPassthrough<RevokeRolesFromUserCommand, UserCacheInvalidatorUser>)
    .forRouter();
MONGO_REGISTER_COMMAND(CmdUMCPassthrough<CreateRoleCommand, UserCacheInvalidatorNOOP>).forRouter();
MONGO_REGISTER_COMMAND(CmdUMCPassthrough<UpdateRoleCommand, UserCacheInvalidatorAll>).forRouter();
MONGO_REGISTER_COMMAND(CmdUMCPassthrough<GrantPrivilegesToRoleCommand, UserCacheInvalidatorAll>)
    .forRouter();
MONGO_REGISTER_COMMAND(CmdUMCPassthrough<RevokePrivilegesFromRoleCommand, UserCacheInvalidatorAll>)
    .forRouter();
MONGO_REGISTER_COMMAND(CmdUMCPassthrough<GrantRolesToRoleCommand, UserCacheInvalidatorAll>)
    .forRouter();
MONGO_REGISTER_COMMAND(CmdUMCPassthrough<RevokeRolesFromRoleCommand, UserCacheInvalidatorAll>)
    .forRouter();
MONGO_REGISTER_COMMAND(CmdUMCPassthrough<DropRoleCommand, UserCacheInvalidatorAll>).forRouter();
MONGO_REGISTER_COMMAND(CmdUMCPassthrough<DropAllRolesFromDatabaseCommand, UserCacheInvalidatorAll>)
    .forRouter();

/**
 * usersInfo and rolesInfo are simpler read-only passthrough commands.
 */
template <typename RequestT>
class CmdUMCInfo : public TypedCommand<CmdUMCInfo<RequestT>> {
public:
    using Request = RequestT;
    using Reply = typename RequestT::Reply;
    using TC = TypedCommand<CmdUMCInfo<RequestT>>;

    class Invocation final : public TC::InvocationBase {
    public:
        using TC::InvocationBase::InvocationBase;
        using TC::InvocationBase::request;

        Reply typedRun(OperationContext* opCtx) {
            auto& cmd = request();
            setReadWriteConcern(opCtx, cmd, this);

            BSONObjBuilder builder;
            const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
                opCtx,
                cmd.getDbName(),
                CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON()),
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
            auth::checkAuthForTypedCommand(opCtx, request());
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return TC::AllowedOnSecondary::kOptIn;
    }
};

MONGO_REGISTER_COMMAND(CmdUMCInfo<UsersInfoCommand>).forRouter();
MONGO_REGISTER_COMMAND(CmdUMCInfo<RolesInfoCommand>).forRouter();

class CmdInvalidateUserCache : public TypedCommand<CmdInvalidateUserCache> {
public:
    using Request = InvalidateUserCacheCommand;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            AuthorizationManager::get(opCtx->getService())->invalidateUserCache();
        }

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auth::checkAuthForTypedCommand(opCtx, request());
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdInvalidateUserCache).forRouter();

class CmdMergeAuthzCollections
    : public CmdUMCPassthrough<MergeAuthzCollectionsCommand, UserCacheInvalidatorNOOP> {
public:
    bool adminOnly() const final {
        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdMergeAuthzCollections).forRouter();

}  // namespace
}  // namespace mongo
