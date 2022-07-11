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

namespace mongo {

/**
 * A DatabaseName is a unique name for database.
 * It holds a database name and tenant id, if one exists. In a serverless environment, a tenant id
 * is expected to exist so that a database can be uniquely identified.
 */
class DatabaseName {
public:
    /**
     * Constructs an empty DatabaseName.
     */
    DatabaseName() = default;

    /**
     * Constructs a DatabaseName from the given tenantId and database name.
     * "dbName" is expected only consist of a db name. It is the caller's responsibility to ensure
     * the dbName is a valid db name.
     */
    DatabaseName(boost::optional<TenantId> tenantId, StringData dbString)
        : _tenantId(std::move(tenantId)), _dbString(dbString.toString()) {
        uassert(ErrorCodes::InvalidNamespace,
                "'.' is an invalid character in a db name: " + _dbString,
                dbString.find('.') == std::string::npos);

        uassert(ErrorCodes::InvalidNamespace,
                "database names cannot have embedded null characters",
                dbString.find('\0') == std::string::npos);
    }

    /**
     * Prefer to use the constructor above.
     * TODO SERVER-65456 Remove this constructor.
     */
    DatabaseName(StringData dbName, boost::optional<TenantId> tenantId = boost::none)
        : DatabaseName(std::move(tenantId), dbName) {}

    static DatabaseName createSystemTenantDbName(StringData dbString);

    const boost::optional<TenantId>& tenantId() const {
        return _tenantId;
    }

    const std::string& db() const {
        return _dbString;
    }

    const std::string& toString() const {
        return db();
    }

    std::string toStringWithTenantId() const {
        if (_tenantId)
            return str::stream() << *_tenantId << '_' << _dbString;

        return _dbString;
    }

    bool equalCaseInsensitive(const DatabaseName& other) const {
        return boost::iequals(toStringWithTenantId(), other.toStringWithTenantId());
    }

    template <typename H>
    friend H AbslHashValue(H h, const DatabaseName& obj) {
        if (obj._tenantId) {
            return H::combine(std::move(h), obj._tenantId.get(), obj._dbString);
        }
        return H::combine(std::move(h), obj._dbString);
    }

    friend auto logAttrs(const DatabaseName& obj) {
        return "databaseName"_attr = obj;
    }

private:
    boost::optional<TenantId> _tenantId = boost::none;
    std::string _dbString;
};

inline std::ostream& operator<<(std::ostream& stream, const DatabaseName& tdb) {
    return stream << tdb.toString();
}

inline StringBuilder& operator<<(StringBuilder& builder, const DatabaseName& tdb) {
    return builder << tdb.toString();
}

inline bool operator==(const DatabaseName& lhs, const DatabaseName& rhs) {
    return (lhs.tenantId() == rhs.tenantId()) && (lhs.db() == rhs.db());
}

inline bool operator!=(const DatabaseName& lhs, const DatabaseName& rhs) {
    return !(lhs == rhs);
}

inline bool operator<(const DatabaseName& lhs, const DatabaseName& rhs) {
    if (lhs.tenantId() != rhs.tenantId()) {
        return lhs.tenantId() < rhs.tenantId();
    }
    return lhs.db() < rhs.db();
}

inline bool operator>(const DatabaseName& lhs, const DatabaseName& rhs) {
    if (lhs.tenantId() != rhs.tenantId()) {
        return lhs.tenantId() > rhs.tenantId();
    }
    return lhs.db() > rhs.db();
}

inline bool operator<=(const DatabaseName& lhs, const DatabaseName& rhs) {
    return !(lhs > rhs);
}

inline bool operator>=(const DatabaseName& lhs, const DatabaseName& rhs) {
    return !(lhs < rhs);
}

}  // namespace mongo
