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

#include "mongo/db/auth/authz_manager_external_state.h"

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using std::string;

#ifndef _MSC_EXTENSIONS
    const long long AuthzManagerExternalState::_authzUpdateLockAcquisitionTimeoutMillis;
#endif

    AuthzManagerExternalState::AuthzManagerExternalState() {}
    AuthzManagerExternalState::~AuthzManagerExternalState() {}

    bool AuthzManagerExternalState::hasAnyPrivilegeDocuments(OperationContext* txn) {
        BSONObj userBSONObj;
        Status status = findOne(
                txn,
                AuthorizationManager::usersCollectionNamespace,
                BSONObj(),
                &userBSONObj);
        // If the status is NoMatchingDocument, there are no privilege documents.
        // If it's OK, there are.  Otherwise, we were unable to complete the query,
        // so best to assume that there _are_ privilege documents.  This might happen
        // if the node contaning the users collection becomes transiently unavailable.
        // See SERVER-12616, for example.
        return status != ErrorCodes::NoMatchingDocument;
    }


    Status AuthzManagerExternalState::insertPrivilegeDocument(OperationContext* txn,
                                                              const string& dbname,
                                                              const BSONObj& userObj,
                                                              const BSONObj& writeConcern) {
        Status status = insert(txn, NamespaceString("admin.system.users"), userObj, writeConcern);
        if (status.isOK()) {
            return status;
        }
        if (status.code() == ErrorCodes::DuplicateKey) {
            std::string name = userObj[AuthorizationManager::USER_NAME_FIELD_NAME].String();
            std::string source = userObj[AuthorizationManager::USER_DB_FIELD_NAME].String();
            return Status(ErrorCodes::DuplicateKey,
                          mongoutils::str::stream() << "User \"" << name << "@" << source <<
                                  "\" already exists");
        }
        if (status.code() == ErrorCodes::UnknownError) {
            return Status(ErrorCodes::UserModificationFailed, status.reason());
        }
        return status;
    }

    Status AuthzManagerExternalState::updatePrivilegeDocument(OperationContext* txn,
                                                              const UserName& user,
                                                              const BSONObj& updateObj,
                                                              const BSONObj& writeConcern) {
        Status status = updateOne(
                txn,
                NamespaceString("admin.system.users"),
                BSON(AuthorizationManager::USER_NAME_FIELD_NAME << user.getUser() <<
                     AuthorizationManager::USER_DB_FIELD_NAME << user.getDB()),
                updateObj,
                false,
                writeConcern);
        if (status.isOK()) {
            return status;
        }
        if (status.code() == ErrorCodes::NoMatchingDocument) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream() << "User " << user.getFullName() <<
                                  " not found");
        }
        if (status.code() == ErrorCodes::UnknownError) {
            return Status(ErrorCodes::UserModificationFailed, status.reason());
        }
        return status;
    }

    Status AuthzManagerExternalState::removePrivilegeDocuments(OperationContext* txn,
                                                               const BSONObj& query,
                                                               const BSONObj& writeConcern,
                                                               int* numRemoved) {
        Status status = remove(txn,
                               NamespaceString("admin.system.users"),
                               query,
                               writeConcern,
                               numRemoved);
        if (status.code() == ErrorCodes::UnknownError) {
            return Status(ErrorCodes::UserModificationFailed, status.reason());
        }
        return status;
    }

    Status AuthzManagerExternalState::updateOne(
            OperationContext* txn,
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& updatePattern,
            bool upsert,
            const BSONObj& writeConcern) {
        int nMatched;
        Status status = update(txn,
                               collectionName,
                               query,
                               updatePattern,
                               upsert,
                               false,
                               writeConcern,
                               &nMatched);
        if (!status.isOK()) {
            return status;
        }
        dassert(nMatched == 1 || nMatched == 0);
        if (nMatched == 0) {
            return Status(ErrorCodes::NoMatchingDocument, "No document found");
        }
        return Status::OK();
    }

}  // namespace mongo
