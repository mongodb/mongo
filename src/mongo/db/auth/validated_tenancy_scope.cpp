// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/validated_tenancy_scope.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/auth/validated_tenancy_scope_gen.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo::auth {

const ValidatedTenancyScope ValidatedTenancyScope::kNotRequired{};

bool ValidatedTenancyScope::hasAuthenticatedUser() const {
    return holds_alternative<UserName>(_tenantOrUser);
}

const UserName& ValidatedTenancyScope::authenticatedUser() const {
    invariant(hasAuthenticatedUser());
    return std::get<UserName>(_tenantOrUser);
}

}  // namespace mongo::auth
