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

#include "mongo/util/database_name_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

std::string DatabaseNameUtil::serialize(const DatabaseName& dbName,
                                        const SerializationContext& context) {
    if (!gMultitenancySupport)
        return dbName.toString();

    switch (context.getSource()) {
        case SerializationContext::Source::Command:
            return serializeForCommands(dbName, context);
        case SerializationContext::Source::Catalog:
            return dbName.toStringWithTenantId();
        case SerializationContext::Source::Storage:
        case SerializationContext::Source::Default:
            // Use forStorage as the default serializing rule
            return serializeForStorage(dbName);
        default:
            MONGO_UNREACHABLE;
    }
}

std::string DatabaseNameUtil::serializeForStorage(const DatabaseName& dbName) {
    // TODO SERVER-84275: Change to use isEnabled again.
    // We need to use isEnabledUseLastLTSFCVWhenUninitialized instead of isEnabled because
    // this could run during startup while the FCV is still uninitialized.
    if (gFeatureFlagRequireTenantID.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return dbName.toString();
    }
    return dbName.toStringWithTenantId();
}

std::string DatabaseNameUtil::serializeForCommands(const DatabaseName& dbName,
                                                   const SerializationContext& context) {
    // tenantId came from either a $tenant field or security token.
    if (context.receivedNonPrefixedTenantId()) {
        switch (context.getPrefix()) {
            case SerializationContext::Prefix::ExcludePrefix:
                // fallthrough
            case SerializationContext::Prefix::Default:
                return dbName.toString();
            case SerializationContext::Prefix::IncludePrefix:
                return dbName.toStringWithTenantId();
            default:
                MONGO_UNREACHABLE;
        }
    }

    // tenantId came from the prefix.
    switch (context.getPrefix()) {
        case SerializationContext::Prefix::ExcludePrefix:
            return dbName.toString();
        case SerializationContext::Prefix::Default:
            // fallthrough
        case SerializationContext::Prefix::IncludePrefix:
            return dbName.toStringWithTenantId();
        default:
            MONGO_UNREACHABLE;
    }
}

DatabaseName DatabaseNameUtil::parseFromStringExpectTenantIdInMultitenancyMode(StringData dbName) {
    if (!gMultitenancySupport) {
        return DatabaseName(boost::none, dbName);
    }

    auto tenantDelim = dbName.find('_');
    if (tenantDelim == std::string::npos) {
        return DatabaseName(boost::none, dbName);
    }

    auto swOID = OID::parse(dbName.substr(0, tenantDelim));
    if (swOID.getStatus() == ErrorCodes::BadValue) {
        // If we fail to parse an OID, either the size of the substring is incorrect, or there is an
        // invalid character. This indicates that the db has the "_" character, but it does not act
        // as a delimeter for a tenantId prefix.
        return DatabaseName(boost::none, dbName);
    }

    const TenantId tenantId(swOID.getValue());
    return DatabaseName(tenantId, dbName.substr(tenantDelim + 1));
}

DatabaseName DatabaseNameUtil::deserialize(boost::optional<TenantId> tenantId,
                                           StringData db,
                                           const SerializationContext& context) {
    if (!gMultitenancySupport) {
        uassert(7005302, "TenantId must not be set, but it is: ", tenantId == boost::none);
        return DatabaseName(boost::none, db);
    }

    if (db.empty()) {
        return DatabaseName(tenantId, "");
    }

    switch (context.getSource()) {
        case SerializationContext::Source::Catalog:
            return deserializeForCatalog(std::move(tenantId), db);
        case SerializationContext::Source::Command:
            if (context.getCallerType() == SerializationContext::CallerType::Request) {
                return deserializeForCommands(std::move(tenantId), db, context);
            }
            [[fallthrough]];
        case SerializationContext::Source::Storage:
        case SerializationContext::Source::Default:
            // Use forStorage as the default deserializing rule
            return deserializeForStorage(std::move(tenantId), db);
        default:
            MONGO_UNREACHABLE;
    }
}

