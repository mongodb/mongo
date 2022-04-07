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
 * A TenantDatabaseName is a unique name for database.
 * It holds a database name and tenant id, if one exists. In a serverless environment, a tenant id
 * is expected to exist so that a database can be uniquely identified.
 */
class TenantDatabaseName {
public:
    /**
     * Constructs an empty TenantDatabaseName.
     */
    TenantDatabaseName() : _tenantId(boost::none), _dbName(""), _tenantDbName(boost::none){};

    /**
     * Constructs a TenantDatabaseName from the given tenantId and database name.
     * "dbName" is expected only consist of a db name. It is the caller's responsibility to ensure
     * the dbName is a valid db name.
     */
    TenantDatabaseName(boost::optional<TenantId> tenantId, StringData dbName) {
        _tenantId = tenantId;
        _dbName = dbName.toString();

        _tenantDbName =
            _tenantId ? boost::make_optional(_tenantId->toString() + "_" + _dbName) : boost::none;
    }

    /**
     * Prefer to use the constructor above.
     * TODO SERVER-65456 Remove this constructor.
     */
    TenantDatabaseName(StringData dbName, boost::optional<TenantId> tenantId = boost::none)
        : TenantDatabaseName(tenantId, dbName) {}

    static TenantDatabaseName createSystemTenantDbName(StringData dbName);

    const boost::optional<TenantId> tenantId() const {
        return _tenantId;
    }

    const std::string& dbName() const {
        return _dbName;
    }

    const std::string& fullName() const {
        if (_tenantDbName)
            return *_tenantDbName;

        invariant(!_tenantId);
        return _dbName;
    }

    const std::string& toString() const {
        return fullName();
    }

    bool equalCaseInsensitive(const TenantDatabaseName& other) const {
        return boost::iequals(fullName(), other.fullName());
    }

    /**
     * Returns -1, 0, or 1 if 'this' is less, equal, or greater than 'other' in
     * lexicographical order.
     */
    int compare(const TenantDatabaseName& other) const {
        return fullName().compare(other.fullName());
    }

    template <typename H>
    friend H AbslHashValue(H h, const TenantDatabaseName& obj) {
        return H::combine(std::move(h), obj.fullName());
    }

    friend auto logAttrs(const TenantDatabaseName& obj) {
        return "tenantDatabaseName"_attr = obj;
    }

private:
    boost::optional<TenantId> _tenantId;
    std::string _dbName;
    boost::optional<std::string> _tenantDbName;
};

inline std::ostream& operator<<(std::ostream& stream, const TenantDatabaseName& tdb) {
    return stream << tdb.fullName();
}

inline StringBuilder& operator<<(StringBuilder& builder, const TenantDatabaseName& tdb) {
    return builder << tdb.fullName();
}

inline bool operator==(const TenantDatabaseName& lhs, const TenantDatabaseName& rhs) {
    return lhs.compare(rhs) == 0;
}

inline bool operator!=(const TenantDatabaseName& lhs, const TenantDatabaseName& rhs) {
    return !(lhs == rhs);
}

inline bool operator<(const TenantDatabaseName& lhs, const TenantDatabaseName& rhs) {
    return lhs.compare(rhs) < 0;
}

inline bool operator>(const TenantDatabaseName& lhs, const TenantDatabaseName& rhs) {
    return rhs < lhs;
}

inline bool operator<=(const TenantDatabaseName& lhs, const TenantDatabaseName& rhs) {
    return !(lhs > rhs);
}

inline bool operator>=(const TenantDatabaseName& lhs, const TenantDatabaseName& rhs) {
    return !(lhs < rhs);
}

}  // namespace mongo
