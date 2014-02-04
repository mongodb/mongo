/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/auth/authorization_session.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
    const std::string ADMIN_DBNAME = "admin";
}  // namespace

    AuthorizationSession::AuthorizationSession(AuthzSessionExternalState* externalState) 
        : _impersonationFlag(false) {
        _externalState.reset(externalState);
    }

    AuthorizationSession::~AuthorizationSession() {
        for (UserSet::iterator it = _authenticatedUsers.begin();
                it != _authenticatedUsers.end(); ++it) {
            getAuthorizationManager().releaseUser(*it);
        }
    }

    AuthorizationManager& AuthorizationSession::getAuthorizationManager() {
        return _externalState->getAuthorizationManager();
    }

    void AuthorizationSession::startRequest() {
        _externalState->startRequest();
        _refreshUserInfoAsNeeded();
    }

    Status AuthorizationSession::addAndAuthorizeUser(const UserName& userName) {
        User* user;
        Status status = getAuthorizationManager().acquireUser(userName, &user);
        if (!status.isOK()) {
            return status;
        }

        // If there are any users in the impersonate list, clear them.
        clearImpersonatedUserNames();

        // Calling add() on the UserSet may return a user that was replaced because it was from the
        // same database.
        User* replacedUser = _authenticatedUsers.add(user);
        if (replacedUser) {
            getAuthorizationManager().releaseUser(replacedUser);
        }

        return Status::OK();
    }

    User* AuthorizationSession::lookupUser(const UserName& name) {
        return _authenticatedUsers.lookup(name);
    }

    void AuthorizationSession::logoutDatabase(const std::string& dbname) {
        clearImpersonatedUserNames();
        User* removedUser = _authenticatedUsers.removeByDBName(dbname);
        if (removedUser) {
            getAuthorizationManager().releaseUser(removedUser);
        }
    }

    UserNameIterator AuthorizationSession::getAuthenticatedUserNames() {
        return _authenticatedUsers.getNames();
    }

    std::string AuthorizationSession::getAuthenticatedUserNamesToken() {
        std::string ret;
        for (UserNameIterator nameIter = getAuthenticatedUserNames();
                nameIter.more();
                nameIter.next()) {
            ret += '\0'; // Using a NUL byte which isn't valid in usernames to separate them.
            ret += nameIter->getFullName();
        }

        return ret;
    }

    void AuthorizationSession::grantInternalAuthorization() {
        _authenticatedUsers.add(internalSecurity.user);
    }

    Status AuthorizationSession::checkAuthForQuery(const NamespaceString& ns,
                                                   const BSONObj& query) {
        if (MONGO_unlikely(ns.isCommand())) {
            return Status(ErrorCodes::InternalError, mongoutils::str::stream() <<
                          "Checking query auth on command namespace " << ns.ns());
        }
        if (!isAuthorizedForActionsOnNamespace(ns, ActionType::find)) {
            return Status(ErrorCodes::Unauthorized,
                          mongoutils::str::stream() << "not authorized for query on " << ns.ns());
        }
        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForGetMore(const NamespaceString& ns,
                                                     long long cursorID) {
        if (!isAuthorizedForActionsOnNamespace(ns, ActionType::find)) {
            return Status(ErrorCodes::Unauthorized,
                          mongoutils::str::stream() << "not authorized for getmore on " << ns.ns());
        }
        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForInsert(const NamespaceString& ns,
                                                    const BSONObj& document) {
        if (ns.coll() == StringData("system.indexes", StringData::LiteralTag())) {
            BSONElement nsElement = document["ns"];
            if (nsElement.type() != String) {
                return Status(ErrorCodes::Unauthorized, "Cannot authorize inserting into "
                              "system.indexes documents without a string-typed \"ns\" field.");
            }
            NamespaceString indexNS(nsElement.str());
            if (!isAuthorizedForActionsOnNamespace(indexNS, ActionType::createIndex)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized to create index on " <<
                              indexNS.ns());
            }
        } else {
            if (!isAuthorizedForActionsOnNamespace(ns, ActionType::insert)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for insert on " <<
                              ns.ns());
            }
        }

        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForUpdate(const NamespaceString& ns,
                                                    const BSONObj& query,
                                                    const BSONObj& update,
                                                    bool upsert) {
        if (!upsert) {
            if (!isAuthorizedForActionsOnNamespace(ns, ActionType::update)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for update on " <<
                              ns.ns());
            }
        }
        else {
            ActionSet required;
            required.addAction(ActionType::update);
            required.addAction(ActionType::insert);
            if (!isAuthorizedForActionsOnNamespace(ns, required)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for upsert on " <<
                              ns.ns());
            }
        }
        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForDelete(const NamespaceString& ns,
                                                    const BSONObj& query) {
        if (!isAuthorizedForActionsOnNamespace(ns, ActionType::remove)) {
            return Status(ErrorCodes::Unauthorized,
                          mongoutils::str::stream() << "not authorized to remove from " << ns.ns());
        }
        return Status::OK();
    }

    Status AuthorizationSession::checkAuthorizedToGrantPrivilege(const Privilege& privilege) {
        const ResourcePattern& resource = privilege.getResourcePattern();
        if (resource.isDatabasePattern() || resource.isExactNamespacePattern()) {
            if (!isAuthorizedForActionsOnResource(
                    ResourcePattern::forDatabaseName(resource.databaseToMatch()),
                    ActionType::grantRole)) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to grant privileges on the "
                                      << resource.databaseToMatch() << "database");
            }
        } else if (!isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName("admin"), ActionType::grantRole)) {
            return Status(ErrorCodes::Unauthorized,
                          "To grant privileges affecting multiple databases or the cluster,"
                          " must be authorized to grant roles from the admin database");
        }
        return Status::OK();
    }


    Status AuthorizationSession::checkAuthorizedToRevokePrivilege(const Privilege& privilege) {
        const ResourcePattern& resource = privilege.getResourcePattern();
        if (resource.isDatabasePattern() || resource.isExactNamespacePattern()) {
            if (!isAuthorizedForActionsOnResource(
                    ResourcePattern::forDatabaseName(resource.databaseToMatch()),
                    ActionType::revokeRole)) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to revoke privileges on the "
                                      << resource.databaseToMatch() << "database");
            }
        } else if (!isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName("admin"), ActionType::revokeRole)) {
            return Status(ErrorCodes::Unauthorized,
                          "To revoke privileges affecting multiple databases or the cluster,"
                          " must be authorized to revoke roles from the admin database");
        }
        return Status::OK();
    }

    bool AuthorizationSession::isAuthorizedToGrantRole(const RoleName& role) {
        return isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(role.getDB()),
                ActionType::grantRole);
    }

    bool AuthorizationSession::isAuthorizedToRevokeRole(const RoleName& role) {
        return isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(role.getDB()),
                ActionType::revokeRole);
    }

    bool AuthorizationSession::isAuthorizedForPrivilege(const Privilege& privilege) {
        if (_externalState->shouldIgnoreAuthChecks())
            return true;

        return _isAuthorizedForPrivilege(privilege);
    }

    bool AuthorizationSession::isAuthorizedForPrivileges(const vector<Privilege>& privileges) {
        if (_externalState->shouldIgnoreAuthChecks())
            return true;

        for (size_t i = 0; i < privileges.size(); ++i) {
            if (!_isAuthorizedForPrivilege(privileges[i]))
                return false;
        }

        return true;
    }

    bool AuthorizationSession::isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                                ActionType action) {
        return isAuthorizedForPrivilege(Privilege(resource, action));
    }

    bool AuthorizationSession::isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                                const ActionSet& actions) {
        return isAuthorizedForPrivilege(Privilege(resource, actions));
    }

    bool AuthorizationSession::isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                                 ActionType action) {
        return isAuthorizedForPrivilege(Privilege(ResourcePattern::forExactNamespace(ns), action));
    }

    bool AuthorizationSession::isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                                const ActionSet& actions) {
        return isAuthorizedForPrivilege(Privilege(ResourcePattern::forExactNamespace(ns), actions));
    }

    static const int resourceSearchListCapacity = 5;
    /**
     * Builds from "target" an exhaustive list of all ResourcePatterns that match "target".
     *
     * Stores the resulting list into resourceSearchList, and returns the length.
     *
     * The seach lists are as follows, depending on the type of "target":
     *
     * target is ResourcePattern::forAnyResource():
     *   searchList = { ResourcePattern::forAnyResource(), ResourcePattern::forAnyResource() }
     * target is the ResourcePattern::forClusterResource():
     *   searchList = { ResourcePattern::forAnyResource(), ResourcePattern::forClusterResource() }
     * target is a database, db:
     *   searchList = { ResourcePattern::forAnyResource(),
     *                  ResourcePattern::forAnyNormalResource(),
     *                  db }
     * target is a non-system collection, db.coll:
     *   searchList = { ResourcePattern::forAnyResource(),
     *                  ResourcePattern::forAnyNormalResource(),
     *                  db,
     *                  coll,
     *                  db.coll }
     * target is a system collection, db.system.coll:
     *   searchList = { ResourcePattern::forAnyResource(),
     *                  system.coll,
     *                  db.system.coll }
     */
    static int buildResourceSearchList(
            const ResourcePattern& target,
            ResourcePattern resourceSearchList[resourceSearchListCapacity]) {

        int size = 0;
        resourceSearchList[size++] = ResourcePattern::forAnyResource();
        if (target.isExactNamespacePattern()) {
            if (!target.ns().isSystem()) {
                resourceSearchList[size++] = ResourcePattern::forAnyNormalResource();
                resourceSearchList[size++] = ResourcePattern::forDatabaseName(target.ns().db());
            }
            resourceSearchList[size++] = ResourcePattern::forCollectionName(target.ns().coll());
        }
        else if (target.isDatabasePattern()) {
                resourceSearchList[size++] = ResourcePattern::forAnyNormalResource();
        }
        resourceSearchList[size++] = target;
        dassert(size <= resourceSearchListCapacity);
        return size;
    }

    bool AuthorizationSession::isAuthorizedToChangeOwnPasswordAsUser(const UserName& userName) {
        User* user = lookupUser(userName);
        if (!user) {
            return false;
        }
        ResourcePattern resourceSearchList[resourceSearchListCapacity];
        const int resourceSearchListLength =
                buildResourceSearchList(ResourcePattern::forDatabaseName(userName.getDB()),
                                        resourceSearchList);

        ActionSet actions;
        for (int i = 0; i < resourceSearchListLength; ++i) {
            actions.addAllActionsFromSet(user->getActionsForResource(resourceSearchList[i]));
        }
        return actions.contains(ActionType::changeOwnPassword);
    }

    bool AuthorizationSession::isAuthorizedToChangeOwnCustomDataAsUser(const UserName& userName) {
        User* user = lookupUser(userName);
        if (!user) {
            return false;
        }
        ResourcePattern resourceSearchList[resourceSearchListCapacity];
        const int resourceSearchListLength =
                buildResourceSearchList(ResourcePattern::forDatabaseName(userName.getDB()),
                                        resourceSearchList);

        ActionSet actions;
        for (int i = 0; i < resourceSearchListLength; ++i) {
            actions.addAllActionsFromSet(user->getActionsForResource(resourceSearchList[i]));
        }
        return actions.contains(ActionType::changeOwnCustomData);
    }

    bool AuthorizationSession::isAuthenticatedAsUserWithRole(const RoleName& roleName) {
        for (UserSet::iterator it = _authenticatedUsers.begin();
                it != _authenticatedUsers.end(); ++it) {
            if ((*it)->hasRole(roleName)) {
                return true;
            }
        }
        return false;
    }

    void AuthorizationSession::_refreshUserInfoAsNeeded() {
        AuthorizationManager& authMan = getAuthorizationManager();
        UserSet::iterator it = _authenticatedUsers.begin();
        while (it != _authenticatedUsers.end()) {
            User* user = *it;

            if (!user->isValid()) {
                // Make a good faith effort to acquire an up-to-date user object, since the one
                // we've cached is marked "out-of-date."
                UserName name = user->getName();
                User* updatedUser;

                Status status = authMan.acquireUser(name, &updatedUser);
                switch (status.code()) {
                case ErrorCodes::OK: {
                    // Success! Replace the old User object with the updated one.
                    fassert(17067, _authenticatedUsers.replaceAt(it, updatedUser) == user);
                    authMan.releaseUser(user);
                    LOG(1) << "Updated session cache of user information for " << name;
                    break;
                }
                case ErrorCodes::UserNotFound: {
                    // User does not exist anymore; remove it from _authenticatedUsers.
                    fassert(17068, _authenticatedUsers.removeAt(it) == user);
                    authMan.releaseUser(user);
                    log() << "Removed deleted user " << name <<
                        " from session cache of user information.";
                    continue;  // No need to advance "it" in this case.
                }
                default:
                    // Unrecognized error; assume that it's transient, and continue working with the
                    // out-of-date privilege data.
                    warning() << "Could not fetch updated user privilege information for " <<
                        name << "; continuing to use old information.  Reason is " << status;
                    break;
                }
            }
            ++it;
        }
    }

    bool AuthorizationSession::_isAuthorizedForPrivilege(const Privilege& privilege) {
        const ResourcePattern& target(privilege.getResourcePattern());

        ResourcePattern resourceSearchList[resourceSearchListCapacity];
        const int resourceSearchListLength = buildResourceSearchList(target, resourceSearchList);

        ActionSet unmetRequirements = privilege.getActions();

        for (UserSet::iterator it = _authenticatedUsers.begin();
                it != _authenticatedUsers.end(); ++it) {
            User* user = *it;

            if (user->getSchemaVersion() == AuthorizationManager::schemaVersion24 &&
                (target.isDatabasePattern() || target.isExactNamespacePattern()) &&
                !user->hasProbedV1(target.databaseToMatch())) {

                UserName name = user->getName();
                User* updatedUser;
                Status status = getAuthorizationManager().acquireV1UserProbedForDb(
                        name,
                        target.databaseToMatch(),
                        &updatedUser);
                if (status.isOK()) {
                    if (user != updatedUser) {
                        LOG(1) << "Updated session cache for V1 user " << name;
                        fassert(17226, _authenticatedUsers.replaceAt(it, updatedUser) == user);
                    }
                    getAuthorizationManager().releaseUser(user);
                    user = updatedUser;
                }
                else if (status != ErrorCodes::UserNotFound) {
                    warning() << "Could not fetch updated user privilege information for V1-style "
                        "user " << name << "; continuing to use old information.  Reason is "
                              << status;
                }
            }

            for (int i = 0; i < resourceSearchListLength; ++i) {
                ActionSet userActions = user->getActionsForResource(resourceSearchList[i]);
                unmetRequirements.removeAllActionsFromSet(userActions);

                if (unmetRequirements.empty())
                    return true;
            }
        }

        return false;
    }

    void AuthorizationSession::setImpersonatedUserNames(const std::vector<UserName>& names) {
        _impersonatedUserNames = names;
        _impersonationFlag = true;
    }

    // Clear the vector of impersonated UserNames.
    void AuthorizationSession::clearImpersonatedUserNames() {
        _impersonatedUserNames.clear();
        _impersonationFlag = false;
    }

    UserNameIterator AuthorizationSession::getImpersonatedUserNames() const {
         return makeUserNameIteratorForContainer(_impersonatedUserNames);
    }

    bool AuthorizationSession::isImpersonating() const {
        return _impersonationFlag;
    }

} // namespace mongo
