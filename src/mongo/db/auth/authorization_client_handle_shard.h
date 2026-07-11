// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authz_session_external_state_shard.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <string_view>

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
                            std::string_view op,
                            const NamespaceString& nss,
                            const BSONObj& o,
                            const BSONObj* o2) final;

public:
    AuthorizationClientHandleShard() = default;
};

}  // namespace mongo
