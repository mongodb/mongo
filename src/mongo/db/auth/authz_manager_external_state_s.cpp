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

#include <boost/thread/mutex.hpp>
#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/config.h"
#include "mongo/s/distlock.h"
#include "mongo/s/type_database.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthzManagerExternalStateMongos::AuthzManagerExternalStateMongos() {}

    AuthzManagerExternalStateMongos::~AuthzManagerExternalStateMongos() {}

    Status AuthzManagerExternalStateMongos::initialize() {
        return Status::OK();
    }

    namespace {
        ScopedDbConnection* getConnectionForAuthzCollection(const NamespaceString& ns) {
            //
            // Note: The connection mechanism here is *not* ideal, and should not be used elsewhere.
            // If the primary for the collection moves, this approach may throw rather than handle
            // version exceptions.
            //

            DBConfigPtr config = grid.getDBConfig(ns.ns());
            Shard s = config->getShard(ns.ns());

            return new ScopedDbConnection(s.getConnString(), 30.0);
        }
    }

    Status AuthzManagerExternalStateMongos::getStoredAuthorizationVersion(int* outVersion) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(
                    AuthorizationManager::usersCollectionNamespace));
            BSONObj cmdResult;
            conn->get()->runCommand(
                    "admin",
                    BSON("getParameter" << 1 <<
                         AuthorizationManager::schemaVersionServerParameter << 1),
                    cmdResult);
            if (!cmdResult["ok"].trueValue()) {
                std::string errmsg = cmdResult["errmsg"].str();
                if (errmsg == "no option found to get" ||
                    StringData(errmsg).startsWith("no such cmd")) {

                    *outVersion = 1;
                    conn->done();
                    return Status::OK();
                }
                int code = cmdResult["code"].numberInt();
                if (code == 0) {
                    code = ErrorCodes::UnknownError;
                }
                return Status(ErrorCodes::Error(code), errmsg);
            }
            BSONElement versionElement =
                cmdResult[AuthorizationManager::schemaVersionServerParameter];
            if (versionElement.eoo())
                return Status(ErrorCodes::UnknownError, "getParameter misbehaved.");
            *outVersion = versionElement.numberInt();
            conn->done();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::getUserDescription(const UserName& userName,
                                                               BSONObj* result) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(
                    AuthorizationManager::usersCollectionNamespace));
            BSONObj cmdResult;
            conn->get()->runCommand(
                    "admin",
                    BSON("usersInfo" <<
                         BSON_ARRAY(BSON(AuthorizationManager::USER_NAME_FIELD_NAME <<
                                         userName.getUser() <<
                                         AuthorizationManager::USER_DB_FIELD_NAME <<
                                         userName.getDB())) <<
                         "showPrivileges" << true <<
                         "showCredentials" << true),
                    cmdResult);
            if (!cmdResult["ok"].trueValue()) {
                int code = cmdResult["code"].numberInt();
                if (code == 0) code = ErrorCodes::UnknownError;
                return Status(ErrorCodes::Error(code), cmdResult["errmsg"].str());
            }

            std::vector<BSONElement> foundUsers = cmdResult["users"].Array();
            if (foundUsers.size() == 0) {
                return Status(ErrorCodes::UserNotFound,
                              "User \"" + userName.toString() + "\" not found");
            }
            if (foundUsers.size() > 1) {
                return Status(ErrorCodes::UserDataInconsistent,
                              mongoutils::str::stream() << "Found multiple users on the \"" <<
                                      userName.getDB() << "\" database with name \"" <<
                                      userName.getUser() << "\"");
            }
            *result = foundUsers[0].Obj().getOwned();
            conn->done();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::getRoleDescription(const RoleName& roleName,
                                                               bool showPrivileges,
                                                               BSONObj* result) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(
                    AuthorizationManager::rolesCollectionNamespace));
            BSONObj cmdResult;
            conn->get()->runCommand(
                    "admin",
                    BSON("rolesInfo" <<
                         BSON_ARRAY(BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME <<
                                         roleName.getRole() <<
                                         AuthorizationManager::ROLE_SOURCE_FIELD_NAME <<
                                         roleName.getDB())) <<
                         "showPrivileges" << showPrivileges),
                    cmdResult);
            if (!cmdResult["ok"].trueValue()) {
                int code = cmdResult["code"].numberInt();
                if (code == 0) code = ErrorCodes::UnknownError;
                return Status(ErrorCodes::Error(code), cmdResult["errmsg"].str());
            }

            std::vector<BSONElement> foundRoles = cmdResult["roles"].Array();
            if (foundRoles.size() == 0) {
                return Status(ErrorCodes::RoleNotFound,
                              "Role \"" + roleName.toString() + "\" not found");
            }
            if (foundRoles.size() > 1) {
                return Status(ErrorCodes::RoleDataInconsistent,
                              mongoutils::str::stream() << "Found multiple roles on the \"" <<
                                      roleName.getDB() << "\" database with name \"" <<
                                      roleName.getRole() << "\"");
            }
            *result = foundRoles[0].Obj().getOwned();
            conn->done();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::getRoleDescriptionsForDB(const std::string dbname,
                                                                     bool showPrivileges,
                                                                     bool showBuiltinRoles,
                                                                     vector<BSONObj>* result) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(
                    AuthorizationManager::rolesCollectionNamespace));
            BSONObj cmdResult;
            conn->get()->runCommand(
                    dbname,
                    BSON("rolesInfo" << 1 <<
                         "showPrivileges" << showPrivileges <<
                         "showBuiltinRoles" << showBuiltinRoles),
                    cmdResult);
            if (!cmdResult["ok"].trueValue()) {
                int code = cmdResult["code"].numberInt();
                if (code == 0) code = ErrorCodes::UnknownError;
                return Status(ErrorCodes::Error(code), cmdResult["errmsg"].str());
            }
            for (BSONObjIterator it(cmdResult["roles"].Obj()); it.more(); it.next()) {
                result->push_back((*it).Obj().getOwned());
            }
            conn->done();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::findOne(
            const NamespaceString& collectionName,
            const BSONObj& queryDoc,
            BSONObj* result) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(collectionName));
            Query query(queryDoc);
            query.readPref(ReadPreference_PrimaryPreferred, BSONArray());
            *result = conn->get()->findOne(collectionName, query).getOwned();
            conn->done();
            if (result->isEmpty()) {
                return Status(ErrorCodes::NoMatchingDocument, mongoutils::str::stream() <<
                              "No document in " << collectionName.ns() << " matches " << queryDoc);
            }
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::query(
            const NamespaceString& collectionName,
            const BSONObj& queryDoc,
            const BSONObj& projection,
            const boost::function<void(const BSONObj&)>& resultProcessor) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(collectionName));
            Query query(queryDoc);
            query.readPref(ReadPreference_PrimaryPreferred, BSONArray());
            conn->get()->query(resultProcessor, collectionName.ns(), query, &projection);
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::getAllDatabaseNames(
            std::vector<std::string>* dbnames) {
        try {
            scoped_ptr<ScopedDbConnection> conn(
                    getConnectionForAuthzCollection(NamespaceString(DatabaseType::ConfigNS)));
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

    Status AuthzManagerExternalStateMongos::insert(
            const NamespaceString& collectionName,
            const BSONObj& document,
            const BSONObj& writeConcern) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(collectionName));

            conn->get()->insert(collectionName, document);

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
                return Status(ErrorCodes::DuplicateKey, errstr);
            }
            return Status(ErrorCodes::UnknownError, errstr);
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::update(const NamespaceString& collectionName,
                                                   const BSONObj& query,
                                                   const BSONObj& updatePattern,
                                                   bool upsert,
                                                   bool multi,
                                                   const BSONObj& writeConcern,
                                                   int* numUpdated) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(collectionName));

            conn->get()->update(collectionName, query, updatePattern, upsert, multi);

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            conn->get()->runCommand("admin", gleBuilder.done(), res);
            string err = conn->get()->getLastErrorString(res);
            conn->done();

            if (!err.empty()) {
                return Status(ErrorCodes::UnknownError, err);
            }

            *numUpdated = res["n"].numberInt();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::remove(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& writeConcern,
            int* numRemoved) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(collectionName));

            conn->get()->remove(collectionName, query);

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            conn->get()->runCommand("admin", gleBuilder.done(), res);
            string err = conn->get()->getLastErrorString(res);
            conn->done();

            if (!err.empty()) {
                return Status(ErrorCodes::UnknownError, err);
            }

            *numRemoved = res["n"].numberInt();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::createIndex(
            const NamespaceString& collectionName,
            const BSONObj& pattern,
            bool unique,
            const BSONObj& writeConcern) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(collectionName));

            if (conn->get()->ensureIndex(collectionName.ns(), pattern, unique)) {

                // Handle write concern
                BSONObjBuilder gleBuilder;
                gleBuilder.append("getLastError", 1);
                gleBuilder.appendElements(writeConcern);
                BSONObj res;
                conn->get()->runCommand("admin", gleBuilder.done(), res);
                string err = conn->get()->getLastErrorString(res);

                if (!err.empty()) {
                    conn->done();
                    return Status(ErrorCodes::UnknownError, err);
                }
            }
            conn->done();
            return Status::OK();

        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::dropIndexes(
            const NamespaceString& collectionName,
            const BSONObj& writeConcern) {

        scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(collectionName));
        try {
            conn->get()->dropIndexes(collectionName.ns());
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            conn->get()->runCommand("admin", gleBuilder.done(), res);
            string errstr = conn->get()->getLastErrorString(res);
            if (!errstr.empty()) {
                conn->done();
                return Status(ErrorCodes::UnknownError, errstr);
            }
            conn->done();
            return Status::OK();
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

    bool AuthzManagerExternalStateMongos::tryAcquireAuthzUpdateLock(const StringData& why) {
        boost::lock_guard<boost::mutex> lkLocal(_distLockGuard);
        if (_authzDataUpdateLock.get()) {
            return false;
        }

        // Temporarily put into an auto_ptr just in case there is an exception thrown during
        // lock acquisition.
        std::auto_ptr<ScopedDistributedLock> lockHolder(new ScopedDistributedLock(
                configServer.getConnectionString(), "authorizationData"));
        lockHolder->setLockMessage(why.toString());

        std::string errmsg;
        if (!lockHolder->acquire(_authzUpdateLockAcquisitionTimeoutMillis, &errmsg)) {
            warning() <<
                    "Error while attempting to acquire distributed lock for user modification: " <<
                    errmsg << endl;
            return false;
        }
        _authzDataUpdateLock.reset(lockHolder.release());
        return true;
    }

    void AuthzManagerExternalStateMongos::releaseAuthzUpdateLock() {
        boost::lock_guard<boost::mutex> lkLocal(_distLockGuard);
        _authzDataUpdateLock.reset();
    }

} // namespace mongo
