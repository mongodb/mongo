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

#include <string>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/serialization_context.h"

namespace mongo {

/**
 * This utility class is only meant to be use for pattern-matching case in Authentication. For this
 * use case incomplete types (ex: NS with a coll and tenant ID but no DB) might be created and this
 * class allows it.
 *
 * All other use-cases should use NamespaceStringUtil to avoid breaking tenant isolation.
 */
class AuthNamespaceStringUtil {
public:
    /**
     * Wrapper for the NamespaceString constructor without validation on "db". It should only
     * be used in specific auth use-cases. Does not expect "db" to be prefixed when multitenancy is
     * on.
     */
    static NamespaceString deserialize(const boost::optional<TenantId>& tenantId,
                                       StringData db,
                                       StringData coll);
};

class NamespaceStringUtil {
public:
    /**
     * Serializes a NamespaceString object.
     *
     * If multitenancySupport is enabled and featureFlagRequireTenantID is enabled, then tenantId is
     * not included in the serialization.
     * eg. serialize(NamespaceString(tenantID, "foo.bar")) -> "foo.bar"
     *
     * If multitenancySupport is enabled and featureFlagRequireTenantID is disabled, then tenantId
     * is included in the serialization.
     * eg. serialize(NamespaceString(tenantID, "foo.bar")) -> "tenantID_foo.bar"
     *
     * If multitenancySupport is enabled and we are serializing a command reply, the
     * featureFlagRequireTenantID has no bearing on whether we prefix or not, and is dependent on
     * the value of the expectPrefix field in the request at the time of deserialization, and
     * whether or not the tenantId was provided as a prefix.
     *
     * If multitenancySupport is disabled, the tenantID is not set in the NamespaceString Object.
     * eg. serialize(NamespaceString(boost::none, "foo.bar")) -> "foo.bar"
     *
     * Do not use this function when serializing a NamespaceString object for catalog.
     */
    static std::string serialize(const NamespaceString& ns, const SerializationContext& context);

    static std::string serialize(const NamespaceString& ns,
                                 const SerializationOptions& options,
                                 const SerializationContext& context);

    /**
     * Serializes a NamespaceString object for catalog.
     *
     * Always includes the tenantId prefix for the catalog serialization.
     * eg. serializeForCatalog(NamespaceString(tenantID, "foo.bar")) -> "tenantID_foo.bar"
     *
     * MUST only be used for serializing a NamespaceString object for catalog.
     */
    static std::string serializeForCatalog(const NamespaceString& ns);

    /**
     * Deserializes StringData ns to a NamespaceString object.
     *
     * If multitenancySupport is enabled and featureFlagRequireTenantID is enabled, then a
     * NamespaceString object is constructed using the tenantId passed in to the constructor. The
     * invariant requires tenantID to be initialized and passed to the constructor.
     * eg. deserialize(tenantID, "foo.bar") -> NamespaceString(tenantID, "foo.bar")
     *
     * If multitenancySupport is enabled and featureFlagRequireTenantID is disabled, then ns is
     * required to be prefixed with a tenantID. The tenantID parameter is ignored and
     * NamespaceString is constructed using only ns. The invariant requires that if a tenantID
     * is a parameter, then the tenatID is equal to the prefixed tenantID.
     * eg. deserialize(boost::none, "preTenantID_foo.bar") -> NamespaceString(preTenantId,
     * "foo.bar")
     *
     * If multitenancySupport is enabled and we are deserializing a command request, we will extract
     * it from the prefix if a tenantId is not provided, otherwise we rely on the value of the
     * expectPrefix field in the request to determine whether or not we should expect to parse a
     * prefix.
     *
     * If multitenancySupport is disabled then the invariant requires tenantID to not be initialized
     * and NamespaceString is constructor without the tenantID.
     * eg. deserialize(boost::none, "foo.bar") -> NamespaceString(boost::none, "foo.bar")
     */
    static NamespaceString deserialize(boost::optional<TenantId> tenantId,
                                       StringData ns,
                                       const SerializationContext& context);

    static NamespaceString deserialize(const DatabaseName& dbName, StringData coll);

    static NamespaceString deserialize(const boost::optional<TenantId>& tenantId,
                                       StringData db,
                                       StringData coll,
                                       const SerializationContext& context);

    /**
     * Constructs a NamespaceString from the string 'ns'. Should only be used when reading a
     * namespace from disk. 'ns' is expected to contain a tenantId when running in Serverless mode.
     */
    static NamespaceString parseFromStringExpectTenantIdInMultitenancyMode(StringData ns);

    /**
     * To be used within a Failpoint. When used in the `executeIf` we parse a BSONObj which should
     * contain a field for the namespace string (such as `ns` or `namespace` etc..).
     */
    static NamespaceString parseFailPointData(const BSONObj& data, StringData nsFieldName);

    /**
     * To be used only for deserializing a NamespaceString object from a ns string in error
     * messages.
     */
    static NamespaceString deserializeForErrorMsg(StringData nsInErrMsg);

private:
    static std::string serializeForStorage(const NamespaceString& ns,
                                           const SerializationContext& context);

    static std::string serializeForCommands(const NamespaceString& ns,
                                            const SerializationContext& context);

    static NamespaceString deserializeForStorage(boost::optional<TenantId> tenantId,
                                                 StringData ns,
                                                 const SerializationContext& context);

    static NamespaceString deserializeForCommands(boost::optional<TenantId> tenantId,
                                                  StringData ns,
                                                  const SerializationContext& context);
};

}  // namespace mongo
