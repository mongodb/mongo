/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <functional>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/auth/authz_manager_external_state_local.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/user_name.h"

namespace mongo {

/**
 * The implementation of AuthzManagerExternalState functionality for mongod.
 */
class AuthzManagerExternalStateMongod : public AuthzManagerExternalStateLocal {
    AuthzManagerExternalStateMongod(const AuthzManagerExternalStateMongod&) = delete;
    AuthzManagerExternalStateMongod& operator=(const AuthzManagerExternalStateMongod&) = delete;

public:
    AuthzManagerExternalStateMongod();
    virtual ~AuthzManagerExternalStateMongod();

    std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(
        AuthorizationManager* authzManager) override;

    virtual Status findOne(OperationContext* opCtx,
                           const NamespaceString& collectionName,
                           const BSONObj& query,
                           BSONObj* result);
    virtual Status query(OperationContext* opCtx,
                         const NamespaceString& collectionName,
                         const BSONObj& query,
                         const BSONObj& projection,
                         const std::function<void(const BSONObj&)>& resultProcessor);
};

}  // namespace mongo
