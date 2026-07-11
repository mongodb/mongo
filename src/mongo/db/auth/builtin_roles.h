// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/database_name.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace auth {

/**
 * Adds to "privileges" the privileges associated with the named built-in role, and returns
 * true. Returns false if "role" does not name a built-in role, and does not modify
 * "privileges".  Addition of new privileges is done as with
 * Privilege::addPrivilegeToPrivilegeVector.
 */
[[MONGO_MOD_PUBLIC]] bool addPrivilegesForBuiltinRole(const RoleName& role,
                                                      PrivilegeVector* privileges);

/**
 * Ennumerate all builtin RoleNames for the given database.
 */
[[MONGO_MOD_PUBLIC]] stdx::unordered_set<RoleName> getBuiltinRoleNamesForDB(
    const DatabaseName& dbname);

/**
 * Adds to "privileges" the necessary privileges to do absolutely anything on the system.
 */
[[MONGO_MOD_PUBLIC]] void generateUniversalPrivileges(PrivilegeVector* privileges,
                                                      const boost::optional<TenantId>&);

/**
 * Returns whether the given role corresponds to a built-in role.
 */
[[MONGO_MOD_PUBLIC]] bool isBuiltinRole(const RoleName& role);

}  // namespace auth
}  // namespace mongo
