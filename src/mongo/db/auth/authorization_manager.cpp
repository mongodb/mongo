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
*/

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_manager.h"

#include <boost/thread/mutex.hpp>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_document_parser.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/user_name_hash.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthInfo internalSecurity;

    MONGO_INITIALIZER(SetupInternalSecurityUser)(InitializerContext* context) {
        User* user = new User(UserName("__system", "local"));

        user->incrementRefCount(); // Pin this user so the ref count never drops below 1.
        ActionSet allActions;
        allActions.addAllActions();
        user->addPrivilege(Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME, allActions));

        internalSecurity.user = user;

        return Status::OK();
    }

    const std::string AuthorizationManager::SERVER_RESOURCE_NAME = "$SERVER";
    const std::string AuthorizationManager::CLUSTER_RESOURCE_NAME = "$CLUSTER";
    const std::string AuthorizationManager::WILDCARD_RESOURCE_NAME = "*";
    const std::string AuthorizationManager::USER_NAME_FIELD_NAME = "user";
    const std::string AuthorizationManager::USER_SOURCE_FIELD_NAME = "userSource";
    const std::string AuthorizationManager::PASSWORD_FIELD_NAME = "pwd";

    bool AuthorizationManager::_doesSupportOldStylePrivileges = true;
    bool AuthorizationManager::_authEnabled = false;

    AuthorizationManager::AuthorizationManager(AuthzManagerExternalState* externalState) :
            _version(1), _externalState(externalState) {}

    AuthorizationManager::~AuthorizationManager() {
        for (unordered_map<UserName, User*>::iterator it = _userCache.begin();
                it != _userCache.end(); ++it) {
            if (it->second != internalSecurity.user) {
                // The internal user should never be deleted.
                delete it->second ;
            }
        }
    }

    AuthzManagerExternalState* AuthorizationManager::getExternalState() const {
        return _externalState.get();
    }

    void AuthorizationManager::setSupportOldStylePrivilegeDocuments(bool enabled) {
        _doesSupportOldStylePrivileges = enabled;
    }

    bool AuthorizationManager::getSupportOldStylePrivilegeDocuments() {
        return _doesSupportOldStylePrivileges;
    }

    void AuthorizationManager::setAuthEnabled(bool enabled) {
        _authEnabled = enabled;
    }

    bool AuthorizationManager::isAuthEnabled() {
        return _authEnabled;
    }

    Status AuthorizationManager::getPrivilegeDocument(const std::string& dbname,
                                                      const UserName& userName,
                                                      BSONObj* result) const {
        return _externalState->getPrivilegeDocument(dbname, userName, result);
    }

    bool AuthorizationManager::hasAnyPrivilegeDocuments() const {
        return _externalState->hasAnyPrivilegeDocuments();
    }

    Status AuthorizationManager::insertPrivilegeDocument(const std::string& dbname,
                                                         const BSONObj& userObj) const {
        return _externalState->insertPrivilegeDocument(dbname, userObj);
    }

    Status AuthorizationManager::updatePrivilegeDocument(const UserName& user,
                                                         const BSONObj& updateObj) const {
        return _externalState->updatePrivilegeDocument(user, updateObj);
    }

    Status AuthorizationManager::removePrivilegeDocuments(const std::string& dbname,
                                                          const BSONObj& query) const {
        return _externalState->removePrivilegeDocuments(dbname, query);
    }

    ActionSet AuthorizationManager::getAllUserActions() const {
        V1PrivilegeDocumentParser parser;
        return parser.getAllUserActions();
    }

    Status AuthorizationManager::acquireUser(const UserName& userName, User** acquiredUser) {
        boost::lock_guard<boost::mutex> lk(_lock);
        unordered_map<UserName, User*>::iterator it = _userCache.find(userName);
        if (it != _userCache.end()) {
            fassert(16914, it->second);
            fassert(17003, it->second->isValid());
            fassert(17008, it->second->getRefCount() > 0);
            it->second->incrementRefCount();
            *acquiredUser = it->second;
            return Status::OK();
        }

        // Put the new user into an auto_ptr temporarily in case there's an error while
        // initializing the user.
        auto_ptr<User> userHolder(new User(userName));
        User* user = userHolder.get();

        BSONObj userObj;
        if (_version == 1) {
            Status status = _externalState->getPrivilegeDocument(userName.getDB().toString(),
                                                                 userName,
                                                                 &userObj);
            if (!status.isOK()) {
                return status;
            }
        } else {
            return Status(ErrorCodes::UnsupportedFormat,
                          mongoutils::str::stream() <<
                                  "Unrecognized authorization format version: " << _version);
        }


        V1PrivilegeDocumentParser parser;
        Status status = parser.initializeUserFromPrivilegeDocument(user, userObj);
        if (!status.isOK()) {
            return status;
        }

        user->incrementRefCount();
        _userCache.insert(make_pair(userName, userHolder.release()));
        *acquiredUser = user;
        return Status::OK();
    }

    void AuthorizationManager::releaseUser(User* user) {
        if (user == internalSecurity.user) {
            return;
        }

        boost::lock_guard<boost::mutex> lk(_lock);
        user->decrementRefCount();
        if (user->getRefCount() == 0) {
            // If it's been invalidated then it's not in the _userCache anymore.
            if (user->isValid()) {
                MONGO_COMPILER_VARIABLE_UNUSED bool erased = _userCache.erase(user->getName());
                dassert(erased);
            }
            delete user;
        }
    }

    void AuthorizationManager::invalidateUser(User* user) {
        boost::lock_guard<boost::mutex> lk(_lock);
        if (!user->isValid()) {
            return;
        }

        unordered_map<UserName, User*>::iterator it = _userCache.find(user->getName());
        massert(17052,
                mongoutils::str::stream() <<
                        "Invalidating cache for user " << user->getName().toString() <<
                        " failed as it is not present in the user cache",
                it != _userCache.end() && it->second == user);
        _userCache.erase(it);
        user->invalidate();
    }

    void AuthorizationManager::addInternalUser(User* user) {
        boost::lock_guard<boost::mutex> lk(_lock);
        _userCache.insert(make_pair(user->getName(), user));
    }

    void AuthorizationManager::_invalidateUserCache_inlock() {
        for (unordered_map<UserName, User*>::iterator it = _userCache.begin();
                it != _userCache.end(); ++it) {
            if (it->second->getName() == internalSecurity.user->getName()) {
                // Don't invalidate the internal user
                continue;
            }
            it->second->invalidate();
            // Need to decrement ref count and manually clean up User object to prevent memory leaks
            // since we're pinning all User objects by incrementing their ref count when we
            // initially populate the cache.
            // TODO(spencer): remove this once we're not pinning User objects.
            it->second->decrementRefCount();
            if (it->second->getRefCount() == 0)
                delete it->second;
        }
        _userCache.clear();
        // Make sure the internal user stays in the cache.
        _userCache.insert(make_pair(internalSecurity.user->getName(), internalSecurity.user));
    }

    Status AuthorizationManager::initializeAllV1UserData() {
        boost::lock_guard<boost::mutex> lk(_lock);
        _invalidateUserCache_inlock();
        V1PrivilegeDocumentParser parser;

        try {
            std::vector<std::string> dbNames;
            Status status = _externalState->getAllDatabaseNames(&dbNames);
            if (!status.isOK()) {
                return status;
            }

            for (std::vector<std::string>::iterator dbIt = dbNames.begin();
                    dbIt != dbNames.end(); ++dbIt) {
                std::string dbname = *dbIt;
                std::vector<BSONObj> privDocs;
                Status status = _externalState->getAllV1PrivilegeDocsForDB(dbname, &privDocs);
                if (!status.isOK()) {
                    return status;
                }

                for (std::vector<BSONObj>::iterator docIt = privDocs.begin();
                        docIt != privDocs.end(); ++docIt) {
                    const BSONObj& privDoc = *docIt;

                    std::string source;
                    if (privDoc.hasField("userSource")) {
                        source = privDoc["userSource"].String();
                    } else {
                        source = dbname;
                    }
                    UserName userName(privDoc["user"].String(), source);
                    if (userName == internalSecurity.user->getName()) {
                        // Don't let clients override the internal user by creating a user with the
                        // same name.
                        continue;
                    }

                    User* user = mapFindWithDefault(_userCache, userName, static_cast<User*>(NULL));
                    if (!user) {
                        user = new User(userName);
                        // Make sure the user always has a refCount of at least 1 so it's
                        // effectively "pinned" and will never be removed from the _userCache
                        // unless the whole cache is invalidated.
                        user->incrementRefCount();
                        _userCache.insert(make_pair(userName, user));
                    }

                    if (source == dbname || source == "$external") {
                        status = parser.initializeUserCredentialsFromPrivilegeDocument(user,
                                                                                       privDoc);
                        if (!status.isOK()) {
                            return status;
                        }
                    }
                    status = parser.initializeUserRolesFromPrivilegeDocument(user, privDoc, dbname);
                    if (!status.isOK()) {
                        return status;
                    }
                    parser.initializeUserPrivilegesFromRoles(user);
                }
            }
        } catch (const DBException& e) {
            return e.toStatus();
        } catch (const std::exception& e) {
            return Status(ErrorCodes::InternalError, e.what());
        }

        return Status::OK();
    }
} // namespace mongo
