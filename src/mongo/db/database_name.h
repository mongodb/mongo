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

namespace database_name {
constexpr unsigned char kSmallStringFlag = 2;
constexpr unsigned char kStaticAllocFlag = 1;
}  // namespace database_name

/**
 * A DatabaseName is a unique name for database.
 * It holds a database name and tenant id, if one exists. In a serverless environment, a tenant id
 * is expected to exist so that a database can be uniquely identified.
 */
class DatabaseName {
protected:
    class Storage;

public:
#define DBNAME_CONSTANT(id, db) static const DatabaseName id;
#include "database_name_reserved.def.h"  // IWYU pragma: keep
#undef DBNAME_CONSTANT

    static constexpr size_t kMaxDatabaseNameLength = 63;

    /**
     * Constructs an empty DatabaseName.
     */
    DatabaseName() = default;

    constexpr DatabaseName(const char* data, size_t length) : _data(data, length) {}

    /**
     * Construct a new DatabaseName from a reference. This reference could be a NamespaceString so
     * only use the discriminator, tenant id and database name from its data.
     */
    DatabaseName(const DatabaseName& dbName)
        : _data(dbName._data, dbName.sizeWithTenant() + kDataOffset) {}

    DatabaseName(DatabaseName&& dbName) = default;

    DatabaseName& operator=(DatabaseName&& dbName) = default;

    /**
     * Copy assignment operator. dbName could be a NamespaceString so only use the discriminator,
     * tenant id and database name from its data.
     */
    DatabaseName& operator=(const DatabaseName& dbName) {
        if (dbName._data.isDynamicAlloc()) {
            _data = Storage(dbName._data, dbName.sizeWithTenant() + kDataOffset);
        } else if (dbName._data.isSmallString()) {
            _data = dbName._data;
            _data.updateFooter(database_name::kSmallStringFlag,
                               dbName.sizeWithTenant() + kDataOffset);
        } else {
            _data = dbName._data;
        }
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

        return TenantId{OID::from(_data.data() + kDataOffset)};
    }

    /**
     * Returns the size of the name of this Database.
     */
    constexpr size_t size() const {
        return static_cast<uint8_t>(*_data.data()) & kDatabaseNameOffsetEndMask;
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
    constexpr StringData db(OmitTenant) const {
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
    constexpr size_t dbNameOffsetStart() const {
        if (std::is_constant_evaluated()) {
            return kDataOffset;
        }
        return kDataOffset + tenantIdSize();
    }

    /**
     * Returns a view of the internal string.
     */
    constexpr StringData view() const {
        return StringData{_data.data(), _data.size()};
    }

    /**
     * Constructs a DatabaseName from the given tenantId and database name.
     * "dbString" is expected only consist of a db name. It is the caller's responsibility to ensure
     * the dbString is a valid db name.
     */
    DatabaseName(boost::optional<TenantId> tenantId, StringData dbString)
        : _data(Storage::make(std::move(tenantId), dbString)) {
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
    }

    /**
     * Invoked by the NamespaceString child class. It is necessary to reduce copies
     * and to ensure DatabaseName constructors do not remove the collection of the namespace string.
     */
    struct TrustedInitTag {};
    DatabaseName(Storage&& data, TrustedInitTag) noexcept : _data(std::move(data)) {}

    /**
     * Invoked by the NamespaceString child class. It bypass the normal DatabaseName constructor
     * which would remove the collection of the NamespaceString.
     */
    DatabaseName(const Storage& data, size_t size, TrustedInitTag) noexcept : _data(data, size) {}

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
            auto tenantId = TenantId{OID::from(_data.data() + kDataOffset)};
            return str::stream() << tenantId.toString() << "_" << db(omitTenant);
        }

        return db(omitTenant).toString();
    }

    static constexpr size_t kDataOffset = sizeof(uint8_t);
    static constexpr uint8_t kTenantIdMask = 0x80;
    static constexpr uint8_t kDatabaseNameOffsetEndMask = 0x7F;

    bool hasTenantId() const {
        return static_cast<uint8_t>(*_data.data()) & kTenantIdMask;
    }

    /**
     * Storage contains the underlying data for a DatabaseName or a NamespaceString. The data is
     * accessible using the data() and size() accessors. It is packed into a  string consisting of
     * these concatenated parts:
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
     *
     */
    class Storage {
    public:
        constexpr Storage(const char* data, size_t length) noexcept
            : _data(data),
              _length(length),
              _footer(createFooter(database_name::kStaticAllocFlag, 0)) {}

        Storage() noexcept
            : _data(nullptr),
              _length(0),
              _footer(createFooter(database_name::kSmallStringFlag, kDataOffset)) {}

