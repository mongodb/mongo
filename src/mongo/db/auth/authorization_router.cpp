// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_router.h"

namespace mongo {

std::string AuthorizationRouter::buildUnknownRolesErrorMsg(
    const stdx::unordered_set<RoleName>& unknownRoles) {
    dassert(unknownRoles.size());

    char delim = ':';
    StringBuilder sb;
    sb << "Could not find role";
    if (unknownRoles.size() > 1) {
        sb << 's';
    }
    for (const auto& unknownRole : unknownRoles) {
        sb << delim << ' ' << unknownRole;
        delim = ',';
    }
    return sb.str();
}

}  // namespace mongo
