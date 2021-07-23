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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

#include "mongo/db/auth/security_token.h"

#include <boost/optional.hpp>

#include "mongo/base/init.h"
#include "mongo/bson/oid.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_detail.h"

namespace mongo {
namespace auth {
namespace {
const auto securityTokenDecoration = OperationContext::declareDecoration<MaybeSecurityToken>();
MONGO_INITIALIZER(SecurityTokenOptionValidate)(InitializerContext*) {
    uassert(ErrorCodes::BadValue,
            "acceptOpMsgSecurityToken may not be specified if featureFlagMongoStore is not enabled",
            !gAcceptOpMsgSecurityToken || gFeatureFlagMongoStore.isEnabledAndIgnoreFCV());
    if (gAcceptOpMsgSecurityToken) {
        logv2::detail::setGetTenantIDCallback([]() -> boost::optional<OID> {
            auto* client = Client::getCurrent();
            auto* opCtx = client ? client->getOperationContext() : nullptr;
            auto token = getSecurityToken(opCtx);
            if (token) {
                return token->getTenant();
            } else {
                return boost::none;
            }
        });
    }
}
}  // namespace

void readSecurityTokenMetadata(OperationContext* opCtx, BSONObj securityToken) try {
    if (securityToken.nFields() == 0) {
        return;
    }

    uassert(ErrorCodes::BadValue,
            "Server not configured to accept security token",
            gAcceptOpMsgSecurityToken);

    securityTokenDecoration(opCtx) = SecurityToken::parse({"Security Token"}, securityToken);
    LOGV2_DEBUG(5838100, 4, "Accepted security token", "token"_attr = securityToken);

} catch (const DBException& ex) {
    uassertStatusOK(ex.toStatus().withContext("Unable to parse Security Token from Metadata"));
}

MaybeSecurityToken getSecurityToken(OperationContext* opCtx) {
    if (!opCtx) {
        return boost::none;
    }

    return securityTokenDecoration(opCtx);
}

}  // namespace auth
}  // namespace mongo
