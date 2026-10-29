// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class Client;
class OperationContext;

namespace [[MONGO_MOD_PUBLIC]] auth {

class ValidatedTenancyScopeFactory {
public:
    /**
     * Parse the provided command {body} and {securityToken}.
     * 1. If an unsigned {securityToken} is provided, we delegate to parseUnsignedToken().
     * 2. If a signed {securityToken} is provided, we delegate to parseToken().
     *
     * If neither is provided, this method returns `boost::none`.
     */
    static boost::optional<ValidatedTenancyScope> parse(Client* client,
                                                        std::string_view securityToken);

    /**
     * Creates an HS256 signed token based on a pre-shared symmetric key.
     * Tokens using this signing algorithm are NOT suitable for production use, and both this
     * method, and the setParameter controlling the validation of this type of token are
     * intentionally restricted to test-only environments.
     */
    struct TokenForTestingTag {};
    static constexpr Minutes kDefaultExpiration{15};
    static ValidatedTenancyScope create(const UserName& username,
                                        std::string_view secret,
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
    static ValidatedTenancyScope parseUnsignedToken(Client* client, std::string_view securityToken);

    /**
     * Validates a JWS signature on the provided JWT header and token,
     * then extracts authenticatedUser, TenantId, and/or TenantProtocol.
     */
    static ValidatedTenancyScope parseToken(Client* client, std::string_view securityToken);
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
