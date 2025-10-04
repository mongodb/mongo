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


#include "mongo/db/auth/security_token_authentication_guard.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo {
namespace auth {

SecurityTokenAuthenticationGuard::SecurityTokenAuthenticationGuard(
    OperationContext* opCtx, const ValidatedTenancyScope& token) {
    if (token.hasAuthenticatedUser()) {
        std::unique_ptr<UserRequest> request =
            std::make_unique<UserRequestGeneral>(token.authenticatedUser(), boost::none);
        auto* client = opCtx->getClient();
        uassertStatusOK(AuthorizationSession::get(client)->addAndAuthorizeUser(
            opCtx, std::move(request), boost::none));
        _client = client;

        LOGV2_DEBUG(5838100,
                    4,
                    "Authenticated with security token",
                    "token"_attr = token.getOriginalToken());
    } else {
        _client = nullptr;
    }
}

SecurityTokenAuthenticationGuard::~SecurityTokenAuthenticationGuard() {
    if (_client) {
        // SecurityToken based users are "logged out" at the end of their request.
        AuthorizationSession::get(_client)->logoutSecurityTokenUser();
    }
}

}  // namespace auth
}  // namespace mongo
