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

#include "mongo/db/auth/authz_manager_external_state_d.h"

#include <boost/thread/mutex.hpp>
#include <boost/date_time/time_duration.hpp>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthzManagerExternalStateMongod::AuthzManagerExternalStateMongod() {}
    AuthzManagerExternalStateMongod::~AuthzManagerExternalStateMongod() {}

    Status AuthzManagerExternalStateMongod::_getUserDocument(
                OperationContext* txn, const UserName& userName, BSONObj* userDoc) {

        Client::ReadContext ctx(txn, "admin");

        int authzVersion;
        Status status = getStoredAuthorizationVersion(txn, &authzVersion);
        if (!status.isOK())
            return status;

        switch (authzVersion) {
        case AuthorizationManager::schemaVersion26Upgrade:
        case AuthorizationManager::schemaVersion26Final:
            break;
        default:
            return Status(ErrorCodes::AuthSchemaIncompatible, mongoutils::str::stream() <<
                          "Unsupported schema version for getUserDescription(): " <<
                          authzVersion);
        }

        status = findOne(
                txn,
                (authzVersion == AuthorizationManager::schemaVersion26Final ?
                 AuthorizationManager::usersCollectionNamespace :
                 AuthorizationManager::usersAltCollectionNamespace),
                BSON(AuthorizationManager::USER_NAME_FIELD_NAME << userName.getUser() <<
                     AuthorizationManager::USER_DB_FIELD_NAME << userName.getDB()),
                userDoc);
        if (status == ErrorCodes::NoMatchingDocument) {
            status = Status(ErrorCodes::UserNotFound, mongoutils::str::stream() <<
                            "Could not find user " << userName.getFullName());
        }
        return status;
    }

    Status AuthzManagerExternalStateMongod::query(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& projection,
            const stdx::function<void(const BSONObj&)>& resultProcessor) {
        try {
            DBDirectClient client;
            client.query(resultProcessor, collectionName.ns(), query, &projection);
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongod::getAllDatabaseNames(
                OperationContext* txn, std::vector<std::string>* dbnames) {
        globalStorageEngine->listDatabases( dbnames );
        return Status::OK();
    }

    Status AuthzManagerExternalStateMongod::findOne(
            OperationContext* txn,
            const NamespaceString& collectionName,
            const BSONObj& query,
            BSONObj* result) {

        Client::ReadContext ctx(txn, collectionName.ns());

        BSONObj found;
        if (Helpers::findOne(txn,
                             ctx.ctx().db()->getCollection(txn, collectionName),
                             query,
                             found)) {
            *result = found.getOwned();
            return Status::OK();
        }
        return Status(ErrorCodes::NoMatchingDocument, mongoutils::str::stream() <<
                      "No document in " << collectionName.ns() << " matches " << query);
    }

    Status AuthzManagerExternalStateMongod::insert(
            const NamespaceString& collectionName,
            const BSONObj& document,
            const BSONObj& writeConcern) {
        try {
            DBDirectClient client;
            client.insert(collectionName, document);

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            client.runCommand("admin", gleBuilder.done(), res);
            string errstr = client.getLastErrorString(res);
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

    Status AuthzManagerExternalStateMongod::update(const NamespaceString& collectionName,
                                                   const BSONObj& query,
                                                   const BSONObj& updatePattern,
                                                   bool upsert,
                                                   bool multi,
                                                   const BSONObj& writeConcern,
                                                   int* nMatched) {
        try {
            DBDirectClient client;
            client.update(collectionName, query, updatePattern, upsert, multi);

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            client.runCommand("admin", gleBuilder.done(), res);
            string err = client.getLastErrorString(res);
            if (!err.empty()) {
                return Status(ErrorCodes::UnknownError, err);
            }

            *nMatched = res["n"].numberInt();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongod::remove(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& writeConcern,
            int* numRemoved) {
        try {
            DBDirectClient client;
            client.remove(collectionName, query);

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            client.runCommand("admin", gleBuilder.done(), res);
            string errstr = client.getLastErrorString(res);
            if (!errstr.empty()) {
                return Status(ErrorCodes::UnknownError, errstr);
            }

            *numRemoved = res["n"].numberInt();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongod::createIndex(
            const NamespaceString& collectionName,
            const BSONObj& pattern,
            bool unique,
            const BSONObj& writeConcern) {
        DBDirectClient client;
        try {
            if (client.ensureIndex(collectionName.ns(),
                                   pattern,
                                   unique)) {
                BSONObjBuilder gleBuilder;
                gleBuilder.append("getLastError", 1);
                gleBuilder.appendElements(writeConcern);
                BSONObj res;
                client.runCommand("admin", gleBuilder.done(), res);
                string errstr = client.getLastErrorString(res);
                if (!errstr.empty()) {
                    return Status(ErrorCodes::UnknownError, errstr);
                }
            }
            return Status::OK();
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongod::dropIndexes(
            const NamespaceString& collectionName,
            const BSONObj& writeConcern) {
        DBDirectClient client;
        try {
            client.dropIndexes(collectionName.ns());
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            client.runCommand("admin", gleBuilder.done(), res);
            string errstr = client.getLastErrorString(res);
            if (!errstr.empty()) {
                return Status(ErrorCodes::UnknownError, errstr);
            }
            return Status::OK();
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

    bool AuthzManagerExternalStateMongod::tryAcquireAuthzUpdateLock(const StringData& why) {
        LOG(2) << "Attempting to lock user data for: " << why << endl;
        return _authzDataUpdateLock.timed_lock(
                boost::posix_time::milliseconds(_authzUpdateLockAcquisitionTimeoutMillis));
    }

    void AuthzManagerExternalStateMongod::releaseAuthzUpdateLock() {
        return _authzDataUpdateLock.unlock();
    }

} // namespace mongo
