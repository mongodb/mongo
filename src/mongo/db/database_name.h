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
#include <boost/optional.hpp>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

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

        private:
            StringData _db;
            mutable std::once_flag _once;
            mutable const DatabaseName* _dbName = nullptr;
        };

        constexpr explicit ConstantProxy(const SharedState* sharedState)
            : _sharedState{sharedState} {}

        operator const DatabaseName&() const {
            return _get();
        }

        decltype(auto) db() const {
            return _get().db();
        }
        decltype(auto) tenantId() const {
            return _get().tenantId();
        }
        decltype(auto) toString() const {
            return _get().toString();
        }

        friend std::ostream& operator<<(std::ostream& stream, const ConstantProxy& dbName) {
            return stream << dbName.toString();
        }
        friend StringBuilder& operator<<(StringBuilder& builder, const ConstantProxy& dbName) {
            return builder << dbName.toString();
        }

    private:
        const DatabaseName& _get() const {
            return _sharedState->get();
        }

        const SharedState* _sharedState;
    };

#define DBNAME_CONSTANT(id, db) static const ConstantProxy id;
#include "database_name_reserved.def.h"
#undef DBNAME_CONSTANT

    static constexpr size_t kMaxDatabaseNameLength = 63;

    /**
     * Constructs an empty DatabaseName.
     */
    DatabaseName() = default;

    /**
     * This function constructs a DatabaseName without checking for presence of TenantId. It
     * must only be used by auth systems which are not yet tenant aware.
     *
     * TODO SERVER-76294 Remove this function. Any remaining call sites must be changed to use a
     * function on DatabaseNameUtil.
     */
    static DatabaseName createDatabaseNameForAuth(const boost::optional<TenantId>& tenantId,
                                                  StringData dbString) {
        return DatabaseName(tenantId, dbString);
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

    /**
     * Prefer to use the constructor above.
     * TODO SERVER-65456 Remove this constructor.
     */
    explicit DatabaseName(StringData dbName, boost::optional<TenantId> tenantId = boost::none)
        : DatabaseName(std::move(tenantId), dbName) {}

    boost::optional<TenantId> tenantId() const {
        if (!_hasTenantId()) {
            return boost::none;
        }

        return TenantId{OID::from(&_data[kDataOffset])};
    }

    StringData db() const {
        auto offset = _hasTenantId() ? kDataOffset + OID::kOIDSize : kDataOffset;
        return StringData{_data.data() + offset, _data.size() - offset};
    }

    bool isEmpty() const {
        return _data.size() == kDataOffset;
    }

    std::string toString() const {
        return db().toString();
    }

    std::string toStringWithTenantId() const {
        if (_hasTenantId()) {
            auto tenantId = TenantId{OID::from(&_data[kDataOffset])};
            return str::stream() << tenantId.toString() << "_" << db();
        }

        return db().toString();
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

    bool equalCaseInsensitive(const DatabaseName& other) const {
        return StringData{_data.data() + kDataOffset, _data.size() - kDataOffset}
            .equalCaseInsensitive(
                StringData{other._data.data() + kDataOffset, other._data.size() - kDataOffset});
    }

    friend std::ostream& operator<<(std::ostream& stream, const DatabaseName& tdb) {
        return stream << tdb.toString();
    }

    friend StringBuilder& operator<<(StringBuilder& builder, const DatabaseName& tdb) {
        return builder << tdb.toString();
    }

    int compare(const DatabaseName& other) const {
        if (_hasTenantId() && !other._hasTenantId()) {
            return 1;
        }

        if (other._hasTenantId() && !_hasTenantId()) {
            return -1;
        }

        return StringData{_data.data() + kDataOffset, _data.size() - kDataOffset}.compare(
            StringData{other._data.data() + kDataOffset, other._data.size() - kDataOffset});
    }

    friend bool operator==(const DatabaseName& lhs, const DatabaseName& rhs) {
        return lhs._data == rhs._data;
    }

    friend bool operator!=(const DatabaseName& lhs, const DatabaseName& rhs) {
        return lhs._data != rhs._data;
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
        return H::combine(std::move(h), obj._data);
    }

    friend auto logAttrs(const DatabaseName& obj) {
        return "db"_attr = obj;
    }

private:
    friend class NamespaceString;
    friend class NamespaceStringOrUUID;
    friend class DatabaseNameUtil;

    /**
     * Constructs a DatabaseName from the given tenantId and database name.
     * "dbName" is expected only consist of a db name. It is the caller's responsibility to ensure
     * the dbName is a valid db name.
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

    static constexpr size_t kDataOffset = sizeof(uint8_t);
    static constexpr uint8_t kTenantIdMask = 0x80;
    static constexpr uint8_t kDatabaseNameOffsetEndMask = 0x7F;

    inline bool _hasTenantId() const {
        return static_cast<uint8_t>(_data.front()) & kTenantIdMask;
    }

    // Private constructor for NamespaceString to construct DatabaseName from its own internal data
    struct TrustedInitTag {};
    DatabaseName(std::string data, TrustedInitTag) : _data(std::move(data)) {}

    // Same in-memory layout as NamespaceString, see documentation in its header
    std::string _data{'\0'};
};

// The `constexpr` definitions for `DatabaseName::ConstantProxy` static data members are below. See
// `constexpr` definitions for the `NamespaceString::ConstantProxy` static data members of NSS in
// namespace_string.h for more details.
namespace dbname_detail::const_proxy_shared_states {
#define DBNAME_CONSTANT(id, db) constexpr inline DatabaseName::ConstantProxy::SharedState id{db};
#include "database_name_reserved.def.h"
#undef DBNAME_CONSTANT
}  // namespace dbname_detail::const_proxy_shared_states

#define DBNAME_CONSTANT(id, db)                                    \
    constexpr inline DatabaseName::ConstantProxy DatabaseName::id{ \
        &dbname_detail::const_proxy_shared_states::id};
#include "database_name_reserved.def.h"
#undef DBNAME_CONSTANT

}  // namespace mongo
