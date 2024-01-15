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

#include "mongo/db/auth/validated_tenancy_scope.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <string>

#include <boost/optional/optional.hpp>

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/jwt_types_gen.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/validated_tenancy_scope_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo::auth {
namespace {
using namespace fmt::literals;

const auto validatedTenancyScopeDecoration =
    OperationContext::declareDecoration<boost::optional<ValidatedTenancyScope>>();

// Signed auth tokens are for internal testing only, and require the use of a preshared key.
// These tokens will have fixed values for kid/iss/aud fields.
// This usage will be replaced by full OIDC processing at a later time.
constexpr auto kTestOnlyKeyId = "test-only-kid"_sd;
constexpr auto kTestOnlyIssuer = "mongodb://test.kernel.localhost"_sd;
constexpr auto kTestOnlyAudience = "mongod-testing"_sd;

}  // namespace

bool ValidatedTenancyScope::hasAuthenticatedUser() const {
    return holds_alternative<UserName>(_tenantOrUser);
}

const UserName& ValidatedTenancyScope::authenticatedUser() const {
    invariant(hasAuthenticatedUser());
    return std::get<UserName>(_tenantOrUser);
}

const boost::optional<ValidatedTenancyScope>& ValidatedTenancyScope::get(OperationContext* opCtx) {
    return validatedTenancyScopeDecoration(opCtx);
}

void ValidatedTenancyScope::set(OperationContext* opCtx,
                                boost::optional<ValidatedTenancyScope> token) {
    validatedTenancyScopeDecoration(opCtx) = std::move(token);
}

ValidatedTenancyScope::ValidatedTenancyScope(const UserName& username,
                                             StringData secret,
                                             TenantProtocol protocol,
                                             TokenForTestingTag tag)
    : ValidatedTenancyScope(username, secret, Date_t::now() + kDefaultExpiration, protocol, tag) {}

ValidatedTenancyScope::ValidatedTenancyScope(const UserName& username,
                                             StringData secret,
                                             Date_t expiration,
                                             TenantProtocol protocol,
                                             TokenForTestingTag) {
    invariant(!secret.empty());

    crypto::JWSHeader header;
    header.setType("JWT"_sd);
    header.setAlgorithm("HS256"_sd);
    header.setKeyId(kTestOnlyKeyId);

    crypto::JWT body;
    body.setIssuer(kTestOnlyIssuer);
    body.setSubject(username.getUnambiguousName());
    body.setAudience(kTestOnlyAudience.toString());
    body.setTenantId(username.getTenant());
    body.setExpiration(std::move(expiration));
    body.setExpectPrefix(protocol == TenantProtocol::kAtlasProxy);

    std::string payload = "{}.{}"_format(base64url::encode(tojson(header.toBSON())),
                                         base64url::encode(tojson(body.toBSON())));

    auto computed =
        SHA256Block::computeHmac(reinterpret_cast<const std::uint8_t*>(secret.rawData()),
                                 secret.size(),
                                 reinterpret_cast<const std::uint8_t*>(payload.data()),
                                 payload.size());

    _originalToken =
        "{}.{}"_format(payload,
                       base64url::encode(StringData(reinterpret_cast<const char*>(computed.data()),
                                                    computed.size())));

    _tenantProtocol = protocol;

    if (gTestOnlyValidatedTenancyScopeKey == secret) {
        _tenantOrUser = username;
        _expiration = body.getExpiration();
    }
}

ValidatedTenancyScope::ValidatedTenancyScope(TenantId tenant,
                                             TenantProtocol protocol,
                                             TenantForTestingTag) {
    crypto::JWSHeader header;
    header.setType("JWT"_sd);
    header.setAlgorithm("none"_sd);
    header.setKeyId("none"_sd);

    crypto::JWT body;
    body.setIssuer("mongodb://testing.localhost"_sd);
    body.setSubject(".");
    body.setAudience(std::string{"mongod-testing"});
    body.setTenantId(tenant);
    body.setExpiration(Date_t::max());
    body.setExpectPrefix(protocol == TenantProtocol::kAtlasProxy);

    _originalToken = "{}.{}."_format(base64url::encode(tojson(header.toBSON())),
                                     base64url::encode(tojson(body.toBSON())));
    _tenantOrUser = std::move(tenant);
    _tenantProtocol = protocol;
}

}  // namespace mongo::auth
