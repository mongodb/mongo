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
#include "mongo/db/auth/validated_tenancy_scope.h"

namespace mongo {

class Client;
class OperationContext;

namespace auth {

class ValidatedTenancyScopeFactory {
public:
    /**
     * Parse the provided command {body} and {securityToken}.
     * 1. If an unsigned {securityToken} is provided, we delegate to parseUnsignedToken().
     * 2. If a signed {securityToken} is provided, we delegate to parseToken().
     *
     * If neither is provided, this method returns `boost::none`.
     */
    static boost::optional<ValidatedTenancyScope> parse(Client* client, StringData securityToken);

    /**
     * Creates an HS256 signed token based on a pre-shared symmetric key.
     * Tokens using this signing algorithm are NOT suitable for production use, and both this
     * method, and the setParameter controlling the validation of this type of token are
     * intentionally restricted to test-only environments.
     */
    struct TokenForTestingTag {};
    static constexpr Minutes kDefaultExpiration{15};
    static ValidatedTenancyScope create(const UserName& username,
                                        StringData secret,
                                        ValidatedTenancyScope::TenantProtocol protocol,
                                        TokenForTestingTag);

    /**
     * Setup a validated tenant for test, do not use outside of test code.
     */
    struct TenantForTestingTag {};
    static ValidatedTenancyScope create(TenantId tenant,
                                        ValidatedTenancyScope::TenantProtocol protocol,
                                        TenantForTestingTag);

    /**
     * Initializes a VTS object with original BSON only.
     * Used by shell to prepare outgoing OpMsg requests.
     */
    struct InitForShellTag {};
    static ValidatedTenancyScope create(std::string token, InitForShellTag);

    /**
     * Backdoor API to setup a validated tenant. For use only when a security context is not
     * available.
     */
    struct TrustedForInnerOpMsgRequestTag {};
    static ValidatedTenancyScope create(TenantId tenant, TrustedForInnerOpMsgRequestTag);

private:
    /**
     * Transitional token mode used to convey TenantId and Protocol ONLY.
     * These tokens do not need to be signed, however, they are only valid
     * when provided by clients who are already authenticated and posess
     * cluster{useTenant} privilege.
     */
    static ValidatedTenancyScope parseUnsignedToken(Client* client, StringData securityToken);

    /**
     * Validates a JWS signature on the provided JWT header and token,
     * then extracts authenticatedUser, TenantId, and/or TenantProtocol.
     */
    static ValidatedTenancyScope parseToken(Client* client, StringData securityToken);
};

/**
 * An RAII type used with the DBDirectClient to reset the tenancy scope of an operation context,
 * since the DBDirectClient reuses the same context, invalidating tenant isolation guardrails.
 */
class ValidatedTenancyScopeGuard {
public:
    ValidatedTenancyScopeGuard(OperationContext* opCtx);
    ~ValidatedTenancyScopeGuard();

    ValidatedTenancyScopeGuard(const ValidatedTenancyScopeGuard&) = delete;
    void operator=(const ValidatedTenancyScopeGuard&) = delete;

    /**
     * Run the provided work within a tenant context, establishing an operation context with a
     * validated tenancy scope for handling inner requests.
     */
    static void runAsTenant(OperationContext* opCtx,
                            const boost::optional<TenantId>& tenantId,
                            std::function<void()> workFunc);

private:
    OperationContext* _opCtx{nullptr};
    boost::optional<ValidatedTenancyScope> _validatedTenancyScope;
    boost::optional<auth::ValidatedTenancyScope::TenantProtocol> _tenantProtocol;
};

}  // namespace auth
}  // namespace mongo
