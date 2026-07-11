// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/decorable.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo::auth {
namespace {

const auto validatedTenancyScopeDecoration =
    OperationContext::declareDecoration<boost::optional<ValidatedTenancyScope>>();

}  // namespace

const boost::optional<ValidatedTenancyScope>& ValidatedTenancyScope::get(OperationContext* opCtx) {
    return validatedTenancyScopeDecoration(opCtx);
}

void ValidatedTenancyScope::set(OperationContext* opCtx,
                                boost::optional<ValidatedTenancyScope> token) {
    validatedTenancyScopeDecoration(opCtx) = std::move(token);
}

}  // namespace mongo::auth
