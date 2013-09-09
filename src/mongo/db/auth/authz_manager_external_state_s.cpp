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

#include "mongo/db/auth/authz_manager_external_state_s.h"

#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/client/dbclientinterface.h"
#include "mongo/client/distlock.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/config.h"
#include "mongo/s/type_database.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
    static Status userNotFoundStatus(ErrorCodes::UserNotFound, "User not found");
}

    AuthzManagerExternalStateMongos::AuthzManagerExternalStateMongos() {}

    AuthzManagerExternalStateMongos::~AuthzManagerExternalStateMongos() {}

    namespace {
        ScopedDbConnection* getConnectionForUsersCollection(const std::string& ns) {
            //
            // Note: The connection mechanism here is *not* ideal, and should not be used elsewhere.
            // If the primary for the collection moves, this approach may throw rather than handle
            // version exceptions.
            //

            DBConfigPtr config = grid.getDBConfig(ns);
            Shard s = config->getShard(ns);

            return new ScopedDbConnection(s.getConnString(), 30.0);
        }
    }

    Status AuthzManagerExternalStateMongos::_findUser(const string& usersNamespace,
                                                      const BSONObj& query,
                                                      BSONObj* result) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(usersNamespace));
            *result = conn->get()->findOne(usersNamespace, query).getOwned();
            conn->done();
            if (result->isEmpty()) {
                return userNotFoundStatus;
            }
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::insertPrivilegeDocument(const string& dbname,
                                                                    const BSONObj& userObj,
                                                                    const BSONObj& writeConcern) {
        try {
            const std::string userNS = "admin.system.users";
            scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(userNS));

            conn->get()->insert(userNS, userObj);

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            conn->get()->runCommand("admin", gleBuilder.done(), res);
            string errstr = conn->get()->getLastErrorString(res);
            conn->done();

            if (errstr.empty()) {
                return Status::OK();
            }
            if (res.hasField("code") && res["code"].Int() == ASSERT_ID_DUPKEY) {
                std::string name = userObj[AuthorizationManager::USER_NAME_FIELD_NAME].String();
                std::string source = userObj[AuthorizationManager::USER_SOURCE_FIELD_NAME].String();
                return Status(ErrorCodes::DuplicateKey,
                              mongoutils::str::stream() << "User \"" << name << "@" << source <<
                                      "\" already exists");
            }
            return Status(ErrorCodes::UserModificationFailed, errstr);
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::updatePrivilegeDocument(
            const UserName& user, const BSONObj& updateObj, const BSONObj& writeConcern) {
        try {
            const std::string userNS = "admin.system.users";
            scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(userNS));

            conn->get()->update(
                    userNS,
                    QUERY(AuthorizationManager::USER_NAME_FIELD_NAME << user.getUser() <<
                          AuthorizationManager::USER_SOURCE_FIELD_NAME << user.getDB()),
                    updateObj);

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            conn->get()->runCommand("admin", gleBuilder.done(), res);
            string err = conn->get()->getLastErrorString(res);
            conn->done();

            if (!err.empty()) {
                return Status(ErrorCodes::UserModificationFailed, err);
            }

            int numUpdated = res["n"].numberInt();
            dassert(numUpdated <= 1 && numUpdated >= 0);
            if (numUpdated == 0) {
                return Status(ErrorCodes::UserNotFound,
                              mongoutils::str::stream() << "User " << user.getFullName() <<
                                      " not found");
            }

            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::removePrivilegeDocuments(const BSONObj& query,
                                                                     const BSONObj& writeConcern,
                                                                     int* numRemoved) {
        try {
            string userNS = "admin.system.users";
            scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(userNS));

            conn->get()->remove(userNS, query);

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            conn->get()->runCommand("admin", gleBuilder.done(), res);
            string err = conn->get()->getLastErrorString(res);
            conn->done();

            if (!err.empty()) {
                return Status(ErrorCodes::UserModificationFailed, err);
            }

            *numRemoved = res["n"].numberInt();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::getAllDatabaseNames(
            std::vector<std::string>* dbnames) {
        try {
            scoped_ptr<ScopedDbConnection> conn(
                    getConnectionForUsersCollection(DatabaseType::ConfigNS));
            auto_ptr<DBClientCursor> c = conn->get()->query(DatabaseType::ConfigNS, Query());

            while (c->more()) {
                DatabaseType dbInfo;
                std::string errmsg;
                if (!dbInfo.parseBSON( c->nextSafe(), &errmsg) || !dbInfo.isValid( &errmsg )) {
                    return Status(ErrorCodes::FailedToParse, errmsg);
                }
                dbnames->push_back(dbInfo.getName());
            }
            conn->done();
            dbnames->push_back("config"); // config db isn't listed in config.databases
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::getAllV1PrivilegeDocsForDB(
            const std::string& dbname, std::vector<BSONObj>* privDocs) {
        try {
            std::string usersNamespace = dbname + ".system.users";
            scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(usersNamespace));
            auto_ptr<DBClientCursor> c = conn->get()->query(usersNamespace, Query());

            while (c->more()) {
                privDocs->push_back(c->nextSafe().getOwned());
            }
            conn->done();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::findOne(
            const NamespaceString& collectionName,
            const BSONObj& query,
            BSONObj* result) {
        fassertFailed(17101);
    }

    Status AuthzManagerExternalStateMongos::insert(
            const NamespaceString& collectionName,
            const BSONObj& document,
            const BSONObj& writeConcern) {
        fassertFailed(17102);
    }

    Status AuthzManagerExternalStateMongos::updateOne(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& updatePattern,
            bool upsert,
            const BSONObj& writeConcern) {
        fassertFailed(17103);
    }

    Status AuthzManagerExternalStateMongos::remove(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& writeConcern) {
        fassertFailed(17104);
    }

    Status AuthzManagerExternalStateMongos::createIndex(
            const NamespaceString& collectionName,
            const BSONObj& pattern,
            bool unique,
            const BSONObj& writeConcern) {
        fassertFailed(17105);
    }

    Status AuthzManagerExternalStateMongos::dropCollection(
            const NamespaceString& collectionName, const BSONObj& writeConcern) {
        fassertFailed(17106);
    }

    Status AuthzManagerExternalStateMongos::renameCollection(
            const NamespaceString& oldName,
            const NamespaceString& newName,
            const BSONObj& writeConcern) {
        fassertFailed(17107);
    }

    Status AuthzManagerExternalStateMongos::copyCollection(
            const NamespaceString& fromName,
            const NamespaceString& toName,
            const BSONObj& writeConcern) {
        fassertFailed(17108);
    }

    bool AuthzManagerExternalStateMongos::tryAcquireAuthzUpdateLock(const StringData& why) {
        if (_authzDataUpdateLock.get()) {
            return false;
        }
        _authzDataUpdateLock.reset(new ScopedDistributedLock(
                configServer.getConnectionString(), "authorizationData"));
        _authzDataUpdateLock->setLockMessage(why.toString());

        std::string errmsg;
        if (!_authzDataUpdateLock->tryAcquire(&errmsg)) {
            warning() <<
                    "Error while attempting to acquire distributed lock for user modification: " <<
                    errmsg << endl;
            _authzDataUpdateLock.reset();
            return false;
        }
        return true;
    }

    void AuthzManagerExternalStateMongos::releaseAuthzUpdateLock() {
        _authzDataUpdateLock.reset();
    }

} // namespace mongo
