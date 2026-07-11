// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    AuditUserAttrsBase::parseProtected(obj);
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
