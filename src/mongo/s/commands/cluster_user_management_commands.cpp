/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/user_management_commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {

using std::string;
using std::stringstream;
using std::vector;

namespace {

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                // Note: Even though we're setting UNSET here,
                                                // kMajority implies JOURNAL if journaling is
                                                // supported by this mongod.
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(30));

class CmdCreateUser : public BasicCommand {
public:
    CmdCreateUser() : BasicCommand("createUser") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Adds a user to the system";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForCreateUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        return Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);
    }

    void redactForLogging(mutablebson::Document* cmdObj) const override {
        auth::redactPasswordData(cmdObj->root());
    }

} cmdCreateUser;

class CmdUpdateUser : public BasicCommand {
public:
    CmdUpdateUser() : BasicCommand("updateUser") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Used to update a user, for example to change its password";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForUpdateUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auth::CreateOrUpdateUserArgs args;
        Status status = auth::parseCreateOrUpdateUserCommands(cmdObj, getName(), dbname, &args);
        uassertStatusOK(status);

        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserByName(args.userName);

        return ok;
    }

    void redactForLogging(mutablebson::Document* cmdObj) const override {
        auth::redactPasswordData(cmdObj->root());
    }

} cmdUpdateUser;

class CmdDropUser : public BasicCommand {
public:
    CmdDropUser() : BasicCommand("dropUser") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Drops a single user.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForDropUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        UserName userName;
        Status status = auth::parseAndValidateDropUserCommand(cmdObj, dbname, &userName);
        uassertStatusOK(status);

        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserByName(userName);

        return ok;
    }

} cmdDropUser;

class CmdDropAllUsersFromDatabase : public BasicCommand {
public:
    CmdDropAllUsersFromDatabase() : BasicCommand("dropAllUsersFromDatabase") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Drops all users for a single database.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForDropAllUsersFromDatabaseCommand(client, dbname);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUsersFromDB(dbname);

        return ok;
    }

} cmdDropAllUsersFromDatabase;

class CmdGrantRolesToUser : public BasicCommand {
public:
    CmdGrantRolesToUser() : BasicCommand("grantRolesToUser") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Grants roles to a user.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForGrantRolesToUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        string userNameString;
        vector<RoleName> roles;
        Status status = auth::parseRolePossessionManipulationCommands(
            cmdObj, getName(), dbname, &userNameString, &roles);
        uassertStatusOK(status);

        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserByName(UserName(userNameString, dbname));

        return ok;
    }

} cmdGrantRolesToUser;

class CmdRevokeRolesFromUser : public BasicCommand {
public:
    CmdRevokeRolesFromUser() : BasicCommand("revokeRolesFromUser") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Revokes roles from a user.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForRevokeRolesFromUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        string userNameString;
        vector<RoleName> unusedRoles;
        Status status = auth::parseRolePossessionManipulationCommands(
            cmdObj, getName(), dbname, &userNameString, &unusedRoles);
        uassertStatusOK(status);

        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserByName(UserName(userNameString, dbname));

        return ok;
    }

} cmdRevokeRolesFromUser;

class CmdUsersInfo : public BasicCommand {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    CmdUsersInfo() : BasicCommand("usersInfo") {}

    std::string help() const override {
        return "Returns information about users.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForUsersInfoCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        return Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
            opCtx, dbname, CommandHelpers::filterCommandRequestForPassthrough(cmdObj), &result);
    }

} cmdUsersInfo;

class CmdCreateRole : public BasicCommand {
public:
    CmdCreateRole() : BasicCommand("createRole") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Adds a role to the system";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForCreateRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        return Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);
    }

} cmdCreateRole;

class CmdUpdateRole : public BasicCommand {
public:
    CmdUpdateRole() : BasicCommand("updateRole") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Used to update a role";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForUpdateRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdUpdateRole;

class CmdGrantPrivilegesToRole : public BasicCommand {
public:
    CmdGrantPrivilegesToRole() : BasicCommand("grantPrivilegesToRole") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Grants privileges to a role";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForGrantPrivilegesToRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdGrantPrivilegesToRole;

class CmdRevokePrivilegesFromRole : public BasicCommand {
public:
    CmdRevokePrivilegesFromRole() : BasicCommand("revokePrivilegesFromRole") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Revokes privileges from a role";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForRevokePrivilegesFromRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdRevokePrivilegesFromRole;

class CmdGrantRolesToRole : public BasicCommand {
public:
    CmdGrantRolesToRole() : BasicCommand("grantRolesToRole") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Grants roles to another role.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForGrantRolesToRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdGrantRolesToRole;

class CmdRevokeRolesFromRole : public BasicCommand {
public:
    CmdRevokeRolesFromRole() : BasicCommand("revokeRolesFromRole") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Revokes roles from another role.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForRevokeRolesFromRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdRevokeRolesFromRole;

class CmdDropRole : public BasicCommand {
public:
    CmdDropRole() : BasicCommand("dropRole") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Drops a single role.  Before deleting the role completely it must remove it "
               "from any users or roles that reference it.  If any errors occur in the middle "
               "of that process it's possible to be left in a state where the role has been "
               "removed from some user/roles but otherwise still exists.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForDropRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdDropRole;

class CmdDropAllRolesFromDatabase : public BasicCommand {
public:
    CmdDropAllRolesFromDatabase() : BasicCommand("dropAllRolesFromDatabase") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Drops all roles from the given database.  Before deleting the roles completely "
               "it must remove them from any users or other roles that reference them.  If any "
               "errors occur in the middle of that process it's possible to be left in a state "
               "where the roles have been removed from some user/roles but otherwise still "
               "exist.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForDropAllRolesFromDatabaseCommand(client, dbname);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdDropAllRolesFromDatabase;

class CmdRolesInfo : public BasicCommand {
public:
    CmdRolesInfo() : BasicCommand("rolesInfo") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Returns information about roles.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForRolesInfoCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        return Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
            opCtx, dbname, CommandHelpers::filterCommandRequestForPassthrough(cmdObj), &result);
    }

} cmdRolesInfo;

class CmdInvalidateUserCache : public BasicCommand {
public:
    CmdInvalidateUserCache() : BasicCommand("invalidateUserCache") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual bool adminOnly() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Invalidates the in-memory cache of user information";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForInvalidateUserCacheCommand(client);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();
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
        return Grid::get(opCtx)->catalogClient()->runUserManagementWriteCommand(
            opCtx,
            getName(),
            dbname,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            &result);
    }

} cmdMergeAuthzCollections;

}  // namespace
}  // namespace mongo
