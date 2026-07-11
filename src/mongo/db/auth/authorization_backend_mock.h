// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_backend_local.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

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
