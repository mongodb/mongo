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

#include "mongo/util/namespace_string_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/oid.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

NamespaceString AuthNamespaceStringUtil::deserialize(const boost::optional<TenantId>& tenantId,
                                                     StringData db,
                                                     StringData coll) {
    uassert(ErrorCodes::InternalError,
            "A tenant ID is only accepted when multitenancySupport is on",
            !tenantId || gMultitenancySupport);
    return NamespaceString(tenantId, db, coll);
}

std::string NamespaceStringUtil::serialize(const NamespaceString& ns,
                                           const SerializationContext& context) {
    if (!gMultitenancySupport)
        return ns.toString();

    switch (context.getSource()) {
        case SerializationContext::Source::Command:
            if (context.getCallerType() == SerializationContext::CallerType::Reply) {
                return serializeForCommands(ns, context);
            }
            [[fallthrough]];
        case SerializationContext::Source::Storage:
        case SerializationContext::Source::Default:
            // Use forStorage as the default serializing rule
            return serializeForStorage(ns, context);
        case SerializationContext::Source::Catalog:
            return serializeForCatalog(ns);
        default:
            MONGO_UNREACHABLE;
    }
}

std::string NamespaceStringUtil::serialize(const NamespaceString& ns,
                                           const SerializationOptions& options,
                                           const SerializationContext& context) {
    return options.serializeIdentifier(serialize(ns, context));
}

std::string NamespaceStringUtil::serializeForCatalog(const NamespaceString& ns) {
    return ns.toStringWithTenantId();
}

std::string NamespaceStringUtil::serializeForStorage(const NamespaceString& ns,
                                                     const SerializationContext& context) {
    // TODO SERVER-84275: Change to use isEnabled again.
    // We need to use isEnabledUseLastLTSFCVWhenUninitialized instead of isEnabled because
    // this could run during startup while the FCV is still uninitialized.
    if (gFeatureFlagRequireTenantID.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return ns.toString();
    }
    return ns.toStringWithTenantId();
}

std::string NamespaceStringUtil::serializeForCommands(const NamespaceString& ns,
                                                      const SerializationContext& context) {
    // tenantId came from the security token.
    switch (context.getPrefix()) {
        case SerializationContext::Prefix::ExcludePrefix:
            return ns.toString();
        case SerializationContext::Prefix::IncludePrefix:
            return ns.toStringWithTenantId();
        default:
            MONGO_UNREACHABLE;
    }
}

NamespaceString NamespaceStringUtil::deserialize(boost::optional<TenantId> tenantId,
                                                 StringData ns,
                                                 const SerializationContext& context) {
    auto dotIndex = ns.find('.');
    if (dotIndex != std::string::npos) {
        return deserialize(
            std::move(tenantId), ns.substr(0, dotIndex), ns.substr(dotIndex + 1), context);
    }
    return deserialize(std::move(tenantId), ns, StringData{}, context);
}

NamespaceString NamespaceStringUtil::deserializeForCatalog(
    const boost::optional<TenantId>& tenantId, StringData ns) {
    auto dotIndex = ns.find('.');
    if (dotIndex != std::string::npos) {
        return deserializeForStorage(
            std::move(tenantId), ns.substr(0, dotIndex), ns.substr(dotIndex + 1));
    }
    return deserializeForStorage(std::move(tenantId), ns, StringData{});
}

NamespaceString NamespaceStringUtil::deserialize(const DatabaseName& dbName, StringData coll) {
    return NamespaceString{dbName, coll};
}

NamespaceString NamespaceStringUtil::deserializeForStorage(boost::optional<TenantId> tenantId,
                                                           StringData db,
                                                           StringData coll) {
    // TODO SERVER-84275: Change to use isEnabled again.
    // We need to use isEnabledUseLastLTSFCVWhenUninitialized instead of isEnabled because
    // this could run during startup while the FCV is still uninitialized.
    if (gFeatureFlagRequireTenantID.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        if (!DatabaseName::isInternalDb(db)) {
            massert(6972100,
                    str::stream() << "TenantId must be set on nss " << db << "." << coll,
                    tenantId != boost::none);
        }
        return NamespaceString(std::move(tenantId), db, coll);
    }

    auto nss = parseFromStringExpectTenantIdInMultitenancyMode(db, coll);
    // TenantId could be prefixed, or passed in separately (or both) and namespace is always
    // constructed with the tenantId separately.
    if (tenantId != boost::none) {
        if (!nss.tenantId()) {
            return NamespaceString(std::move(tenantId), db, coll);
        }
        massert(6972101,
                str::stream() << "TenantId must match the db prefix tenantId: "
                              << tenantId->toString() << " prefix " << nss.tenantId()->toString(),
                tenantId == nss.tenantId());
    }

    return nss;
}

