/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <iosfwd>
#include <mutex>
#include <string>
#include <utility>

#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"

namespace mongo {

struct OmitTenant {
    explicit OmitTenant() = default;
};
constexpr inline OmitTenant omitTenant;

/**
 * A DatabaseName is a unique name for database.
 * It holds a database name and tenant id, if one exists. In a serverless environment, a tenant id
 * is expected to exist so that a database can be uniquely identified.
 */
class DatabaseName {
public:
    /**
     * Used to create `constexpr` reserved DatabaseName constants. See
     * NamespaceString::ConstantProxy in namespace_string.h for more details.
     */
    class ConstantProxy {
    public:
        class SharedState {
        public:
            explicit constexpr SharedState(StringData db) : _db{db} {}

            const DatabaseName& get() const {
                std::call_once(_once, [this] {
                    _dbName = new DatabaseName{TenantId::systemTenantId(), _db};
                });
                return *_dbName;
            }

        protected:
            StringData _db;
            mutable std::once_flag _once;
            mutable const DatabaseName* _dbName = nullptr;
        };

        constexpr explicit ConstantProxy(const SharedState* sharedState)
            : _sharedState{sharedState} {}

        operator const DatabaseName&() const {
            return _get();
        }

        StringData db(OmitTenant) const {
            return _get().db(omitTenant);
        }
        decltype(auto) tenantId() const {
            return _get().tenantId();
        }
        decltype(auto) toString() const {
            return _get().toString();
        }

    protected:
        const DatabaseName& _get() const {
            return _sharedState->get();
        }

        const SharedState* _sharedState;
    };

#define DBNAME_CONSTANT(id, db) static const ConstantProxy id;
#include "database_name_reserved.def.h"  // IWYU pragma: keep

#undef DBNAME_CONSTANT

    static constexpr size_t kMaxDatabaseNameLength = 63;

    /**
     * Constructs an empty DatabaseName.
     */
    DatabaseName() = default;

    /**
     * Construct a new DatabaseName from a reference. This reference could be a NamespaceString so
     * only use the discriminator, tenant id and database name from its data.
     */
    DatabaseName(const DatabaseName& dbName)
        : _data(dbName.view().substr(0, dbName.sizeWithTenant() + kDataOffset)) {}

    DatabaseName(DatabaseName&& dbName) = default;

    DatabaseName& operator=(DatabaseName&& dbName) = default;

    /**
     * Copy assignment operator. dbName could be a NamespaceString so only use the discriminator,
     * tenant id and database name from its data.
     */
    DatabaseName& operator=(const DatabaseName& dbName) {
        _data = dbName.view().substr(0, dbName.sizeWithTenant() + kDataOffset).toString();
        return *this;
    }

    /**
     * This function constructs a DatabaseName without checking for presence of TenantId.
     *
     * MUST only be used for tests.
     */
    static DatabaseName createDatabaseName_forTest(boost::optional<TenantId> tenantId,
                                                   StringData dbString) {
        return DatabaseName(tenantId, dbString);
    }

    boost::optional<TenantId> tenantId() const {
        if (!hasTenantId()) {
            return boost::none;
        }

        return TenantId{OID::from(&_data[kDataOffset])};
    }

    /**
     * Returns the size of the name of this Database.
     */
    size_t size() const {
        return static_cast<uint8_t>(_data.front()) & kDatabaseNameOffsetEndMask;
    }

    bool isEmpty() const {
        return size() == 0;
    }
    bool isAdminDB() const {
        return db(omitTenant) == DatabaseName::kAdmin.db(omitTenant);
    }
    bool isLocalDB() const {
        return db(omitTenant) == DatabaseName::kLocal.db(omitTenant);
    }
    bool isConfigDB() const {
        return db(omitTenant) == DatabaseName::kConfig.db(omitTenant);
    }
    bool isExternalDB() const {
        return db(omitTenant) == DatabaseName::kExternal.db(omitTenant);
    }
    bool isInternalDb() const {
        return isAdminDB() || isConfigDB() || isLocalDB();
    }

    /**
     * NOTE: DollarInDbNameBehavior::allow is deprecated.
     *
     * Please use DollarInDbNameBehavior::disallow and check explicitly for any DB names that must
     * contain a $.
     */
    enum class DollarInDbNameBehavior {
        Disallow,
        Allow,  // Deprecated
    };


    /**
     * samples:
     *   good
     *      foo
     *      bar
     *      foo-bar
     *   bad:
     *      foo bar
     *      foo.bar
     *      foo"bar
     *
     * @param db - a possible database name
     * @param DollarInDbNameBehavior - please do not change the default value. DB names that must
     *                                 contain a $ should be checked explicitly.
     * @return if db is an allowed database name
     */

    static bool validDBName(StringData dbName,
                            DollarInDbNameBehavior behavior = DollarInDbNameBehavior::Disallow);

    static bool isValid(const DatabaseName& dbName,
                        DollarInDbNameBehavior behavior = DollarInDbNameBehavior::Disallow) {
        return validDBName(dbName.db(omitTenant), behavior);
    }

