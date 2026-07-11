// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/database_name.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/serialization_context.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * This class is meant for specific authentication use cases, mainly to serialize or deserialize
 * `authDB`. Engineers should default to use `DatabaseNameUtil` unless the use-case has been
 * carefully vetted in multitenancy.
 */
class AuthDatabaseNameUtil {
public:
    /**
     * This method should only be used in authentication code to deserialize `authDB` (which can be
     * any value and doesn't have a tenant). All other cases should use `DatabaseNameUtil`.
     */
    static DatabaseName deserialize(std::string_view db) {
        return DatabaseName(boost::none, db);
    }
};

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
     * If multitenancySupport is enabled and we are serializing a command reply, the
     * featureFlagRequireTenantID has no bearing on whether we prefix or not, and is dependent on
     * the value of the expectPrefix field in the request at the time of deserialization, and
     * whether or not the tenantId was provided as a prefix.
     *
     * If multitenancySupport is disabled, the tenantID is not set in the DatabaseName Object.
     * eg. serialize(DatabaseName(boost::none, "foo")) -> "foo"
     */
    static std::string serialize(const DatabaseName& dbName, const SerializationContext& context);

    /**
     * Deserializes std::string_view dbName to a DatabaseName object.
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
     * If multitenancySupport is enabled and we are deserializing a command request, we will extract
     * it from the prefix if a tenantId is not provided, otherwise we rely on the value of the
     * expectPrefix field in the request to determine whether or not we should expect to parse a
     * prefix.
     *
     * If multitenancySupport is disabled then the invariant requires tenantID to not be initialized
     * and DatabaseName is constructed without the tenantID.
     * eg. deserialize(boost::none, "foo") -> DatabaseName(boost::none, "foo")
     */
    static DatabaseName deserialize(boost::optional<TenantId> tenantId,
                                    std::string_view db,
                                    const SerializationContext& context);

    /**
     * To be used with Failpoints since they can be database specific. Parses the `data` BSONObj to
     * find an existing `dbFieldName` and returns a DatabaseName object from it.
     */
    static DatabaseName parseFailPointData(const BSONObj& data, std::string_view dbFieldName);

    /**
     * To be used only for deserializing a DatabaseName object from a db string in error messages.
     */
    static DatabaseName deserializeForErrorMsg(std::string_view dbInErrMsg);

private:
    static DatabaseName parseFromStringExpectTenantIdInMultitenancyMode(std::string_view dbName);

    static std::string serializeForStorage(const DatabaseName& dbName);

    static std::string serializeForCommands(const DatabaseName& dbName,
                                            const SerializationContext& context);

    static DatabaseName deserializeForStorage(boost::optional<TenantId> tenantId,
                                              std::string_view db);

    static DatabaseName deserializeForCommands(boost::optional<TenantId> tenantId,
                                               std::string_view db,
                                               const SerializationContext& context);

    static DatabaseName deserializeForCatalog(boost::optional<TenantId> tenantId,
                                              std::string_view db);
};

}  // namespace mongo
