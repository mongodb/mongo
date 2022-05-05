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


#include "mongo/db/auth/security_token.h"

#include <boost/optional.hpp>

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_detail.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace auth {
namespace {
const auto securityTokenDecoration = OperationContext::declareDecoration<MaybeSecurityToken>();
MONGO_INITIALIZER(SecurityTokenOptionValidate)(InitializerContext*) {
    uassert(ErrorCodes::BadValue,
            "multitenancySupport may not be specified if featureFlagMongoStore is not enabled",
            !gMultitenancySupport || gFeatureFlagMongoStore.isEnabledAndIgnoreFCV());
    if (gMultitenancySupport) {
        logv2::detail::setGetTenantIDCallback([]() -> boost::optional<TenantId> {
            auto* client = Client::getCurrent();
            if (!client)
                return boost::none;

            if (auto* opCtx = client->getOperationContext()) {
                auto token = getSecurityToken(opCtx);
                if (token) {
                    return token->getAuthenticatedUser().getTenant();
                } else {
                    return boost::none;
                }
            }

            return boost::none;
        });
    }
}
}  // namespace

SecurityTokenAuthenticationGuard::SecurityTokenAuthenticationGuard(OperationContext* opCtx) {
    auto token = getSecurityToken(opCtx);
    if (token == boost::none) {
        _client = nullptr;
        return;
    }

    auto client = opCtx->getClient();
    uassertStatusOK(AuthorizationSession::get(client)->addAndAuthorizeUser(
        opCtx, token->getAuthenticatedUser()));
    _client = client;
}

SecurityTokenAuthenticationGuard::~SecurityTokenAuthenticationGuard() {
    if (_client) {
        // SecurityToken based users are "logged out" at the end of their request.
        AuthorizationSession::get(_client)->logoutSecurityTokenUser(_client);
    }
}

BSONObj signSecurityToken(BSONObj obj) {
    auto authUserElem = obj[SecurityToken::kAuthenticatedUserFieldName];
    uassert(ErrorCodes::BadValue,
            "Invalid field(s) in token being signed",
            (authUserElem.type() == Object) && (obj.nFields() == 1));

    auto authUserObj = authUserElem.Obj();
    ConstDataRange authUserCDR(authUserObj.objdata(), authUserObj.objsize());

    // Placeholder algorithm.
    auto sig = SHA256Block::computeHash({authUserCDR});

    BSONObjBuilder signedToken(obj);
    signedToken.appendBinData(SecurityToken::kSigFieldName, sig.size(), BinDataGeneral, sig.data());
    return signedToken.obj();
}

SecurityToken verifySecurityToken(BSONObj obj) {
    uassert(ErrorCodes::BadValue, "Multitenancy not enabled", gMultitenancySupport);

    auto token = SecurityToken::parse({"Security Token"}, obj);
    auto authenticatedUser = token.getAuthenticatedUser();
    uassert(ErrorCodes::BadValue,
            "Security token authenticated user requires a valid Tenant ID",
            authenticatedUser.getTenant());

    // Use actual authenticatedUser object as passed to preserve hash input.
    auto authUserObj = obj[SecurityToken::kAuthenticatedUserFieldName].Obj();
    ConstDataRange authUserCDR(authUserObj.objdata(), authUserObj.objsize());

    // Placeholder algorithm.
    auto computed = SHA256Block::computeHash({authUserCDR});

    uassert(ErrorCodes::Unauthorized, "Token signature invalid", computed == token.getSig());
    return token;
}

void readSecurityTokenMetadata(OperationContext* opCtx, BSONObj securityToken) try {
    if (securityToken.nFields() == 0) {
        return;
    }

    securityTokenDecoration(opCtx) = verifySecurityToken(securityToken);
    LOGV2_DEBUG(5838100, 4, "Accepted security token", "token"_attr = securityToken);
} catch (const DBException& ex) {
    uassertStatusOK(ex.toStatus().withContext("Unable to parse Security Token from Metadata"));
}

MaybeSecurityToken getSecurityToken(OperationContext* opCtx) {
    return securityTokenDecoration(opCtx);
}

}  // namespace auth
}  // namespace mongo