    static bool isValid(StringData dbName) {
        return validDBName(dbName);
    }

    /**
     * Returns a db name string without tenant id.  Only to be used when a tenant id cannot be
     * tolerated in the serialized output, and should otherwise be avoided whenever possible.
     */
    std::string serializeWithoutTenantPrefix_UNSAFE() const {
        return db(omitTenant).toString();
    }

    /**
     * This function should only be used when creating a resouce id for databasename.
     */
    std::string toStringForResourceId() const {
        return toStringWithTenantId();
    }

    /**
     * This function should only be used when logging a db name in an error message.
     */
    std::string toStringForErrorMsg() const {
        return toStringWithTenantId();
    }

    /**
     * Method to be used only when logging a DatabaseName in a log message.
     * It is called anytime a DatabaseName is logged by logAttrs or otherwise.
     */
    friend std::string toStringForLogging(const DatabaseName& dbName) {
        return dbName.toStringWithTenantId();
    }

    /**
     * This function returns the DatabaseName as a string, including the tenantId.
     *
     * MUST only be used for tests.
     */
    std::string toStringWithTenantId_forTest() const {
        return toStringWithTenantId();
    }

    /**
     * This function returns the DatabaseName as a string, ignoring the tenantId.
     *
     * MUST only be used for tests.
     */
    std::string toString_forTest() const {
        return toString();
    }

    /**
     * Returns true if the db names of `this` and `other` are equal, ignoring case, *and* they both
     * refer to the same tenant ID (or none).
     * The tenant comparison *is* case-sensitive.
     */
    bool equalCaseInsensitive(const DatabaseName& other) const {
        return tenantIdView() == other.tenantIdView() &&
            db(omitTenant).equalCaseInsensitive(other.db(omitTenant));
    }

    int compare(const DatabaseName& other) const {
        if (hasTenantId() && !other.hasTenantId()) {
            return 1;
        }

        if (other.hasTenantId() && !hasTenantId()) {
            return -1;
        }

        return StringData{_data.data() + kDataOffset, sizeWithTenant()}.compare(
            StringData{other._data.data() + kDataOffset, other.sizeWithTenant()});
    }

    friend bool operator==(const DatabaseName& lhs, const DatabaseName& rhs) {
        return lhs.view().substr(kDataOffset, lhs.sizeWithTenant()) ==
            rhs.view().substr(kDataOffset, rhs.sizeWithTenant());
    }

    friend bool operator<(const DatabaseName& lhs, const DatabaseName& rhs) {
        return lhs.compare(rhs) < 0;
    }

    friend bool operator<=(const DatabaseName& lhs, const DatabaseName& rhs) {
        return lhs.compare(rhs) <= 0;
    }

    friend bool operator>(const DatabaseName& lhs, const DatabaseName& rhs) {
        return lhs.compare(rhs) > 0;
    }

    friend bool operator>=(const DatabaseName& lhs, const DatabaseName& rhs) {
        return lhs.compare(rhs) >= 0;
    }

    template <typename H>
    friend H AbslHashValue(H h, const DatabaseName& obj) {
        //  _data might contain a collection : only hash the discriminator, tenant and database.
        return H::combine(
            std::move(h),
            std::string_view{obj.view().substr(0, obj.sizeWithTenant() + kDataOffset)});
    }

    friend auto logAttrs(const DatabaseName& obj) {
        return "db"_attr = obj;
    }

    /**
     * This method returns the database name without the tenant id. It MUST only be used on
     * DatabaseName that can never contain a tenant id (such as global database constants) otherwise
     * data isolation between tenant can break.
     */
    StringData db(OmitTenant) const {
        return view().substr(dbNameOffsetStart(), size());
    }

protected:
    friend class NamespaceString;
    friend class NamespaceStringOrUUID;
    friend class DatabaseNameUtil;
    friend class NamespaceStringUtil;

    template <typename T>
    friend class AuthName;

    /**
     * Returns the size of the optional tenant id.
     */
    size_t tenantIdSize() const {
        return hasTenantId() ? OID::kOIDSize : 0;
    }

    /**
     * Returns the size of database name plus the size of the tenant.
     */
    size_t sizeWithTenant() const {
        return size() + tenantIdSize();
    }

    /**
     * Returns the offset from the start of _data to the start of the database name.
     */
    size_t dbNameOffsetStart() const {
        return kDataOffset + tenantIdSize();
    }

    /**
     * Returns a view of the internal string.
     */
    StringData view() const {
        return _data;
    }

