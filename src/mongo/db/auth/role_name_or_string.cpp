// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/role_name_or_string.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

/**
 * macOS 10.14 does not support get<T>(std::variant<T,...>)
 * because it lacks an implementation of std::bad_variant_access::what()
 *
 * get_if<T> doesn't require the exception, so we can get away with using it,
 * provided that we have done due-diligence checking holds_alternative<T>
 */
template <typename T, typename U>
const T& variant_get(const U* variant) {
    const T* value = get_if<T>(variant);
    dassert(value != nullptr);
    return *value;
}

RoleName RoleNameOrString::getRoleName(const DatabaseName& dbname) const {
    if (holds_alternative<RoleName>(_roleName)) {
        auto role = variant_get<RoleName>(&_roleName);
        return RoleName(role.getName(), role.getDB(), dbname.tenantId());
    } else {
        dassert(holds_alternative<std::string>(_roleName));
        return RoleName(variant_get<std::string>(&_roleName), dbname);
    }
}

RoleNameOrString RoleNameOrString::parseFromBSON(const BSONElement& elem) {
    if (elem.type() == BSONType::object) {
        return RoleNameOrString(RoleName::parseFromBSON(elem));
    } else if (elem.type() == BSONType::string) {
        return RoleNameOrString(elem.checkAndGetStringData());
    } else {
        uasserted(ErrorCodes::BadValue, "Role name must be either a document or string");
    }
}

void RoleNameOrString::serializeToBSON(std::string_view fieldName, BSONObjBuilder* bob) const {
    if (holds_alternative<RoleName>(_roleName)) {
        variant_get<RoleName>(&_roleName).serializeToBSON(fieldName, bob);
    } else {
        dassert(holds_alternative<std::string>(_roleName));
        bob->append(fieldName, variant_get<std::string>(&_roleName));
    }
}

void RoleNameOrString::serializeToBSON(BSONArrayBuilder* bob) const {
    if (holds_alternative<RoleName>(_roleName)) {
        variant_get<RoleName>(&_roleName).serializeToBSON(bob);
    } else {
        dassert(holds_alternative<std::string>(_roleName));
        bob->append(variant_get<std::string>(&_roleName));
    }
}

}  // namespace mongo
