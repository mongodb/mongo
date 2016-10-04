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

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authz_manager_external_state_local.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/stdx/functional.h"

namespace mongo {

/**
 * The implementation of AuthzManagerExternalState functionality for mongod.
 */
class AuthzManagerExternalStateMongod : public AuthzManagerExternalStateLocal {
    MONGO_DISALLOW_COPYING(AuthzManagerExternalStateMongod);

public:
    AuthzManagerExternalStateMongod();
    virtual ~AuthzManagerExternalStateMongod();

    std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(
        AuthorizationManager* authzManager) override;

    virtual Status findOne(OperationContext* txn,
                           const NamespaceString& collectionName,
                           const BSONObj& query,
                           BSONObj* result);
    virtual Status query(OperationContext* txn,
                         const NamespaceString& collectionName,
                         const BSONObj& query,
                         const BSONObj& projection,
                         const stdx::function<void(const BSONObj&)>& resultProcessor);
};

}  // namespace mongo
