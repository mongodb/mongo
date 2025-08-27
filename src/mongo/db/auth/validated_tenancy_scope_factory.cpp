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

#include "mongo/db/auth/validated_tenancy_scope_factory.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/bson/bsonelement.h"
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
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/net/socket_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo::auth {

namespace {

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
    auto newProtocol = expectPrefix ? ValidatedTenancyScope::TenantProtocol::kAtlasProxy
                                    : ValidatedTenancyScope::TenantProtocol::kDefault;
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
    uasserted(status.code(), fmt::format("Unable to parse security token: {}", status.reason()));
}

}  // namespace

ValidatedTenancyScope ValidatedTenancyScopeFactory::parseUnsignedToken(Client* client,
                                                                       StringData securityToken) {
    IDLParserContext ctxt("securityToken");
    const auto parsed = parseSignedToken(securityToken);

    auto header = crypto::JWSHeader::parse(decodeJSON(parsed.header), ctxt);
    uassert(
        ErrorCodes::InvalidJWT,
        fmt::format("Unexpected algorithm '{}' for unsigned security token", header.getAlgorithm()),
        header.getAlgorithm() == "none");

    uassert(ErrorCodes::InvalidJWT,
            "Unexpected signature on unsigned security token",
            parsed.signature.empty());

    // This function is used in the shell for testing which lacks AuthorizationSession
    if (AuthorizationSession::exists(client)) {
        auto* as = AuthorizationSession::get(client);
        uassert(ErrorCodes::Unauthorized,
                "Use of unsigned security token requires either useTenant privilege or a system "
                "connection",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(boost::none), ActionType::useTenant) ||
                    client->isFromSystemConnection());
    }

    auto jwt = crypto::JWT::parse(decodeJSON(parsed.body), ctxt);
    uassert(ErrorCodes::Unauthorized,
            "Unsigned security token must contain a tenantId",
            jwt.getTenantId() != boost::none);

    auto tenantId = jwt.getTenantId().get();
    auto tenantProtocol = parseAndValidateTenantProtocol(client, jwt);
    return ValidatedTenancyScope(
        client, securityToken, std::move(tenantId), Date_t(), std::move(tenantProtocol));
}

