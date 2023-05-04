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
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/util/str.h"
#include <ostream>

namespace mongo {

std::string NamespaceStringUtil::serialize(const NamespaceString& ns,
                                           const SerializationContext& context,
                                           const SerializationOptions& options) {
    if (!gMultitenancySupport)
        return options.serializeIdentifier(ns.toString());

    // TODO SERVER-74284: uncomment to redirect command-sepcific serialization requests
    // if (context.getSource() == SerializationContext::Source::Command &&
    //     context.getCallerType() == SerializationContext::CallerType::Reply)
    //     return serializeForCommands(ns, context);

    // if we're not serializing a Command Reply, use the default serializing rules
    return options.serializeIdentifier(serializeForStorage(ns, context));
}

std::string NamespaceStringUtil::serializeForStorage(const NamespaceString& ns,
                                                     const SerializationContext& context) {
    if (gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility)) {
        return ns.toString();
    }
    return ns.toStringWithTenantId();
}

std::string NamespaceStringUtil::serializeForCommands(const NamespaceString& ns,
                                                      const SerializationContext& context) {
    // tenantId came from either a $tenant field or security token
    if (context.receivedNonPrefixedTenantId()) {
        switch (context.getPrefix()) {
            case SerializationContext::Prefix::ExcludePrefix:
                // fallthrough
            case SerializationContext::Prefix::Default:
                return ns.toString();
            case SerializationContext::Prefix::IncludePrefix:
                return ns.toStringWithTenantId();
            default:
                MONGO_UNREACHABLE;
        }
    }

    switch (context.getPrefix()) {
        case SerializationContext::Prefix::ExcludePrefix:
            return ns.toString();
        case SerializationContext::Prefix::Default:
            // fallthrough
        case SerializationContext::Prefix::IncludePrefix:
            return ns.toStringWithTenantId();
        default:
            MONGO_UNREACHABLE;
    }
}

NamespaceString NamespaceStringUtil::deserialize(boost::optional<TenantId> tenantId,
                                                 StringData ns,
                                                 const SerializationContext& context) {
    if (ns.empty()) {
        return NamespaceString();
    }

    if (!gMultitenancySupport) {
        massert(6972102,
                str::stream() << "TenantId must not be set, but it is: " << tenantId->toString(),
                tenantId == boost::none);
        return NamespaceString(boost::none, ns);
    }

    // TODO SERVER-74284: uncomment to redirect command-sepcific deserialization requests
    // if (context.getSource() == SerializationContext::Source::Command &&
    //     context.getCallerType() == SerializationContext::CallerType::Request)
    //     return deserializeForCommands(std::move(tenantId), ns, context);

    // if we're not deserializing a Command Request, use the default deserializing rules
    return deserializeForStorage(std::move(tenantId), ns, context);
}

NamespaceString NamespaceStringUtil::deserializeForStorage(boost::optional<TenantId> tenantId,
                                                           StringData ns,
                                                           const SerializationContext& context) {
    if (gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility)) {
        StringData dbName = ns.substr(0, ns.find('.'));
        if (!(dbName == DatabaseName::kAdmin.db()) && !(dbName == DatabaseName::kLocal.db()) &&
            !(dbName == DatabaseName::kConfig.db())) {
            massert(6972100,
                    str::stream() << "TenantId must be set on nss " << ns,
                    tenantId != boost::none);
        }
        return NamespaceString(std::move(tenantId), ns);
    }

    auto nss = NamespaceString::parseFromStringExpectTenantIdInMultitenancyMode(ns);
    // TenantId could be prefixed, or passed in separately (or both) and namespace is always
    // constructed with the tenantId separately.
    if (tenantId != boost::none) {
        if (!nss.tenantId()) {
            return NamespaceString(std::move(tenantId), ns);
        }
        massert(6972101,
                str::stream() << "TenantId must match the db prefix tenantId: "
                              << tenantId->toString() << " prefix " << nss.tenantId()->toString(),
                tenantId == nss.tenantId());
    }

    return nss;
}