        constexpr ~Storage() {
            if (!std::is_constant_evaluated()) {
                if (isDynamicAlloc() && _data != nullptr) {
                    delete[] _data;
                }
            }
        }

        /**
         * Constructs a copy of other and, possibly, truncate the data.
         *
         * newSize must be smaller of equal than other.size(). It might be smaller if other contains
         * a collection and we are only trying to copy the database part from it.
         */
        Storage(const Storage& other, const size_t newSize)
            : _data(other._data), _length(other._length), _footer(other._footer) {
            if (other.isStaticAlloc() && other.size() == newSize) {
                return;
            } else if (other.isSmallString()) {
                updateFooter(database_name::kSmallStringFlag, newSize);
            } else if (newSize < kSmallStringSize) {
                setFooter(database_name::kSmallStringFlag, newSize);
                memcpy(mutableDataptr(), other._data, newSize);
            } else if (other.isDynamicAlloc()) {
                char* dataptr = new char[newSize];
                _data = dataptr;
                _length = newSize;
                memcpy(dataptr, other._data, newSize);
            }
        }

        Storage(Storage&& other) noexcept
            : _data(other._data), _length(other._length), _footer(other._footer) {
            if (other.isDynamicAlloc()) {
                other.reset();
            }
        }

        Storage& operator=(Storage&& other) noexcept {
            deallocate();
            copy(other);
            if (other.isDynamicAlloc()) {
                other.reset();
            }
            return *this;
        }

        Storage& operator=(const Storage& other) {
            deallocate();
            copy(other);
            if (other.isDynamicAlloc()) {
                char* dataptr = new char[other.size()];
                _data = dataptr;
                memcpy(dataptr, other._data, other.size());
            }
            return *this;
        }

        void copy(const Storage& other) noexcept {
            _data = other._data;
            _length = other._length;
            _footer = other._footer;
        }

        /**
         * Returns a word with a valid flag byte and the rest of the data cleared.
         */
        constexpr size_t createFooter(unsigned char flagsIn, unsigned char length) {
            if (flagsIn & database_name::kSmallStringFlag) {
                flagsIn |= (length << 2);
            }

            char byteflags[sizeof(size_t)] = {0};
            byteflags[sizeof(size_t) - 1] = flagsIn;
            return absl::bit_cast<size_t>(byteflags);
        }

        /**
         * Sets the footer field with the correct flags and length. Clear the first sizeof(_footer)
         * - 1 bytes which might store data when using the small string optimisation.
         */
        void setFooter(unsigned char flagsIn, unsigned char length = 0) {
            _footer = createFooter(flagsIn, length);
        }

        /**
         * Sets the flag and length of the footer field without changing the first sizeof(_footer) -
         * 1 bytes which might contain data.
         */
        void updateFooter(unsigned char flagsIn, unsigned char length) {
            if (flagsIn & database_name::kSmallStringFlag) {
                flagsIn |= (length << 2);
            }
            reinterpret_cast<unsigned char*>(&_footer)[sizeof(size_t) - 1] = flagsIn;
        }

        /**
         * Returns a pointer to the data contained, abstract out the different modes (static
         * allocation, dynamic allocation or small-string optimisation).
         */
        constexpr const char* data() const {
            if (std::is_constant_evaluated()) {
                return _data;
            }
            if (isSmallString())
                return reinterpret_cast<const char*>(&_data);
            return _data;
        }

        constexpr size_t size() const {
            if (isSmallString()) {
                return getFlags() >> 2;
            }
            return _length;
        }

        constexpr bool isSmallString() const {
            return getFlags() & database_name::kSmallStringFlag;
        }

        constexpr bool isDynamicAlloc() const {
            return getFlags() == 0;
        }

        constexpr bool isStaticAlloc() const {
            return getFlags() & database_name::kStaticAllocFlag;
        }

        static Storage make(StringData db,
                            StringData collectionName,
                            bool hasTenant,
                            const char* tenantData) {
            uassert(ErrorCodes::InvalidNamespace,
                    "Collection names cannot start with '.': " + collectionName,
                    collectionName.empty() || collectionName[0] != '.');
            uassert(ErrorCodes::InvalidNamespace,
                    "namespaces cannot have embedded null characters",
                    collectionName.find('\0') == std::string::npos);

            uint8_t details = db.size() & kDatabaseNameOffsetEndMask;
            size_t dbStartIndex = kDataOffset;
            if (hasTenant) {
                dbStartIndex += OID::kOIDSize;
                details |= kTenantIdMask;
            }

            Storage data;
            // Note there is no null terminator.
            constexpr size_t dotSize = 1;
            const size_t length = collectionName.empty()
                ? dbStartIndex + db.size()
                : dbStartIndex + db.size() + dotSize + collectionName.size();

            char* dataptr;
            if (length > kSmallStringSize) {
                dataptr = new char[length];
                data._data = dataptr;
                data._length = length;
                data._footer = 0;
            } else {
                data.setFooter(database_name::kSmallStringFlag, static_cast<unsigned char>(length));
                dataptr = data.mutableDataptr();
            }
            invariant(dataptr == data.mutableDataptr());

            *dataptr = details;
            if (hasTenant) {
                std::memcpy(dataptr + kDataOffset, tenantData, OID::kOIDSize);
            }

            if (!db.empty()) {
                std::memcpy(dataptr + dbStartIndex, db.data(), db.size());
            }

            if (!collectionName.empty()) {
                *(dataptr + dbStartIndex + db.size()) = '.';
                std::memcpy(dataptr + dbStartIndex + db.size() + dotSize,
                            collectionName.rawData(),
                            collectionName.size());
            }

            return data;
        }

