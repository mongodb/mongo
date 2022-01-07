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
#include <boost/optional.hpp>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/server_feature_flags_gen.h"

namespace mongo {
class TenantDatabaseName {
public:
    /**
     * Constructs a TenantDatabaseName from the given tenantId and database name.
     * "dbName" is expected only consist of a db name. It is the caller's responsibility to ensure
     * the dbName is a valid db name.
     *
     * If featureFlagRequireTenantID is set, tenantId is required.
     */
    TenantDatabaseName(boost::optional<mongo::OID> tenantId, StringData dbName) {
        // TODO SERVER-62114 Check instead if gMultitenancySupport is enabled.
        if (gFeatureFlagRequireTenantID.isEnabledAndIgnoreFCV())
            invariant(tenantId);

        _tenantId = tenantId;
        _dbName = dbName.toString();

        _tenantDbName =
            _tenantId ? boost::make_optional(_tenantId->toString() + "_" + _dbName) : boost::none;
    }

    const boost::optional<mongo::OID> tenantId() const {
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

    friend bool operator==(const TenantDatabaseName& l, const TenantDatabaseName& r) {
        return l.tenantId() == r.tenantId() && l.dbName() == r.dbName();
    }

    friend bool operator!=(const TenantDatabaseName& l, const TenantDatabaseName& r) {
        return !(l == r);
    }

    template <typename H>
    friend H AbslHashValue(H h, const TenantDatabaseName& obj) {
        return H::combine(std::move(h), obj.fullName());
    }

    friend auto logAttrs(const TenantDatabaseName& obj) {
        return "tenantDatabaseName"_attr = obj;
    }

private:
    boost::optional<mongo::OID> _tenantId;
    std::string _dbName;
    boost::optional<std::string> _tenantDbName;
};
}  // namespace mongo
