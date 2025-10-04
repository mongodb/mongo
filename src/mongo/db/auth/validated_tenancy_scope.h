/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/auth/user_name.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/time_support.h"

#include <utility>
#include <variant>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class Client;
class OperationContext;

namespace auth {

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

    StringData getOriginalToken() const {
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
                          StringData securityToken,
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
