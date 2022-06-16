/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

#include <string>
#include <variant>

#include "mongo/db/auth/role_name.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"

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
    explicit RoleNameOrString(StringData role) : _roleName(role.toString()) {}
    explicit RoleNameOrString(RoleName role) : _roleName(std::move(role)) {}

    // IDL support.
    static RoleNameOrString parseFromBSON(const BSONElement& elem);
    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const;
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
