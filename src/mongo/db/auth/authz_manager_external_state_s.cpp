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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authz_manager_external_state_s.h"

#include <boost/thread/mutex.hpp>
#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/client/auth_helpers.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/config.h"
#include "mongo/s/distlock.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using boost::scoped_ptr;
    using std::endl;
    using std::vector;

    AuthzManagerExternalStateMongos::AuthzManagerExternalStateMongos() {}

    AuthzManagerExternalStateMongos::~AuthzManagerExternalStateMongos() {}

    Status AuthzManagerExternalStateMongos::initialize(OperationContext* txn) {
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

    Status AuthzManagerExternalStateMongos::getStoredAuthorizationVersion(
                                                OperationContext* txn, int* outVersion) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(
                    AuthorizationManager::usersCollectionNamespace));
            Status status = auth::getRemoteStoredAuthorizationVersion(conn->get(), outVersion);
            conn->done();
            return status;
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::getUserDescription(
                    OperationContext* txn, const UserName& userName, BSONObj* result) {
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
                                         AuthorizationManager::ROLE_DB_FIELD_NAME <<
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
            OperationContext* txn,
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
            OperationContext* txn,
            const NamespaceString& collectionName,
            const BSONObj& queryDoc,
            const BSONObj& projection,
            const stdx::function<void(const BSONObj&)>& resultProcessor) {
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

    Status AuthzManagerExternalStateMongos::insert(
            OperationContext* txn,
            const NamespaceString& collectionName,
            const BSONObj& document,
            const BSONObj& writeConcern) {

        return grid.catalogManager()->insert(collectionName, document, NULL);
    }

    Status AuthzManagerExternalStateMongos::update(OperationContext* txn,
                                                   const NamespaceString& collectionName,
                                                   const BSONObj& query,
                                                   const BSONObj& updatePattern,
                                                   bool upsert,
                                                   bool multi,
                                                   const BSONObj& writeConcern,
                                                   int* nMatched) {

        BatchedCommandResponse response;
        Status res = grid.catalogManager()->update(collectionName,
                                                   query,
                                                   updatePattern,
                                                   upsert,
                                                   multi,
                                                   &response);
        if (res.isOK()) {
            *nMatched = response.getN();
        }

        return res;
    }

    Status AuthzManagerExternalStateMongos::remove(
            OperationContext* txn,
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& writeConcern,
            int* numRemoved) {

        BatchedCommandResponse response;

        Status res = grid.catalogManager()->remove(collectionName, query, 0, &response);
        if (res.isOK()) {
            *numRemoved = response.getN();
        }

        return res;
    }

    bool AuthzManagerExternalStateMongos::tryAcquireAuthzUpdateLock(StringData why) {
        boost::lock_guard<boost::mutex> lkLocal(_distLockGuard);
        if (_authzDataUpdateLock.get()) {
            return false;
        }

        // Temporarily put into an auto_ptr just in case there is an exception thrown during
        // lock acquisition.
        std::auto_ptr<ScopedDistributedLock> lockHolder(new ScopedDistributedLock(
                configServer.getConnectionString(), "authorizationData"));
        lockHolder->setLockMessage(why.toString());

        Status acquisitionStatus = lockHolder->acquire(_authzUpdateLockAcquisitionTimeoutMillis);
        if (!acquisitionStatus.isOK()) {
            warning() <<
                    "Error while attempting to acquire distributed lock for user modification: " <<
                    acquisitionStatus.toString() << endl;
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