NamespaceString NamespaceStringUtil::deserializeForCommands(boost::optional<TenantId> tenantId,
                                                            StringData db,
                                                            StringData coll,
                                                            const SerializationContext& context) {
    // we only get here if we are processing a Command Request.  We disregard the feature flag
    // in this case, essentially letting the request dictate the state of the feature.

    // We shouldn't have a tenant ID prefix without an external tenant ID (from the security token).
    if (tenantId != boost::none) {
        switch (context.getPrefix()) {
            case SerializationContext::Prefix::ExcludePrefix: {
                return NamespaceString(std::move(tenantId), db, coll);
            }
            case SerializationContext::Prefix::IncludePrefix: {
                auto nss = parseFromStringExpectTenantIdInMultitenancyMode(db, coll);
                massert(8423385,
                        str::stream()
                            << "TenantId from security token present as '" << tenantId->toString()
                            << "' with expectPrefix field set but without a prefix set",
                        nss.tenantId());
                massert(8423381,
                        str::stream()
                            << "TenantId from security token must match prefixed "
                               "tenantId: "
                            << tenantId->toString() << " prefix " << nss.tenantId()->toString(),
                        tenantId.value() == nss.tenantId());
                return nss;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    // We received the tenantId from the prefix.
    auto nss = parseFromStringExpectTenantIdInMultitenancyMode(db, coll);
    uassert(8233501,
            str::stream() << "The tenantId must be passed explicitely " << db << "." << coll,
            !nss.hasTenantId());
    uassert(8423387,
            str::stream() << "TenantId must be set on nss " << db << "." << coll,
            nss.dbName().isInternalDb());

    return nss;
}

NamespaceString NamespaceStringUtil::deserialize(const boost::optional<TenantId>& tenantId,
                                                 StringData db,
                                                 StringData coll,
                                                 const SerializationContext& context) {
    if (!gMultitenancySupport) {
        massert(6972102,
                str::stream() << "TenantId must not be set, but it is: " << tenantId->toString(),
                !tenantId);
        return NamespaceString(boost::none, db, coll);
    }

    // if db is empty, we can never have a prefixed tenantId so we don't need to call
    // deserialize
    if ((db.empty() && coll.empty()) || (db.empty() && tenantId)) {
        return NamespaceString(tenantId, db, coll);
    }

    switch (context.getSource()) {
        case SerializationContext::Source::Command:
            if (context.getCallerType() == SerializationContext::CallerType::Request) {
                return deserializeForCommands(std::move(tenantId), db, coll, context);
            }
            [[fallthrough]];
        case SerializationContext::Source::Storage:
        case SerializationContext::Source::Catalog:
        case SerializationContext::Source::Default:
            // Use forStorage as the default deserializing rule
            return deserializeForStorage(std::move(tenantId), db, coll);
        default:
            MONGO_UNREACHABLE;
    }
}

NamespaceString NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
    StringData ns) {
    auto dotIndex = ns.find('.');
    if (dotIndex != std::string::npos) {
        return parseFromStringExpectTenantIdInMultitenancyMode(ns.substr(0, dotIndex),
                                                               ns.substr(dotIndex + 1));
    }
    return parseFromStringExpectTenantIdInMultitenancyMode(ns, StringData{});
}

NamespaceString NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
    StringData db, StringData coll) {

    if (!gMultitenancySupport) {
        return NamespaceString(boost::none, db, coll);
    }

    const auto tenantDelim = db.find('_');
    if (tenantDelim == std::string::npos) {
        return NamespaceString(boost::none, db, coll);
    }

    auto swOID = OID::parse(db.substr(0, tenantDelim));
    if (!swOID.getStatus().isOK()) {
        // If we fail to parse an OID, either the size of the substring is incorrect, or there
        // is an invalid character. This indicates that the db has the "_" character, but it
        // does not act as a delimeter for a tenantId prefix.
        return NamespaceString(boost::none, db, coll);
    }

    const TenantId tenantId(swOID.getValue());
    return NamespaceString(tenantId, db.substr(tenantDelim + 1, db.size() - 1 - tenantDelim), coll);
}

NamespaceString NamespaceStringUtil::parseFailPointData(const BSONObj& data,
                                                        StringData nsFieldName,
                                                        const boost::optional<TenantId>& tenantId) {
    const auto ns = data.getStringField(nsFieldName);
    return NamespaceStringUtil::deserialize(tenantId, ns, SerializationContext::stateDefault());
}

NamespaceString NamespaceStringUtil::deserializeForErrorMsg(StringData nsInErrMsg) {
    // TenantId always prefix in the error message. This method returns either (tenantId,
    // nonPrefixedDb) or (none, prefixedDb) depending on gMultitenancySupport flag.
    return NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(nsInErrMsg);
}

}  // namespace mongo