    /**
     * Constructs a DatabaseName from the given tenantId and database name.
     * "dbString" is expected only consist of a db name. It is the caller's responsibility to ensure
     * the dbString is a valid db name.
     */
    DatabaseName(boost::optional<TenantId> tenantId, StringData dbString) {
        uassert(ErrorCodes::InvalidNamespace,
                "'.' is an invalid character in a db name: " + dbString,
                dbString.find('.') == std::string::npos);
        uassert(ErrorCodes::InvalidNamespace,
                "database names cannot have embedded null characters",
                dbString.find('\0') == std::string::npos);
        uassert(ErrorCodes::InvalidNamespace,
                fmt::format("db name must be at most {} characters, found: {}",
                            kMaxDatabaseNameLength,
                            dbString.size()),
                dbString.size() <= kMaxDatabaseNameLength);

        uint8_t details = dbString.size() & kDatabaseNameOffsetEndMask;
        size_t dbStartIndex = kDataOffset;
        if (tenantId) {
            dbStartIndex += OID::kOIDSize;
            details |= kTenantIdMask;
        }

        _data.resize(dbStartIndex + dbString.size());
        *reinterpret_cast<uint8_t*>(_data.data()) = details;
        if (tenantId) {
            std::memcpy(_data.data() + kDataOffset, tenantId->_oid.view().view(), OID::kOIDSize);
        }
        if (!dbString.empty()) {
            std::memcpy(_data.data() + dbStartIndex, dbString.rawData(), dbString.size());
        }
    }

    StringData tenantIdView() const {
        if (!hasTenantId()) {
            return {};
        }

        return view().substr(kDataOffset, OID::kOIDSize);
    }

    std::string toString() const {
        return db(omitTenant).toString();
    }

    std::string toStringWithTenantId() const {
        if (hasTenantId()) {
            auto tenantId = TenantId{OID::from(&_data[kDataOffset])};
            return str::stream() << tenantId.toString() << "_" << db(omitTenant);
        }

        return db(omitTenant).toString();
    }

    static constexpr size_t kDataOffset = sizeof(uint8_t);
    static constexpr uint8_t kTenantIdMask = 0x80;
    static constexpr uint8_t kDatabaseNameOffsetEndMask = 0x7F;

    bool hasTenantId() const {
        return static_cast<uint8_t>(_data.front()) & kTenantIdMask;
    }

    // Private constructor for NamespaceString to construct DatabaseName from its own internal data
    struct TrustedInitTag {};
    DatabaseName(std::string data, TrustedInitTag) : _data(std::move(data)) {}

    /**
     * We pack all possible namespaces data into a string consisting of these concatenated parts:
     *
     * Length      Name            Description
     * ---------------------------------------------------------------------------------------------
     *      1      discriminator   Consists of bit fields:
     *                             [0..6] : dbSize : length of the database part (cannot exceed
     *                                               64 characters). 7 bits.
     *                             [7]    : hasTenant : MSB. 1 if there is a tenant.
     *
     *     12      tenant ID       OID that uniquely identify the tenant for this namespace.
     *                             Optional, only if hasTenant == 1.
     *
     * dbSize      database        Database name, size equal to the dbSize component of the
     *                             discriminator.
     *
     *      1      dot             Dot character between the database and the collection.
     *                             Optional, only if `this` is a NamespaceString and there is
     *                             space left in the string.
     *
     *    ...      collection      Collection name.
     *                             Optional, only if `this` is a NamespaceString and there is space
     *                             left in the string.
     */
    std::string _data{'\0'};
};

inline std::string stringifyForAssert(const DatabaseName& dbName) {
    return toStringForLogging(dbName);
}

inline bool DatabaseName::validDBName(StringData db,
                                      DatabaseName::DollarInDbNameBehavior behavior) {
    if (db.size() == 0 || db.size() > DatabaseName::kMaxDatabaseNameLength)
        return false;

    for (StringData::const_iterator iter = db.begin(), end = db.end(); iter != end; ++iter) {
        switch (*iter) {
            case '\0':
            case '/':
            case '\\':
            case '.':
            case ' ':
            case '"':
                return false;
            case '$':
                if (behavior == DatabaseName::DollarInDbNameBehavior::Disallow)
                    return false;
                continue;
#ifdef _WIN32
            // We prohibit all FAT32-disallowed characters on Windows
            case '*':
            case '<':
            case '>':
            case ':':
            case '|':
            case '?':
                return false;
#endif
            default:
                continue;
        }
    }
    return true;
}


// The `constexpr` definitions for `DatabaseName::ConstantProxy` static data members are below. See
// `constexpr` definitions for the `NamespaceString::ConstantProxy` static data members of NSS in
// namespace_string.h for more details.
namespace dbname_detail::const_proxy_shared_states {
#define DBNAME_CONSTANT(id, db) constexpr inline DatabaseName::ConstantProxy::SharedState id{db};
#include "database_name_reserved.def.h"  // IWYU pragma: keep

#undef DBNAME_CONSTANT
}  // namespace dbname_detail::const_proxy_shared_states

#define DBNAME_CONSTANT(id, db)                                    \
    constexpr inline DatabaseName::ConstantProxy DatabaseName::id{ \
        &dbname_detail::const_proxy_shared_states::id};
#include "database_name_reserved.def.h"  // IWYU pragma: keep

#undef DBNAME_CONSTANT

}  // namespace mongo
