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

#include "mongo/db/database_name.h"
#include "mongo/db/tenant_id.h"

namespace mongo {

class DatabaseNameUtil {
public:
    /**
     * Serializes a DatabaseName object.
     *
     * If multitenancySupport is enabled and featureFlagRequireTenantID is enabled, then tenantId is
     * not included in the serialization.
     * eg. serialize(DatabaseName(tenantID, "foo")) -> "foo"
     *
     * If multitenancySupport is enabled and featureFlagRequireTenantID is disabled, then tenantId
     * is included in the serialization.
     * eg. serialize(DatabaseName(tenantID, "foo")) -> "tenantID_foo"
     *
     * If multitenancySupport is disabled, the tenantID is not set in the DatabaseName Object.
     * eg. serialize(DatabaseName(boost::none, "foo")) -> "foo"
     */
    static std::string serialize(const DatabaseName& dbName);

    /**
     * Deserializes StringData dbName to a DatabaseName object.
     *
     * If multitenancySupport is enabled and featureFlagRequireTenantID is enabled, then a
     * DatabaseName object is constructed using the tenantId passed in to the constructor. The
     * invariant requires tenantID to be initialized and passed to the constructor.
     * eg. deserialize(tenantID, "foo") -> DatabaseName(tenantID, "foo")
     *
     * If multitenancySupport is enabled and featureFlagRequireTenantID is disabled, then dbName is
     * required to be prefixed with a tenantID. The tenantID parameter is ignored and
     * DatabaseName is constructed using only ns. The invariant requires that if a tenantID
     * is a parameter, then the tenantID is equal to the prefixed tenantID.
     * eg. deserialize(boost::none, "preTenantID_foo") -> DatabaseName(preTenantId,
     * "foo")
     *
     * If multitenancySupport is disabled then the invariant requires tenantID to not be initialized
     * and DatabaseName is constructed without the tenantID.
     * eg. deserialize(boost::none, "foo") -> DatabaseName(boost::none, "foo")
     */
    static DatabaseName deserialize(boost::optional<TenantId> tenantId, StringData db);
};

}  // namespace mongo
