/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/impersonation_session.h"

#include <boost/optional.hpp>
#include <tuple>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"

namespace mongo {

ImpersonationSessionGuard::ImpersonationSessionGuard(OperationContext* opCtx) : _opCtx(opCtx) {
    auto authSession = AuthorizationSession::get(_opCtx->getClient());
    const auto impersonatedUsersAndRoles = rpc::getImpersonatedUserMetadata(opCtx);
    if (impersonatedUsersAndRoles) {
        uassert(ErrorCodes::Unauthorized,
                "Unauthorized use of impersonation metadata.",
                authSession->isAuthorizedForPrivilege(
                    Privilege(ResourcePattern::forClusterResource(), ActionType::impersonate)));
        fassert(ErrorCodes::InternalError, !authSession->isImpersonating());
        authSession->setImpersonatedUserData(impersonatedUsersAndRoles->getUsers(),
                                             impersonatedUsersAndRoles->getRoles());
        _active = true;
        return;
    }
}

ImpersonationSessionGuard::~ImpersonationSessionGuard() {
    if (_active) {
        AuthorizationSession::get(_opCtx->getClient())->clearImpersonatedUserData();
    }
}

}  // namespace mongo
