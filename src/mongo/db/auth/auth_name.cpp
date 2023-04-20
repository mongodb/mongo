/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/auth/auth_name.h"

#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"

namespace mongo {
namespace {
constexpr auto kTenantFieldName = "tenant"_sd;
}  // namespace

template <typename T>
StatusWith<T> AuthName<T>::parse(StringData str, const boost::optional<TenantId>& tenant) {
    auto split = str.find('.');

    if (split == std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << T::kName << " must contain a '.' separated database."
                                    << T::kFieldName << " pair");
    }

    return T(str.substr(split + 1), str.substr(0, split), tenant);
}

template <typename T>
T AuthName<T>::parseFromVariant(const stdx::variant<std::string, BSONObj>& name,
                                const boost::optional<TenantId>& tenant) {
    if (stdx::holds_alternative<std::string>(name)) {
        return uassertStatusOK(parse(stdx::get<std::string>(name)));
    }

    return parseFromBSONObj(stdx::get<BSONObj>(name), tenant);
}

template <typename T>
T AuthName<T>::parseFromBSONObj(const BSONObj& obj, const boost::optional<TenantId>& activeTenant) {
    std::bitset<3> usedFields;
    constexpr size_t kNameFieldBit = 0;
    constexpr size_t kDbFieldBit = 1;
    constexpr size_t kTenantFieldBit = 2;
    StringData name, db;
    boost::optional<TenantId> tenant = activeTenant;

    const auto validateField = [&](const BSONElement& elem, const size_t bit, BSONType expType) {
        const auto fieldName = elem.fieldNameStringData();
        uassert(ErrorCodes::BadValue,
                str::stream() << T::kName << " must contain a " << typeName(expType)
                              << " field named: " << fieldName << ". But, has type "
                              << typeName(elem.type()),
                elem.type() == expType);
        uassert(ErrorCodes::BadValue,
                str::stream() << T::kName << " has more than one field named: " << fieldName,
                !usedFields[bit]);
        usedFields.set(bit);
    };

    for (const auto& element : obj) {
        const auto fieldName = element.fieldNameStringData();

        if (fieldName == T::kFieldName) {
            validateField(element, kNameFieldBit, String);
            name = element.valueStringData();

        } else if (fieldName == "db"_sd) {
            validateField(element, kDbFieldBit, String);
            db = element.valueStringData();

        } else if (fieldName == kTenantFieldName) {
            validateField(element, kTenantFieldBit, jstOID);
            tenant = TenantId::parseFromBSON(element);
            if (activeTenant) {
                uassert(ErrorCodes::BadValue,
                        str::stream()
                            << T::kName
                            << " contains a TenantId which does not match the active tenant",
                        tenant == activeTenant);
            }

        } else if constexpr (std::is_same_v<UserName, T>) {
            // Only UserName is strict, RoleName is non-strict.
            uasserted(ErrorCodes::BadValue,
                      str::stream()
                          << T::kName << " contains an unknown field named: '" << fieldName);
        }
    }

    uassert(ErrorCodes::BadValue,
            str::stream() << T::kName << " must contain a field named: " << T::kFieldName,
            usedFields[kNameFieldBit]);

    uassert(ErrorCodes::BadValue,
            str::stream() << T::kName << " must contain a field named: db",
            usedFields[kDbFieldBit]);

    return T(name, db, tenant);
}

template <typename T>
T AuthName<T>::parseFromBSON(const BSONElement& elem,
                             const boost::optional<TenantId>& activeTenant) {
    if (elem.type() == String) {
        return uassertStatusOK(parse(elem.valueStringData(), activeTenant));
    } else if (elem.type() == Object) {
        const auto obj = elem.embeddedObject();
        return parseFromBSONObj(obj, activeTenant);
    } else {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << T::kName << " must be either a string or an object");
    }
}

template <typename T>
void AuthName<T>::serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const {
    BSONObjBuilder builder(bob->subobjStart(fieldName));
    appendToBSON(&builder);
}

template <typename T>
void AuthName<T>::serializeToBSON(BSONArrayBuilder* bob) const {
    BSONObjBuilder builder(bob->subobjStart());
    appendToBSON(&builder);
}

template <typename T>
void AuthName<T>::appendToBSON(BSONObjBuilder* bob, bool encodeTenant) const {
    *bob << T::kFieldName << getName() << "db"_sd << getDB();
    if (encodeTenant && _tenant) {
        *bob << kTenantFieldName << _tenant.value();
    }
}

template <typename T>
BSONObj AuthName<T>::toBSON(bool encodeTenant) const {
    BSONObjBuilder bob;
    appendToBSON(&bob, encodeTenant);
    return bob.obj();
}

template <typename T>
std::size_t AuthName<T>::getBSONObjSize() const {
    return 4UL +                            // BSONObj size
        1UL + T::kFieldName.size() + 1UL +  // FieldName elem type, FieldName, terminating NULL.
        4UL + getName().size() + 1UL +      // Length of name data, name data, terminating NULL.
        1UL + ("db"_sd).size() + 1UL +      // DB field elem type, "db", terminating NULL.
        4UL + getDB().size() + 1UL +        // DB value length, DB value, terminating NULL.
        1UL;                                // EOD marker.
}

// Materialize the types we care about.
template class AuthName<RoleName>;
template class AuthName<UserName>;

}  // namespace mongo
