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

RoleName RoleNameOrString::getRoleName(StringData dbname) const {
    if (std::holds_alternative<RoleName>(_roleName)) {
        return std::get<RoleName>(_roleName);
    } else {
        dassert(std::holds_alternative<std::string>(_roleName));
        return RoleName(std::get<std::string>(_roleName), dbname);
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
        std::get<RoleName>(_roleName).serializeToBSON(fieldName, bob);
    } else {
        dassert(std::holds_alternative<std::string>(_roleName));
        bob->append(fieldName, std::get<std::string>(_roleName));
    }
}

void RoleNameOrString::serializeToBSON(BSONArrayBuilder* bob) const {
    if (std::holds_alternative<RoleName>(_roleName)) {
        std::get<RoleName>(_roleName).serializeToBSON(bob);
    } else {
        dassert(std::holds_alternative<std::string>(_roleName));
        bob->append(std::get<std::string>(_roleName));
    }
}

}  // namespace mongo