ValidatedTenancyScope ValidatedTenancyScopeFactory::parseToken(Client* client,
                                                               StringData securityToken) {
    IDLParserContext ctxt("securityToken");
    const auto parsed = parseSignedToken(securityToken);

    uassert(ErrorCodes::Unauthorized,
            "Signed authentication tokens are not accepted without feature flag opt-in",
            gFeatureFlagSecurityToken.isEnabledUseLatestFCVWhenUninitialized(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

    uassert(ErrorCodes::OperationFailed,
            "Unable to validate test tokens when testOnlyValidatedTenancyScopeKey is not "
            "provided",
            !gTestOnlyValidatedTenancyScopeKey.empty());

    StringData secret(gTestOnlyValidatedTenancyScopeKey);
    auto header = crypto::JWSHeader::parse(decodeJSON(parsed.header), ctxt);
    uassert(ErrorCodes::BadValue,
            "Security token must be signed using 'HS256' algorithm",
            header.getAlgorithm() == "HS256"_sd);

    auto computed =
        SHA256Block::computeHmac(reinterpret_cast<const std::uint8_t*>(secret.data()),
                                 secret.size(),
                                 reinterpret_cast<const std::uint8_t*>(parsed.payload.data()),
                                 parsed.payload.size());
    auto sigraw = base64url::decode(parsed.signature);
    auto signature = SHA256Block::fromBuffer(reinterpret_cast<const std::uint8_t*>(sigraw.data()),
                                             sigraw.size());
    uassert(ErrorCodes::Unauthorized, "Token signature invalid", computed == signature);

    auto jwt = crypto::JWT::parse(decodeJSON(parsed.body), ctxt);

    // Expected hard-coded values for kid/iss/aud.
    // These signed tokens are used exclusively by internal testing,
    // and should not ever have different values than what we create.
    uassert(ErrorCodes::BadValue,
            fmt::format("Security token must use kid == '{}'", kTestOnlyKeyId),
            header.getKeyId() == kTestOnlyKeyId);
    uassert(ErrorCodes::BadValue,
            fmt::format("Security token must use iss == '{}'", kTestOnlyIssuer),
            jwt.getIssuer() == kTestOnlyIssuer);
    uassert(ErrorCodes::BadValue,
            fmt::format("Security token must use aud == '{}'", kTestOnlyAudience),
            holds_alternative<std::string>(jwt.getAudience()));
    uassert(ErrorCodes::BadValue,
            fmt::format("Security token must use aud == '{}'", kTestOnlyAudience),
            std::get<std::string>(jwt.getAudience()) == kTestOnlyAudience);

    auto swUserName = UserName::parse(jwt.getSubject(), jwt.getTenantId());
    uassertStatusOK(swUserName.getStatus().withContext("Invalid subject name"));

    auto user = std::move(swUserName.getValue());
    auto tenantProtocol = parseAndValidateTenantProtocol(client, jwt);
    return ValidatedTenancyScope(
        client, securityToken, std::move(user), jwt.getExpiration(), std::move(tenantProtocol));
}

boost::optional<ValidatedTenancyScope> ValidatedTenancyScopeFactory::parse(
    Client* client, StringData securityToken) {

    if (!gMultitenancySupport) {
        return boost::none;
    }

    // TODO SERVER-66822: Re-enable this uassert.
    // uassert(ErrorCodes::Unauthorized,
    //         "Multitenancy is enabled, securityToken is required.",
    //         opMsg.securityToken.nFields() > 0);

    if (!securityToken.empty()) {
        // Unsigned tenantId provided via highly privileged connection will respect tenantId field
        // only.
        if (securityToken.ends_with('.')) {
            return ValidatedTenancyScopeFactory::parseUnsignedToken(client, securityToken);
        } else {
            // Else, we expect this to be an HS256 token using a preshared secret.
            return ValidatedTenancyScopeFactory::parseToken(client, securityToken);
        }
    } else {
        return boost::none;
    }
}

ValidatedTenancyScope ValidatedTenancyScopeFactory::create(
    const UserName& userName,
    StringData secret,
    ValidatedTenancyScope::TenantProtocol protocol,
    TokenForTestingTag) {

    invariant(!secret.empty());
    Date_t expiration = Date_t::now() + kDefaultExpiration;

    crypto::JWSHeader header;
    header.setType("JWT"_sd);
    header.setAlgorithm("HS256"_sd);
    header.setKeyId(kTestOnlyKeyId);

    crypto::JWT body;
    body.setIssuer(kTestOnlyIssuer);
    body.setSubject(userName.getUnambiguousName());
    body.setAudience(std::string{kTestOnlyAudience});
    body.setTenantId(userName.tenantId());
    body.setExpiration(std::move(expiration));
    body.setExpectPrefix(protocol == ValidatedTenancyScope::TenantProtocol::kAtlasProxy);

    std::string payload = fmt::format("{}.{}",
                                      base64url::encode(tojson(header.toBSON())),
                                      base64url::encode(tojson(body.toBSON())));

    auto computed = SHA256Block::computeHmac(reinterpret_cast<const std::uint8_t*>(secret.data()),
                                             secret.size(),
                                             reinterpret_cast<const std::uint8_t*>(payload.data()),
                                             payload.size());

    const std::string originalToken =
        fmt::format("{}.{}",
                    payload,
                    base64url::encode(StringData(reinterpret_cast<const char*>(computed.data()),
                                                 computed.size())));

    if (gTestOnlyValidatedTenancyScopeKey == secret) {
        return ValidatedTenancyScope(userName, originalToken, body.getExpiration(), protocol);
    }
    return ValidatedTenancyScope(originalToken, protocol);
}


ValidatedTenancyScope ValidatedTenancyScopeFactory::create(
    TenantId tenant, ValidatedTenancyScope::TenantProtocol protocol, TenantForTestingTag) {
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
    body.setExpectPrefix(protocol == ValidatedTenancyScope::TenantProtocol::kAtlasProxy);

    const std::string originalToken = fmt::format("{}.{}.",
                                                  base64url::encode(tojson(header.toBSON())),
                                                  base64url::encode(tojson(body.toBSON())));
    return ValidatedTenancyScope(originalToken, std::move(tenant), protocol);
}

ValidatedTenancyScope ValidatedTenancyScopeFactory::create(std::string token, InitForShellTag) {
    return ValidatedTenancyScope(std::move(token));
}

ValidatedTenancyScope ValidatedTenancyScopeFactory::create(TenantId tenant,
                                                           TrustedForInnerOpMsgRequestTag) {
    crypto::JWSHeader header;
    header.setType("JWT"_sd);
    header.setAlgorithm("none"_sd);
    header.setKeyId("none"_sd);

    crypto::JWT body;
    body.setIssuer(fmt::format("mongodb://{}", prettyHostNameAndPort(serverGlobalParams.port)));
    body.setSubject(".");
    body.setAudience(std::string{"interal-request"});
    body.setTenantId(tenant);
    body.setExpiration(Date_t::max());
    body.setExpectPrefix(false);  // Always use default protocol, not expect prefix.

    const std::string originalToken = fmt::format("{}.{}.",
                                                  base64url::encode(tojson(header.toBSON())),
                                                  base64url::encode(tojson(body.toBSON())));
    return ValidatedTenancyScope(
        originalToken, std::move(tenant), ValidatedTenancyScope::TenantProtocol::kDefault);
}

ValidatedTenancyScopeGuard::ValidatedTenancyScopeGuard(OperationContext* opCtx) : _opCtx(opCtx) {
    _validatedTenancyScope = ValidatedTenancyScope::get(opCtx);
    ValidatedTenancyScope::set(opCtx, boost::none);

    _tenantProtocol = tenantProtocolDecoration(_opCtx->getClient());
    if (_tenantProtocol) {
        tenantProtocolDecoration(_opCtx->getClient()) =
            auth::ValidatedTenancyScope::TenantProtocol::kDefault;
    }
}

ValidatedTenancyScopeGuard::~ValidatedTenancyScopeGuard() {
    ValidatedTenancyScope::set(_opCtx, _validatedTenancyScope);
    tenantProtocolDecoration(_opCtx->getClient()) = _tenantProtocol;
};

void ValidatedTenancyScopeGuard::runAsTenant(OperationContext* opCtx,
                                             const boost::optional<TenantId>& tenantId,
                                             std::function<void()> workFunc) {
    auth::ValidatedTenancyScopeGuard tenancyStasher(opCtx);
    const auto vts = tenantId
        ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
              *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
        : boost::none;
    auth::ValidatedTenancyScope::set(opCtx, vts);

    workFunc();
}


}  // namespace mongo::auth
