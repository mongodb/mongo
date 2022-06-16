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

#include "mongo/db/auth/role_name_or_string.h"

namespace mongo {

/**
 * macOS 10.14 does not support std::get<T>(std::variant<T,...>)
 * because it lacks an implementation of std::bad_variant_access::what()
 *
 * std::get_if<T> doesn't require the exception, so we can get away with using it,
 * provided that we have done due-diligence checking std::holds_alternative<T>
 */
template <typename T, typename U>
const T& variant_get(const U* variant) {
    const T* value = std::get_if<T>(variant);
    dassert(value != nullptr);
    return *value;
}

RoleName RoleNameOrString::getRoleName(const DatabaseName& dbname) const {
    if (std::holds_alternative<RoleName>(_roleName)) {
        auto role = variant_get<RoleName>(&_roleName);
        return RoleName(role.getName(), role.getDB(), dbname.tenantId());
    } else {
        dassert(std::holds_alternative<std::string>(_roleName));
        return RoleName(variant_get<std::string>(&_roleName), dbname);
    }
}

RoleNameOrString RoleNameOrString::parseFromBSON(const BSONElement& elem) {
    if (elem.type() == Object) {
        return RoleNameOrString(RoleName::parseFromBSON(elem));
    } else if (elem.type() == String) {
        return RoleNameOrString(elem.checkAndGetStringData());
    } else {
        uasserted(ErrorCodes::BadValue, "Role name must be either a document or string");
    }
}

void RoleNameOrString::serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const {
    if (std::holds_alternative<RoleName>(_roleName)) {
        variant_get<RoleName>(&_roleName).serializeToBSON(fieldName, bob);
    } else {
        dassert(std::holds_alternative<std::string>(_roleName));
        bob->append(fieldName, variant_get<std::string>(&_roleName));
    }
}

void RoleNameOrString::serializeToBSON(BSONArrayBuilder* bob) const {
    if (std::holds_alternative<RoleName>(_roleName)) {
        variant_get<RoleName>(&_roleName).serializeToBSON(bob);
    } else {
        dassert(std::holds_alternative<std::string>(_roleName));
        bob->append(variant_get<std::string>(&_roleName));
    }
}

}  // namespace mongo