DatabaseName DatabaseNameUtil::deserializeForStorage(boost::optional<TenantId> tenantId,
                                                     StringData db) {
    // TODO SERVER-84275: Change to use isEnabled again.
    // We need to use isEnabledUseLastLTSFCVWhenUninitialized instead of isEnabled because
    // this could run during startup while the FCV is still uninitialized.
    if (gFeatureFlagRequireTenantID.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        if (db != DatabaseName::kAdmin.db(omitTenant) &&
            db != DatabaseName::kLocal.db(omitTenant) &&
            db != DatabaseName::kConfig.db(omitTenant) &&
            db != DatabaseName::kExternal.db(omitTenant))
            uassert(7005300, "TenantId must be set", tenantId != boost::none);

        return DatabaseName(std::move(tenantId), db);
    }

    auto dbName = DatabaseNameUtil::parseFromStringExpectTenantIdInMultitenancyMode(db);
    // TenantId could be prefixed, or passed in separately (or both) and namespace is always
    // constructed with the tenantId separately.
    if (tenantId != boost::none) {
        if (!dbName.tenantId()) {
            return DatabaseName(std::move(tenantId), db);
        }
        uassert(7005301, "TenantId must match that in db prefix", tenantId == dbName.tenantId());
    }
    return dbName;
}

DatabaseName DatabaseNameUtil::deserializeForCommands(boost::optional<TenantId> tenantId,
                                                      StringData db,
                                                      const SerializationContext& context) {
    // we only get here if we are processing a Command Request.  We disregard the feature flag in
    // this case, essentially letting the request dictate the state of the feature.

    // We received a tenantId from $tenant or the security token.
    if (tenantId != boost::none && context.receivedNonPrefixedTenantId()) {
        switch (context.getPrefix()) {
            case SerializationContext::Prefix::ExcludePrefix:
                // fallthrough
            case SerializationContext::Prefix::Default:
                return DatabaseName(std::move(tenantId), db);
            case SerializationContext::Prefix::IncludePrefix: {
                auto dbName = parseFromStringExpectTenantIdInMultitenancyMode(db);
                if (!dbName.tenantId() && dbName.isInternalDb()) {
                    return DatabaseName(std::move(tenantId), dbName.db(omitTenant));
                }

                uassert(
                    8423386,
                    str::stream()
                        << "TenantId supplied by $tenant or security token as '"
                        << tenantId->toString()
                        << "' but prefixed tenantId also required given expectPrefix is set true",
                    dbName.tenantId());
                uassert(
                    8423384,
                    str::stream()
                        << "TenantId from $tenant or security token must match prefixed tenantId: "
                        << tenantId->toString() << " prefix " << dbName.tenantId()->toString(),
                    tenantId.value() == dbName.tenantId());
                return dbName;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    // We received the tenantId from the prefix.
    auto dbName = parseFromStringExpectTenantIdInMultitenancyMode(db);
    if (!dbName.isInternalDb() && !dbName.isExternalDB())
        uassert(8423388, "TenantId must be set", dbName.tenantId() != boost::none);

    return dbName;
}

DatabaseName DatabaseNameUtil::deserializeForCatalog(boost::optional<TenantId> tenantId,
                                                     StringData db) {
    // Internally, CollectionCatalog still keys against DatabaseName but needs to address
    // all tenantIds when pattern matching by passing in boost::none.
    if (tenantId == boost::none) {
        return DatabaseNameUtil::parseFromStringExpectTenantIdInMultitenancyMode(db);
    }
    return DatabaseName(std::move(tenantId), db);
}

DatabaseName DatabaseNameUtil::parseFailPointData(const BSONObj& data, StringData dbFieldName) {
    const auto db = data.getStringField(dbFieldName);
    const auto tenantField = data.getField("tenantId");
    const auto tenantId = tenantField.ok()
        ? boost::optional<TenantId>(TenantId::parseFromBSON(tenantField))
        : boost::none;
    return DatabaseNameUtil::deserialize(tenantId, db, SerializationContext::stateDefault());
}

DatabaseName DatabaseNameUtil::deserializeForErrorMsg(StringData dbInErrMsg) {
    // TenantId always prefix in the error message. This method returns either (tenantId,
    // nonPrefixedDb) or (none, prefixedDb) depending on gMultitenancySupport flag.
    return DatabaseNameUtil::parseFromStringExpectTenantIdInMultitenancyMode(dbInErrMsg);
}

}  // namespace mongo
