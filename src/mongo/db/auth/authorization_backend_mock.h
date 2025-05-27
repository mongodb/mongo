/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_backend_local.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::auth {

class AuthorizationBackendMock : public AuthorizationBackendLocal {
public:
    /**
     * Inserts the given user object into the "admin" database.
     */
    Status insertUserDocument(OperationContext* opCtx,
                              const BSONObj& userObj,
                              const BSONObj& writeConcern);

    /**
     * Inserts the given role object into the "admin" database.
     */
    Status insertRoleDocument(OperationContext* opCtx,
                              const BSONObj& roleObj,
                              const BSONObj& writeConcern);

    // This implementation does not understand uniqueness constraints, ignores writeConcern,
    // and only correctly handles some upsert behaviors.
    Status updateOne(OperationContext* opCtx,
                     const NamespaceString& collectionName,
                     const BSONObj& query,
                     const BSONObj& updatePattern,
                     bool upsert,
                     const BSONObj& writeConcern);

    // This implementation does not understand uniqueness constraints.
    Status insert(OperationContext* opCtx,
                  const NamespaceString& collectionName,
                  const BSONObj& document,
                  const BSONObj& writeConcern);

    Status remove(OperationContext* opCtx,
                  const NamespaceString& collectionName,
                  const BSONObj& query,
                  const BSONObj& writeConcern,
                  int* numRemoved);

    Status findOne(OperationContext* opCtx,
                   const NamespaceString& collectionName,
                   const BSONObj& query,
                   BSONObj* result) final;

    void setFindsShouldFail(bool enable) {
        _findsShouldFail = enable;
    }

    /**
     * Calls the base resolveRoles, but is exposed for testing purposes.
     */
    StatusWith<ResolvedRoleData> resolveRoles_forTest(OperationContext* opCtx,
                                                      const std::vector<RoleName>& roleNames,
                                                      ResolveRoleOption option) {
        return resolveRoles(opCtx, roleNames, option);
    }

protected:
    RolesSnapshot _snapshotRoles(OperationContext* opCtx) override {
        return RolesSnapshot();
    }

private:
    typedef std::vector<BSONObj> BSONObjCollection;
    typedef std::map<NamespaceString, BSONObjCollection> NamespaceDocumentMap;

    Status _findOneIter(OperationContext* opCtx,
                        const NamespaceString& collectionName,
                        const BSONObj& query,
                        BSONObjCollection::iterator* result);

    Status _queryVector(OperationContext* opCtx,
                        const NamespaceString& collectionName,
                        const BSONObj& query,
                        std::vector<BSONObjCollection::iterator>* result);


    AuthorizationManager* _authzManager;  // For reporting logOps.
    NamespaceDocumentMap _documents;      // Mock database.
    bool _findsShouldFail = false;        // Useful for AuthorizationSessionTest.
};

}  // namespace mongo::auth
