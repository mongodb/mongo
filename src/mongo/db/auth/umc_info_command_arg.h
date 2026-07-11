// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/database_name.h"
#include "mongo/util/modules.h"
#include "mongo/util/serialization_context.h"

#include <string>
#include <string_view>
#include <variant>

namespace mongo {
namespace auth {
using namespace std::literals::string_view_literals;

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
class [[MONGO_MOD_PUBLIC]] UMCInfoCommandArg {
public:
    UMCInfoCommandArg() : UMCInfoCommandArg(AllOnCurrentDB{}) {}
    static_assert(std::is_same<UserName, T>::value || std::is_same<RoleName, T>::value,
                  "UMCInfoCommandArg only valid with T = UserName | RoleName");

    using Single = std::variant<T, std::string>;
    using Multiple = std::vector<Single>;

    explicit UMCInfoCommandArg(Single value) : _value(std::move(value)) {}
    explicit UMCInfoCommandArg(Multiple values) : _value(std::move(values)) {}

    static UMCInfoCommandArg parseFromBSON(const boost::optional<TenantId> tenantId,
                                           const BSONElement& elem,
                                           const SerializationContext&) {
        if (elem.isNumber() && (elem.safeNumberLong() == 1)) {
            return UMCInfoCommandArg(AllOnCurrentDB{});
        }
        if (enableForAllDBs && (elem.type() == BSONType::object) &&
            (elem.Obj()[kForAllDBs].trueValue())) {
            return UMCInfoCommandArg(AllForAllDBs{});
        }

        if (elem.type() == BSONType::array) {
            Multiple values;
            for (const auto& v : elem.Obj()) {
                values.push_back(parseNamedElement(v, tenantId));
            }
            return UMCInfoCommandArg(std::move(values));
        }

        return UMCInfoCommandArg(parseNamedElement(elem, tenantId));
    }

    void serializeToBSON(std::string_view fieldName,
                         BSONObjBuilder* bob,
                         const SerializationContext&) const {
        if (holds_alternative<AllOnCurrentDB>(_value)) {
            bob->append(fieldName, 1);
        } else if (holds_alternative<AllForAllDBs>(_value)) {
            bob->append(fieldName, BSON(kForAllDBs << 1));
        } else if (holds_alternative<Single>(_value)) {
            serializeSingle(fieldName, bob, get<Single>(_value));
        } else {
            invariant(holds_alternative<Multiple>(_value));
            const auto& elems = get<Multiple>(_value);
            BSONArrayBuilder setBuilder(bob->subarrayStart(fieldName));
            for (const auto& elem : elems) {
                serializeSingle(&setBuilder, elem);
            }
            setBuilder.doneFast();
        }
    }

    void serializeToBSON(BSONArrayBuilder* bob,
                         const SerializationContext& serializationContext) const {
        // Minimize code duplication by using object serialization path.
        // In practice, we don't use this API, it only exists for IDL completeness.
        BSONObjBuilder tmp;
        serializeToBSON("", &tmp, serializationContext);
        auto elem = tmp.obj();
        bob->append(elem.firstElement());
    }

    /**
     * {usersInfo: 1}
     */
    bool isAllOnCurrentDB() const {
        return holds_alternative<AllOnCurrentDB>(_value);
    }

    /**
     * {usersInfo: {forrAllDBs: 1}}
     */
    bool isAllForAllDBs() const {
        return holds_alternative<AllForAllDBs>(_value);
    }

    /**
     * {usersInfo: 'string' | {db,user|role} | [...] }
     */
    bool isExact() const {
        return holds_alternative<Single>(_value) || holds_alternative<Multiple>(_value);
    }

    /**
     * For isExact() commands, returns a set of T with unspecified DB names resolved with $dbname.
     */
    std::vector<T> getElements(const DatabaseName& dbname) const {
        if (!isExact()) {
            dassert(false);
            uasserted(ErrorCodes::InternalError, "Unable to get exact match for wildcard query");
        }

        if (holds_alternative<Single>(_value)) {
            return {getElement(get<Single>(_value), dbname)};
        } else {
            invariant(holds_alternative<Multiple>(_value));
            const auto& values = get<Multiple>(_value);
            std::vector<T> ret;
            std::transform(values.cbegin(),
                           values.cend(),
                           std::back_inserter(ret),
                           [dbname](const auto& value) { return getElement(value, dbname); });
            return ret;
        }
    }

private:
    static constexpr std::string_view kForAllDBs = "forAllDBs"sv;

    struct AllOnCurrentDB {};
    struct AllForAllDBs {};

    explicit UMCInfoCommandArg(AllOnCurrentDB opt) : _value(std::move(opt)) {}
    explicit UMCInfoCommandArg(AllForAllDBs opt) : _value(std::move(opt)) {}

    static Single parseNamedElement(const BSONElement& elem,
                                    const boost::optional<TenantId> tenantId) {
        if (elem.type() == BSONType::string) {
            return elem.String();
        }
        return T::parseFromBSON(elem, tenantId);
    }

    static void serializeSingle(std::string_view fieldName, BSONObjBuilder* builder, Single elem) {
        if (holds_alternative<T>(elem)) {
            builder->append(fieldName, get<T>(elem).toBSON());
        } else {
            invariant(holds_alternative<std::string>(elem));
            builder->append(fieldName, get<std::string>(elem));
        }
    }

    static void serializeSingle(BSONArrayBuilder* builder, Single elem) {
        if (holds_alternative<T>(elem)) {
            builder->append(get<T>(elem).toBSON());
        } else {
            invariant(holds_alternative<std::string>(elem));
            builder->append(get<std::string>(elem));
        }
    }

    static T getElement(Single elem, const DatabaseName& dbname) {
        if (holds_alternative<T>(elem)) {
            return get<T>(elem);
        } else {
            invariant(holds_alternative<std::string>(elem));
            return T(get<std::string>(elem), dbname);
        }
    }

    // Single is stored as a distinct type from Multiple
    // to ensure that reserialization maintains the same level of nesting.
    std::variant<AllOnCurrentDB, AllForAllDBs, Single, Multiple> _value;
};

using UsersInfoCommandArg = UMCInfoCommandArg<UserName, true>;
using RolesInfoCommandArg = UMCInfoCommandArg<RoleName, false>;

}  // namespace auth
}  // namespace mongo
