// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/user_name.h"
#include "mongo/util/modules.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/time_support.h"

#include <string_view>
#include <utility>
#include <variant>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class Client;
class OperationContext;

namespace [[MONGO_MOD_PUBLIC]] auth {

class ValidatedTenancyScope {
public:
    /**
     * For use in methods where we are absolutely sure we do not need to consider tenant domain.
     *
     * @note Please use this very carefully, ensuring it's intentionally omitted for the specific
     * use case.
     */
    static const ValidatedTenancyScope kNotRequired;

    enum class TenantProtocol { kDefault, kAtlasProxy };

    ValidatedTenancyScope() = default;
    ValidatedTenancyScope(const ValidatedTenancyScope&) = default;

    bool operator==(const ValidatedTenancyScope& rhs) const {
        return _originalToken == rhs._originalToken;
    }
    bool operator!=(const ValidatedTenancyScope& rhs) const {
        return !(*this == rhs);
    }

    bool hasAuthenticatedUser() const;

    const UserName& authenticatedUser() const;

    bool isValid() const {
        return !std::holds_alternative<std::monostate>(_tenantOrUser) && !_originalToken.empty();
    }

    bool hasTenantId() const {
        return visit(OverloadedVisitor{
                         [](const std::monostate&) { return false; },
                         [](const UserName& userName) { return !!userName.tenantId(); },
                         [](const TenantId& tenant) { return true; },
                     },
                     _tenantOrUser);
    }

    TenantId tenantId() const {
        return visit(OverloadedVisitor{
                         [](const std::monostate&) -> TenantId { MONGO_UNREACHABLE; },
                         [](const UserName& userName) { return *userName.tenantId(); },
                         [](const TenantId& tenant) { return tenant; },
                     },
                     _tenantOrUser);
    }

    std::string_view getOriginalToken() const {
        return _originalToken;
    }

    /**
     * Return true if the tenant protocol parsed from the mongodb/expectPrefix field is AtlasProxy.
     * Atlas proxy is the only protocol with `expectPrefix` enabled.
     */
    bool isFromAtlasProxy() const {
        return _tenantProtocol == TenantProtocol::kAtlasProxy;
    }

    Date_t getExpiration() const {
        return _expiration;
    }

    /**
     * Get/Set a ValidatedTenancyScope as a decoration on the OperationContext
     */
    static const boost::optional<ValidatedTenancyScope>& get(OperationContext* opCtx);
    static void set(OperationContext* opCtx, boost::optional<ValidatedTenancyScope> token);

private:
    friend class ValidatedTenancyScopeFactory;

    /**
     * Private constructor to be called only by the ValidatedTenancyScopeFactory in order to
     * enforce authorization and validation of a parsed security token prior to constructing a
     * ValidatedTenancyScope.
     */
    ValidatedTenancyScope(Client* client,
                          std::string_view securityToken,
                          const std::variant<std::monostate, UserName, TenantId>& tenantOrUser,
                          Date_t expiration,
                          TenantProtocol tenantProtocol)
        : _originalToken(std::string{securityToken}),
          _expiration(expiration),
          _tenantOrUser(tenantOrUser),
          _tenantProtocol(tenantProtocol) {}

    /**
     * Private constructor to be called only by the ValidatedTenancyScopeFactory in order to
     * enforce authorization of a authenticated user's tenantId prior to constructing a
     * ValidatedTenancyScope.
     */
    explicit ValidatedTenancyScope(TenantId tenant) : _tenantOrUser(std::move(tenant)) {}

    /**
     * The constructors below are for specific cases and should remain private and only used through
     * the ValidatedTenancyScopeFactory::create calls.
     */
    explicit ValidatedTenancyScope(const UserName& userName,
                                   const std::string& token,
                                   Date_t expiration,
                                   TenantProtocol protocol)
        : _originalToken(token),
          _expiration(expiration),
          _tenantOrUser(userName),
          _tenantProtocol(protocol) {}

    /**
     * The constructors below are private and to be used through the ValidatedTenancyScopeFactory
     * only in specific cases such as testing, shell or OpMsgRequests when a security context is not
     * available. See ValidatedTenancyScopeFactory for  more details.
     */
    explicit ValidatedTenancyScope(const std::string& token, TenantProtocol protocol)
        : _originalToken(token), _tenantProtocol(protocol) {}

    explicit ValidatedTenancyScope(const std::string& token,
                                   TenantId tenant,
                                   TenantProtocol protocol)
        : _originalToken(token), _tenantOrUser(std::move(tenant)), _tenantProtocol(protocol) {}

    explicit ValidatedTenancyScope(std::string token) : _originalToken(std::move(token)) {}

    // Preserve original token for serializing.
    std::string _originalToken;

    // Expiration time if any.
    Date_t _expiration = Date_t::max();

    // monostate represents a VTS which has not actually been validated.
    // It should only persist into construction within the shell,
    // where VTS is used for sending token data to a server via _originalBSON.
    std::variant<std::monostate, UserName, TenantId> _tenantOrUser;

    // Define the protocol used by the connection to the server. It will only be set to AtlasProxy
    // if the token received contains `expectPrefix` to true and will be changed only once.
    TenantProtocol _tenantProtocol{TenantProtocol::kDefault};
};

}  // namespace auth
}  // namespace mongo