NamespaceString NamespaceStringUtil::deserializeForCommands(boost::optional<TenantId> tenantId,
                                                            StringData ns,
                                                            const SerializationContext& context) {
    // we only get here if we are processing a Command Request.  We disregard the feature flag in
    // this case, essentially letting the request dictate the state of the feature.

    if (tenantId != boost::none) {
        switch (context.getPrefix()) {
            case SerializationContext::Prefix::ExcludePrefix:
                // fallthrough
            case SerializationContext::Prefix::Default:
                return NamespaceString(std::move(tenantId), ns);
            case SerializationContext::Prefix::IncludePrefix: {
                auto nss = NamespaceString::parseFromStringExpectTenantIdInMultitenancyMode(ns);
                massert(8423385,
                        str::stream() << "TenantId from $tenant or security token present as '"
                                      << tenantId->toString()
                                      << "' with expectPrefix field set but without a prefix set",
                        nss.tenantId());
                massert(
                    8423381,
                    str::stream()
                        << "TenantId from $tenant or security token must match prefixed tenantId: "
                        << tenantId->toString() << " prefix " << nss.tenantId()->toString(),
                    tenantId.value() == nss.tenantId());
                return nss;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    auto nss = NamespaceString::parseFromStringExpectTenantIdInMultitenancyMode(ns);
    if ((nss.dbName() != DatabaseName::kAdmin) && (nss.dbName() != DatabaseName::kLocal) &&
        (nss.dbName() != DatabaseName::kConfig)) {
        massert(8423387,
                str::stream() << "TenantId must be set on nss " << ns,
                nss.tenantId() != boost::none);
    }

    return nss;
}

NamespaceString NamespaceStringUtil::parseNamespaceFromRequest(
    const boost::optional<TenantId>& tenantId, StringData ns) {
    return deserialize(tenantId, ns);
}

NamespaceString NamespaceStringUtil::parseNamespaceFromRequest(
    const boost::optional<TenantId>& tenantId, StringData db, StringData coll) {
    if (coll.empty())
        return deserialize(tenantId, db);

    uassert(ErrorCodes::InvalidNamespace,
            "Collection names cannot start with '.': " + coll,
            coll[0] != '.');

    return deserialize(tenantId, str::stream() << db << "." << coll);
}

NamespaceString NamespaceStringUtil::parseNamespaceFromRequest(const DatabaseName& dbName,
                                                               StringData coll) {
    if (coll.empty()) {
        return NamespaceString(dbName);
    }

    uassert(ErrorCodes::InvalidNamespace,
            "Collection names cannot start with '.': " + coll,
            coll[0] != '.');

    return deserialize(dbName.tenantId(), str::stream() << dbName.db() << "." << coll);
}

NamespaceString NamespaceStringUtil::parseNamespaceFromDoc(
    const boost::optional<TenantId>& tenantId, StringData ns) {
    return deserialize(tenantId, ns);
}

NamespaceString NamespaceStringUtil::parseNamespaceFromDoc(
    const boost::optional<TenantId>& tenantId, StringData db, StringData coll) {
    if (coll.empty())
        return deserialize(tenantId, db);

    uassert(ErrorCodes::InvalidNamespace,
            "Collection names cannot start with '.': " + coll,
            coll[0] != '.');

    return deserialize(tenantId, str::stream() << db << "." << coll);
}

NamespaceString NamespaceStringUtil::parseNamespaceFromDoc(const DatabaseName& dbName,
                                                           StringData coll) {
    if (coll.empty())
        return NamespaceString(dbName);

    uassert(ErrorCodes::InvalidNamespace,
            "Collection names cannot start with '.': " + coll,
            coll[0] != '.');

    return deserialize(dbName.tenantId(), str::stream() << dbName.db() << "." << coll);
}

NamespaceString NamespaceStringUtil::parseNamespaceFromResponse(const DatabaseName& dbName,
                                                                StringData coll) {
    return parseNamespaceFromDoc(dbName, coll);
}

}  // namespace mongo
