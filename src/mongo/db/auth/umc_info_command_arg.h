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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/database_name.h"
#include "mongo/stdx/variant.h"

namespace mongo {
namespace auth {

/**
 * Wraps the usersInfo and rolesInfo command args.
 *
 * These commands accept the following formats:
 *  {....sInfo: 1} // All users on the current DB.
 *  {usersInfo: {forAllDBs: 1}} // All users on all DBs. (usersInfo only)
 *
 *  {....sInfo: 'alice'} // Specific user on current DB.
 *  {....sInfo: {db: 'test', user: 'alice'} // Specific user on specific DB.
 *  {....sInfo: [stringOrDoc]} // Set of users (using above two formats)
 *
 * Use isAllOnCurrentDB(), isAllForAllDBs(), and isExact() to determine format.
 * Then use getElements(dbname) for isExact() form to get list of T names.
 */
template <typename T, bool enableForAllDBs>
class UMCInfoCommandArg {
public:
    UMCInfoCommandArg() : UMCInfoCommandArg(AllOnCurrentDB{}) {}
    static_assert(std::is_same<UserName, T>::value || std::is_same<RoleName, T>::value,
                  "UMCInfoCommandArg only valid with T = UserName | RoleName");

    static UMCInfoCommandArg parseFromBSON(const BSONElement& elem) {
        if (elem.isNumber() && (elem.safeNumberLong() == 1)) {
            return UMCInfoCommandArg(AllOnCurrentDB{});
        }
        if (enableForAllDBs && (elem.type() == Object) && (elem.Obj()[kForAllDBs].trueValue())) {
            return UMCInfoCommandArg(AllForAllDBs{});
        }

        if (elem.type() == Array) {
            Multiple values;
            for (const auto& v : elem.Obj()) {
                values.push_back(parseNamedElement(v));
            }
            return UMCInfoCommandArg(std::move(values));
        }

        return UMCInfoCommandArg(parseNamedElement(elem));
    }

    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const {
        if (stdx::holds_alternative<AllOnCurrentDB>(_value)) {
            bob->append(fieldName, 1);
        } else if (stdx::holds_alternative<AllForAllDBs>(_value)) {
            bob->append(fieldName, BSON(kForAllDBs << 1));
        } else if (stdx::holds_alternative<Single>(_value)) {
            serializeSingle(fieldName, bob, stdx::get<Single>(_value));
        } else {
            invariant(stdx::holds_alternative<Multiple>(_value));
            const auto& elems = stdx::get<Multiple>(_value);
            BSONArrayBuilder setBuilder(bob->subarrayStart(fieldName));
            for (const auto& elem : elems) {
                serializeSingle(&setBuilder, elem);
            }
            setBuilder.doneFast();
        }
    }

    void serializeToBSON(BSONArrayBuilder* bob) const {
        // Minimize code duplication by using object serialization path.
        // In practice, we don't use this API, it only exists for IDL completeness.
        BSONObjBuilder tmp;
        serializeToBSON("", &tmp);
        auto elem = tmp.obj();
        bob->append(elem.firstElement());
    }

    /**
     * {usersInfo: 1}
     */
    bool isAllOnCurrentDB() const {
        return stdx::holds_alternative<AllOnCurrentDB>(_value);
    }

    /**
     * {usersInfo: {forrAllDBs: 1}}
     */
    bool isAllForAllDBs() const {
        return stdx::holds_alternative<AllForAllDBs>(_value);
    }

    /**
     * {usersInfo: 'string' | {db,user|role} | [...] }
     */
    bool isExact() const {
        return stdx::holds_alternative<Single>(_value) || stdx::holds_alternative<Multiple>(_value);
    }

    /**
     * For isExact() commands, returns a set of T with unspecified DB names resolved with $dbname.
     */
    std::vector<T> getElements(const DatabaseName& dbname) const {
        if (!isExact()) {
            dassert(false);
            uasserted(ErrorCodes::InternalError, "Unable to get exact match for wildcard query");
        }

        if (stdx::holds_alternative<Single>(_value)) {
            return {getElement(stdx::get<Single>(_value), dbname)};
        } else {
            invariant(stdx::holds_alternative<Multiple>(_value));
            const auto& values = stdx::get<Multiple>(_value);
            std::vector<T> ret;
            std::transform(values.cbegin(),
                           values.cend(),
                           std::back_inserter(ret),
                           [dbname](const auto& value) { return getElement(value, dbname); });
            return ret;
        }
    }

private:
    static constexpr StringData kForAllDBs = "forAllDBs"_sd;

    struct AllOnCurrentDB {};
    struct AllForAllDBs {};
    using Single = stdx::variant<T, std::string>;
    using Multiple = std::vector<Single>;

    explicit UMCInfoCommandArg(AllOnCurrentDB opt) : _value(std::move(opt)) {}
    explicit UMCInfoCommandArg(AllForAllDBs opt) : _value(std::move(opt)) {}
    explicit UMCInfoCommandArg(Single value) : _value(std::move(value)) {}
    explicit UMCInfoCommandArg(Multiple values) : _value(std::move(values)) {}

    static Single parseNamedElement(const BSONElement& elem) {
        if (elem.type() == String) {
            return elem.String();
        }
        return T::parseFromBSON(elem);
    }

    static void serializeSingle(StringData fieldName, BSONObjBuilder* builder, Single elem) {
        if (stdx::holds_alternative<T>(elem)) {
            builder->append(fieldName, stdx::get<T>(elem).toBSON());
        } else {
            invariant(stdx::holds_alternative<std::string>(elem));
            builder->append(fieldName, stdx::get<std::string>(elem));
        }
    }

    static void serializeSingle(BSONArrayBuilder* builder, Single elem) {
        if (stdx::holds_alternative<T>(elem)) {
            builder->append(stdx::get<T>(elem).toBSON());
        } else {
            invariant(stdx::holds_alternative<std::string>(elem));
            builder->append(stdx::get<std::string>(elem));
        }
    }

    static T getElement(Single elem, const DatabaseName& dbname) {
        if (stdx::holds_alternative<T>(elem)) {
            return stdx::get<T>(elem);
        } else {
            invariant(stdx::holds_alternative<std::string>(elem));
            return T(stdx::get<std::string>(elem), dbname);
        }
    }

    // Single is stored as a distinct type from Multiple
    // to ensure that reserialization maintains the same level of nesting.
    stdx::variant<AllOnCurrentDB, AllForAllDBs, Single, Multiple> _value;
};

using UsersInfoCommandArg = UMCInfoCommandArg<UserName, true>;
using RolesInfoCommandArg = UMCInfoCommandArg<RoleName, false>;

}  // namespace auth
}  // namespace mongo
