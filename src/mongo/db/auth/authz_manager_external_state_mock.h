/*
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

#pragma once

#include <map>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authz_manager_external_state_local.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class AuthorizationManager;

/**
 * Mock of the AuthzManagerExternalState class used only for testing.
 */
class AuthzManagerExternalStateMock : public AuthzManagerExternalStateLocal {
    MONGO_DISALLOW_COPYING(AuthzManagerExternalStateMock);

public:
    AuthzManagerExternalStateMock();
    virtual ~AuthzManagerExternalStateMock();

    void setAuthorizationManager(AuthorizationManager* authzManager);
    void setAuthzVersion(int version);

    std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(
        AuthorizationManager* authzManager) override;

    virtual Status findOne(OperationContext* txn,
                           const NamespaceString& collectionName,
                           const BSONObj& query,
                           BSONObj* result);

    virtual Status query(OperationContext* txn,
                         const NamespaceString& collectionName,
                         const BSONObj& query,
                         const BSONObj& projection,  // Currently unused in mock
                         const stdx::function<void(const BSONObj&)>& resultProcessor);

    /**
     * Inserts the given user object into the "admin" database.
     */
    Status insertPrivilegeDocument(OperationContext* txn,
                                   const BSONObj& userObj,
                                   const BSONObj& writeConcern);

    // This implementation does not understand uniqueness constraints.
    virtual Status insert(OperationContext* txn,
                          const NamespaceString& collectionName,
                          const BSONObj& document,
                          const BSONObj& writeConcern);

    // This implementation does not understand uniqueness constraints, ignores writeConcern,
    // and only correctly handles some upsert behaviors.
    virtual Status updateOne(OperationContext* txn,
                             const NamespaceString& collectionName,
                             const BSONObj& query,
                             const BSONObj& updatePattern,
                             bool upsert,
                             const BSONObj& writeConcern);
    virtual Status update(OperationContext* txn,
                          const NamespaceString& collectionName,
                          const BSONObj& query,
                          const BSONObj& updatePattern,
                          bool upsert,
                          bool multi,
                          const BSONObj& writeConcern,
                          int* nMatched);
    virtual Status remove(OperationContext* txn,
                          const NamespaceString& collectionName,
                          const BSONObj& query,
                          const BSONObj& writeConcern,
                          int* numRemoved);

    std::vector<BSONObj> getCollectionContents(const NamespaceString& collectionName);

private:
    typedef std::vector<BSONObj> BSONObjCollection;
    typedef std::map<NamespaceString, BSONObjCollection> NamespaceDocumentMap;

    Status _findOneIter(const NamespaceString& collectionName,
                        const BSONObj& query,
                        BSONObjCollection::iterator* result);

    Status _queryVector(const NamespaceString& collectionName,
                        const BSONObj& query,
                        std::vector<BSONObjCollection::iterator>* result);


    AuthorizationManager* _authzManager;  // For reporting logOps.
    NamespaceDocumentMap _documents;      // Mock database.
};

}  // namespace mongo
