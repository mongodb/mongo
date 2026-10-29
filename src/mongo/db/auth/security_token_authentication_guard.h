// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace auth {

/**
 * If ValidatedTenancyScope represents an AuthenticatedUser,
 * that user will be authenticated against the client until this guard dies.
 * This is used in ServiceEntryPoint to scope authentication to a single operation.
 */
class [[MONGO_MOD_PUBLIC]] SecurityTokenAuthenticationGuard {
public:
    SecurityTokenAuthenticationGuard() = delete;
    SecurityTokenAuthenticationGuard(OperationContext*, const ValidatedTenancyScope&);
    ~SecurityTokenAuthenticationGuard();

private:
    Client* _client;
};

}  // namespace auth
}  // namespace mongo
