// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_format.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

namespace mongo {
namespace auth {

/**
 * Parses the privileges described in "privileges" into a vector of Privilege objects.
 * Returns Status::OK() upon successfully parsing all the elements of "privileges".
 */
Status parseAndValidatePrivilegeArray(const BSONArray& privileges,
                                      PrivilegeVector* parsedPrivileges);

/**
 * Takes a BSONArray of name,db pair documents, parses that array and returns (via the
 * output param parsedRoleNames) a list of the role names in the input array.
 * Performs syntactic validation of "rolesArray", only.
 */
Status parseRoleNamesFromBSONArray(const BSONArray& rolesArray,
                                   std::string_view dbname,
                                   std::vector<RoleName>* parsedRoleNames);

/**
 * Takes a BSONArray of name,db pair documents, parses that array and returns (via the
 * output param parsedUserNames) a list of the usernames in the input array.
 * Performs syntactic validation of "usersArray", only.
 */
Status parseUserNamesFromBSONArray(const BSONArray& usersArray,
                                   std::string_view dbname,
                                   std::vector<UserName>* parsedUserNames);

}  // namespace auth
}  // namespace mongo
