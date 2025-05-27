/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/rpc/metadata/audit_user_attrs.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/synchronized_value.h"

#include <cmath>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace rpc {
namespace {
const auto auditUserAttrsDecoration =
    OperationContext::declareDecoration<synchronized_value<boost::optional<AuditUserAttrs>>>();
}  // namespace

boost::optional<AuditUserAttrs> AuditUserAttrs::get(OperationContext* opCtx) {
    if (!opCtx) {
        return boost::none;
    }
    return auditUserAttrsDecoration(opCtx).synchronize();
}

void AuditUserAttrs::set(OperationContext* opCtx, AuditUserAttrs attrs) {
    tassert(9791300, "Must have opCtx in AuditUserAttrs::set", opCtx);
    auditUserAttrsDecoration(opCtx) = std::move(attrs);
}

AuditUserAttrs::AuditUserAttrs(const BSONObj& obj) {
    AuditUserAttrsBase::parseProtected(IDLParserContext("AuditUserAttrsBase"), obj);
}

void AuditUserAttrs::resetToAuthenticatedUser(OperationContext* opCtx) {
    tassert(9791301, "Must have opCtx in AuditUserAttrs::resetToAuthenticatedUser", opCtx);

    auto client = opCtx->getClient();
    if (AuthorizationSession::exists(client)) {
        auto authzSession = AuthorizationSession::get(client);
        if (auto userName = authzSession->getAuthenticatedUserName()) {
            AuditUserAttrs::set(opCtx,
                                *userName,
                                roleNameIteratorToContainer<std::vector<RoleName>>(
                                    authzSession->getAuthenticatedRoleNames()),
                                false /* isImpersonating */);
            return;
        }
    }
    // If there's no currently authenticated user, reset to empty.
    auditUserAttrsDecoration(opCtx) = boost::none;
}
}  // namespace rpc
}  // namespace mongo
