// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
