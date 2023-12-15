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

MONGO_INITIALIZER(SecurityTokenOptionValidate)(InitializerContext*) {
    if (gMultitenancySupport) {
        logv2::detail::setGetTenantIDCallback([]() -> std::string {
            auto* client = Client::getCurrent();
            if (!client) {
                return std::string();
            }

            if (auto* opCtx = client->getOperationContext()) {
                if (auto token = ValidatedTenancyScope::get(opCtx)) {
                    return token->tenantId().toString();
                }
            }

            return std::string();
        });
    }

    if (gFeatureFlagSecurityToken.isEnabledUseLatestFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        LOGV2_WARNING(
            7539600,
            "featureFlagSecurityToken is enabled.  This flag MUST NOT be enabled in production");
    }
}

constexpr bool kExpectPrefixDefault = false;
const auto tenantProtocolDecoration =
    Client::declareDecoration<boost::optional<ValidatedTenancyScope::TenantProtocol>>();

// Turns {jwt}/expectPrefix boolean into a TenantProtocol enum value.
// If {client} is non-null, this enum will be set on a decoration on the client.
// Once set, the enum value may NOT be changed by subsequent VTS tokens on the same client.
ValidatedTenancyScope::TenantProtocol parseAndValidateTenantProtocol(Client* client,
                                                                     const crypto::JWT& jwt) {
    const bool expectPrefix = jwt.getExpectPrefix().get_value_or(kExpectPrefixDefault);
    using TenantProtocol = ValidatedTenancyScope::TenantProtocol;
    auto newProtocol = expectPrefix ? TenantProtocol::kAtlasProxy : TenantProtocol::kDefault;
    if (client) {
        auto& currentProtocol = tenantProtocolDecoration(client);
        if (!currentProtocol) {
            // First time seeing a VTS on this Client.
            currentProtocol = newProtocol;
        } else {
            massert(8154400,
                    "Connection protocol can only change once.",
                    currentProtocol == newProtocol);
        }
    }
    return newProtocol;
}

struct ParsedTokenView {
    StringData header;
    StringData body;
    StringData signature;

    StringData payload;
};

// Split "header.body.signature" into {"header", "body", "signature", "header.body"}
ParsedTokenView parseSignedToken(StringData token) {
    ParsedTokenView pt;

    auto split = token.find('.', 0);
    uassert(8039404, "Missing JWS delimiter", split != std::string::npos);
    pt.header = token.substr(0, split);
    auto pos = split + 1;

    split = token.find('.', pos);
    uassert(8039405, "Missing JWS delimiter", split != std::string::npos);
    pt.body = token.substr(pos, split - pos);
    pt.payload = token.substr(0, split);
    pos = split + 1;

    split = token.find('.', pos);
    uassert(8039406, "Too many delimiters in JWS token", split == std::string::npos);
    pt.signature = token.substr(pos);
    return pt;
}

BSONObj decodeJSON(StringData b64) try { return fromjson(base64url::decode(b64)); } catch (...) {
    auto status = exceptionToStatus();
    uasserted(status.code(), "Unable to parse security token: {}"_format(status.reason()));
}

}  // namespace

