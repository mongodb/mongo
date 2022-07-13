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

#include <boost/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo {

class Client;
class OperationContext;

namespace auth {

class ValidatedTenancyScope {
public:
    ValidatedTenancyScope() = delete;
    ValidatedTenancyScope(const ValidatedTenancyScope&) = default;

    // kInitForShell allows parsing a securityToken without multitenancy enabled.
    // This is required in the shell since we do not enable this setting in non-servers.
    enum class InitTag {
        kNormal,
        kInitForShell,
    };

    /**
     * Constructs a ValidatedTenancyScope by parsing a SecurityToken from a BSON object
     * and verifying its cryptographic signature.
     */
    explicit ValidatedTenancyScope(BSONObj securityToken, InitTag tag = InitTag::kNormal);

    /**
     * Constructs a ValidatedTenancyScope for tenant only by validating that the
     * current client is permitted to specify a tenant via the $tenant field.
     */
    ValidatedTenancyScope(Client* client, TenantId tenant);

    /**
     * Parses the client provided command body and securityToken for tenantId,
     * and for securityToken respectively, the authenticatedUser as well.
     *
     * Returns boost::none when multitenancy support is not enabled.
     */
    static boost::optional<ValidatedTenancyScope> create(Client* client,
                                                         BSONObj body,
                                                         BSONObj securityToken);

    bool hasAuthenticatedUser() const;

    const UserName& authenticatedUser() const;

    const TenantId& tenantId() const {
        return stdx::visit(
            OverloadedVisitor{
                [](const UserName& userName) -> decltype(auto) { return *userName.getTenant(); },
                [](const TenantId& tenant) -> decltype(auto) { return tenant; },
            },
            _tenantOrUser);
    }

    BSONObj getOriginalToken() const {
        return _originalToken;
    }

    /**
     * Get/Set a ValidatedTenancyScope as a decoration on the OperationContext
     */
    static const boost::optional<ValidatedTenancyScope>& get(OperationContext* opCtx);
    static void set(OperationContext* opCtx, boost::optional<ValidatedTenancyScope> token);

    /**
     * Transitional token generator, do not use outside of test code.
     */
    struct TokenForTestingTag {};
    explicit ValidatedTenancyScope(BSONObj token, TokenForTestingTag);

    /**
     * Setup a validated tenant for test, do not use outside of test code.
     */
    struct TenantForTestingTag {};
    explicit ValidatedTenancyScope(TenantId tenant, TenantForTestingTag)
        : _tenantOrUser(std::move(tenant)) {}

    /**
     * Backdoor API for use by FLE Query Analysis to setup a validated tenant without a security
     * context.
     */
    struct TrustedFLEQueryAnalysisTag {};
    explicit ValidatedTenancyScope(TenantId tenant, TrustedFLEQueryAnalysisTag)
        : _tenantOrUser(std::move(tenant)) {}

private:
    // Preserve original token for serializing from MongoQ.
    BSONObj _originalToken;

    stdx::variant<UserName, TenantId> _tenantOrUser;
};

}  // namespace auth
}  // namespace mongo