        static Storage make(const DatabaseName& dbName, StringData collectionName) {
            return make(dbName.db(omitTenant),
                        collectionName,
                        dbName.hasTenantId(),
                        dbName._data.data() + kDataOffset);
        }

        static Storage make(boost::optional<TenantId> tenantId,
                            StringData db,
                            StringData collectionName) {
            uassert(ErrorCodes::InvalidNamespace,
                    fmt::format("db name must be at most {} characters, found: {}",
                                kMaxDatabaseNameLength,
                                db.size()),
                    db.size() <= kMaxDatabaseNameLength);
            uassert(ErrorCodes::InvalidNamespace,
                    "namespace cannot have embedded null characters",
                    db.find('\0') == std::string::npos);

            const char* tenantData = tenantId ? tenantId->_oid.view().view() : nullptr;

            return make(db, collectionName, !!tenantId, tenantData);
        }

        static Storage make(boost::optional<TenantId> tenantId, StringData ns) {
            auto dotIndex = ns.find('.');
            if (dotIndex == std::string::npos) {
                return make(tenantId, ns, StringData{});
            }

            return make(tenantId, ns.substr(0, dotIndex), ns.substr(dotIndex + 1));
        }

    private:
        /**
         * Extracts the flags's byte from the _footer field.
         */
        constexpr unsigned char getFlags() const {
            return absl::bit_cast<std::array<char, sizeof(size_t)>>(_footer)[sizeof(size_t) - 1];
        }

        char* mutableDataptr() {
            if (isSmallString())
                return reinterpret_cast<char*>(&_data);
            return const_cast<char*>(_data);
        }

        /**
         * Resets the data to an empty Storage (small string optimisation without data).
         */
        void reset() {
            _data = nullptr;
            _length = 0;
            setFooter(database_name::kSmallStringFlag, kDataOffset);
        }

        void deallocate() {
            if (isDynamicAlloc() && _data != nullptr) {
                delete[] _data;
                reset();
            }
        }

        /**
         * Storage can work in three different mode (dynamic allocation, static allocation or
         * small-string optimisation) depending on the flag bits (the last two bits of _footer) :
         *     Flags value given by _footer[sizeof(_footer) - 1] & 0x00000011:
         *         0: the data is dynamically allocated.
         *         1: the data is statically allocated.
         *         2: the data is packed using the small string optimisation
         *
         * When using static of dynamic allocation, _data is a pointer to the actual data and
         * _length contains its size.
         *
         * When using the small string optimisation the data is packed in _data, _length and the
         * first sizeof(_footer)-1 bytes of _footer. The size of the data is contained in the first
         * 6 bits of the last byte of _footer :
         */
        const char* _data;
        size_t _length;
        size_t _footer;
    };
    Storage _data;

    static constexpr const size_t kSmallStringSize =
        std::min(sizeof(Storage) - sizeof(char), size_t(63));
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

// The `constexpr` definitions for `DatabaseName` static data members are below.
namespace dbname_detail::constexpr_data {
template <size_t dbSize>
constexpr auto makeDbData(const char* db) {
    // The 1 is the length/no-tenant marker.
    std::array<char, 1 + dbSize> result{};
    auto p = result.begin();
    *p++ = dbSize;
    p = std::copy_n(db, dbSize, p);
    return result;
}
#define DBNAME_CONSTANT(id, db) \
    constexpr inline auto id##_data = makeDbData<db.size()>(db.rawData());
#include "database_name_reserved.def.h"
#undef DBNAME_CONSTANT
}  // namespace dbname_detail::constexpr_data

#define DBNAME_CONSTANT(id, db)                          \
    constexpr inline DatabaseName DatabaseName::id(      \
        dbname_detail::constexpr_data::id##_data.data(), \
        dbname_detail::constexpr_data::id##_data.size());
#include "database_name_reserved.def.h"
#undef DBNAME_CONSTANT

}  // namespace mongo
