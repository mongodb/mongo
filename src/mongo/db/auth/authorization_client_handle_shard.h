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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authz_session_external_state_shard.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

class AuthorizationClientHandleShard final : public AuthorizationClientHandle {
    AuthorizationClientHandleShard(const AuthorizationClientHandleShard&) = delete;
    AuthorizationClientHandleShard& operator=(const AuthorizationClientHandleShard&) = delete;

    std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(Client* client) final {
        return std::make_unique<AuthzSessionExternalStateShard>(client);
    }

    StatusWith<BSONObj> runAuthorizationReadCommand(OperationContext* opCtx,
                                                    const DatabaseName& dbname,
                                                    const BSONObj& command) final;

    void notifyDDLOperation(OperationContext* opCtx,
                            AuthorizationRouter* authzRouter,
                            StringData op,
                            const NamespaceString& nss,
                            const BSONObj& o,
                            const BSONObj* o2) final;

public:
    AuthorizationClientHandleShard() = default;
};

}  // namespace mongo
