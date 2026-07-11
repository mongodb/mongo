// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authz_session_external_state_router.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

class AuthorizationClientHandleRouter final : public AuthorizationClientHandle {
public:
    AuthorizationClientHandleRouter() = default;

    std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(Client* client) final {
        return std::make_unique<AuthzSessionExternalStateRouter>(client);
    }

    void notifyDDLOperation(OperationContext* opCtx,
                            AuthorizationRouter* authzRouter,
                            std::string_view op,
                            const NamespaceString& nss,
                            const BSONObj& o,
                            const BSONObj* o2) final {}

protected:
    AuthorizationClientHandleRouter(const AuthorizationClientHandleRouter&) = delete;
    AuthorizationClientHandleRouter& operator=(const AuthorizationClientHandleRouter&) = delete;

    StatusWith<BSONObj> runAuthorizationReadCommand(OperationContext* opCtx,
                                                    const DatabaseName& dbname,
                                                    const BSONObj& command) final;
};

}  // namespace mongo