ValidatedTenancyScope::ValidatedTenancyScope(Client* client, StringData securityToken)
    : _originalToken(securityToken.toString()) {

    uassert(ErrorCodes::InvalidOptions,
            "Multitenancy not enabled, refusing to accept securityToken",
            gMultitenancySupport);

    IDLParserContext ctxt("securityToken");
    auto parsed = parseSignedToken(securityToken);
    // Unsigned tenantId provided via highly privileged connection will respect tenantId field only.
    if (parsed.signature.empty()) {
        auto* as = AuthorizationSession::get(client);
        uassert(ErrorCodes::Unauthorized,
                "Use of unsigned security token requires either useTenant privilege or a system "
                "connection",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(boost::none), ActionType::useTenant) ||
                    client->isFromSystemConnection());
        auto jwt = crypto::JWT::parse(ctxt, decodeJSON(parsed.body));
        uassert(ErrorCodes::Unauthorized,
                "Unsigned security token must contain a tenantId",
                jwt.getTenantId() != boost::none);
        _tenantOrUser = jwt.getTenantId().get();
        _tenantProtocol = parseAndValidateTenantProtocol(client, jwt);
        return;
    }

    // Else, we expect this to be an HS256 token using a preshared secret.
    uassert(ErrorCodes::Unauthorized,
            "Signed authentication tokens are not accepted without feature flag opt-in",
            gFeatureFlagSecurityToken.isEnabledUseLatestFCVWhenUninitialized(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

    uassert(ErrorCodes::OperationFailed,
            "Unable to validate test tokens when testOnlyValidatedTenancyScopeKey is not provided",
            !gTestOnlyValidatedTenancyScopeKey.empty());
    StringData secret(gTestOnlyValidatedTenancyScopeKey);

    auto header = crypto::JWSHeader::parse(ctxt, decodeJSON(parsed.header));
    uassert(ErrorCodes::BadValue,
            "Security token must be signed using 'HS256' algorithm",
            header.getAlgorithm() == "HS256"_sd);

    auto computed =
        SHA256Block::computeHmac(reinterpret_cast<const std::uint8_t*>(secret.rawData()),
                                 secret.size(),
                                 reinterpret_cast<const std::uint8_t*>(parsed.payload.rawData()),
                                 parsed.payload.size());
    auto sigraw = base64url::decode(parsed.signature);
    auto signature = SHA256Block::fromBuffer(reinterpret_cast<const std::uint8_t*>(sigraw.data()),
                                             sigraw.size());

    uassert(ErrorCodes::Unauthorized, "Token signature invalid", computed == signature);

    auto jwt = crypto::JWT::parse(ctxt, decodeJSON(parsed.body));

    // Expected hard-coded values for kid/iss/aud.
    // These signed tokens are used exclusively by internal testing,
    // and should not ever have different values than what we create.
    uassert(ErrorCodes::BadValue,
            "Security token must use kid == '{}'"_format(kTestOnlyKeyId),
            header.getKeyId() == kTestOnlyKeyId);
    uassert(ErrorCodes::BadValue,
            "Security token must use iss == '{}'"_format(kTestOnlyIssuer),
            jwt.getIssuer() == kTestOnlyIssuer);
    uassert(ErrorCodes::BadValue,
            "Security token must use aud == '{}'"_format(kTestOnlyAudience),
            holds_alternative<std::string>(jwt.getAudience()));
    uassert(ErrorCodes::BadValue,
            "Security token must use aud == '{}'"_format(kTestOnlyAudience),
            std::get<std::string>(jwt.getAudience()) == kTestOnlyAudience);

    auto swUserName = UserName::parse(jwt.getSubject(), jwt.getTenantId());
    uassertStatusOK(swUserName.getStatus().withContext("Invalid subject name"));

    _tenantOrUser = std::move(swUserName.getValue());
    _expiration = jwt.getExpiration();
    _tenantProtocol = parseAndValidateTenantProtocol(client, jwt);
}

ValidatedTenancyScope::ValidatedTenancyScope(Client* client, TenantId tenant)
    : _tenantOrUser(std::move(tenant)) {
    uassert(ErrorCodes::InvalidOptions,
            "Multitenancy not enabled, refusing to accept $tenant parameter",
            gMultitenancySupport);

    auto as = AuthorizationSession::get(client);
    // The useTenant action type allows the action of impersonating any tenant, so we check against
    // the cluster resource with the current authenticated user's tenant ID rather than the specific
    // tenant ID being impersonated.
    uassert(
        ErrorCodes::Unauthorized,
        "'$tenant' may only be specified with the useTenant action type",
        client &&
            as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(as->getUserTenantId()), ActionType::useTenant));
}

boost::optional<ValidatedTenancyScope> ValidatedTenancyScope::create(Client* client,
                                                                     BSONObj body,
                                                                     StringData securityToken) {
    if (!gMultitenancySupport) {
        return boost::none;
    }

    auto dollarTenantElem = body["$tenant"_sd];

    uassert(6545800,
            "Cannot pass $tenant id if also passing securityToken",
            dollarTenantElem.eoo() || securityToken.empty());
    uassert(ErrorCodes::OperationFailed,
            "Cannot process $tenant id when no client is available",
            dollarTenantElem.eoo() || client);

    // TODO SERVER-66822: Re-enable this uassert.
    // uassert(ErrorCodes::Unauthorized,
    //         "Multitenancy is enabled, $tenant id or securityToken is required.",
    //         dollarTenantElem || opMsg.securityToken.nFields() > 0);

    if (dollarTenantElem) {
        return ValidatedTenancyScope(client, TenantId::parseFromBSON(dollarTenantElem));
    } else if (!securityToken.empty()) {
        return ValidatedTenancyScope(client, securityToken);
    } else {
        return boost::none;
    }
}

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
