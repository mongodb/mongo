// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/database_name.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace mongo {

/**
 * Wraps the RoleName type for IDL parsing.
 *
 * User management commands which accept a foreign role name
 * allow specifying the name as either a { role: "", db: "" } document
 * or as a simple string where the db name is implied.
 *
 * This class will parse either form, and allow always returning
 * a fully qualified RoleName.
 */
class RoleNameOrString {
public:
    RoleNameOrString() = delete;
    explicit RoleNameOrString(std::string role) : _roleName(std::move(role)) {}
    explicit RoleNameOrString(std::string_view role) : _roleName(std::string{role}) {}
    explicit RoleNameOrString(RoleName role) : _roleName(std::move(role)) {}

    // IDL support.
    static RoleNameOrString parseFromBSON(const BSONElement& elem);
    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* bob) const;
    void serializeToBSON(BSONArrayBuilder* bob) const;

    /**
     * Returns the fully qualified RoleName if present,
     * or constructs a RoleName using the parsed role and provided dbname.
     */
    RoleName getRoleName(const DatabaseName& dbname) const;

private:
    std::variant<RoleName, std::string> _roleName;
};

}  // namespace mongo
