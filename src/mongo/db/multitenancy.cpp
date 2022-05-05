/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/multitenancy.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/security_token.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

// Holds the tenantId for the operation if it was provided in the request on the $tenant field only
// if the tenantId was not also provided in the security token.
const auto dollarTenantDecoration =
    OperationContext::declareDecoration<boost::optional<mongo::TenantId>>();

void parseDollarTenantFromRequest(OperationContext* opCtx, const OpMsg& request) {
    // The internal security user is allowed to run commands on behalf of a tenant by passing
    // the tenantId in the "$tenant" field.
    auto tenantElem = request.body["$tenant"];
    if (!tenantElem)
        return;

    uassert(ErrorCodes::InvalidOptions,
            "Multitenancy not enabled, cannot set $tenant in command body",
            gMultitenancySupport);

    uassert(ErrorCodes::Unauthorized,
            "'$tenant' may only be specified with the useTenant action type",
            AuthorizationSession::get(opCtx->getClient())
                ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                   ActionType::useTenant));

    auto tenantId = TenantId::parseFromBSON(tenantElem);

    uassert(6223901,
            str::stream() << "Cannot pass $tenant id if also passing securityToken, securityToken: "
                          << auth::getSecurityToken(opCtx)->getAuthenticatedUser().getTenant()
                          << " $tenant: " << tenantId,
            !auth::getSecurityToken(opCtx));


    dollarTenantDecoration(opCtx) = std::move(tenantId);
    LOGV2_DEBUG(
        6223900, 4, "Setting tenantId from $tenant request parameter", "tenantId"_attr = tenantId);
}

boost::optional<TenantId> getActiveTenant(OperationContext* opCtx) {
    auto token = auth::getSecurityToken(opCtx);
    if (!token) {
        return dollarTenantDecoration(opCtx);
    }

    invariant(!dollarTenantDecoration(opCtx));
    return token->getAuthenticatedUser().getTenant();
}

}  // namespace mongo
