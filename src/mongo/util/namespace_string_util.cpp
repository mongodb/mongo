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

std::string NamespaceStringUtil::serialize(const NamespaceString& ns) {
    if (gMultitenancySupport) {
        if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
            gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility)) {
            return ns.toString();
        }
        return ns.toStringWithTenantId();
    }
    return ns.toString();
}

NamespaceString NamespaceStringUtil::deserialize(boost::optional<TenantId> tenantId,
                                                 StringData ns) {
    if (ns.empty()) {
        return NamespaceString();
    }
    if (gMultitenancySupport) {
        if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
            gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility)) {
            // TODO SERVER-69721 : Enable the invariant.
            // TODO SERVER-62491: Invariant for all databases. Remove the invariant bypass for
            // admin, local, config dbs.
            // StringData dbName = ns.substr(0, ns.find('.'));
            // if (!(dbName == NamespaceString::kAdminDb) && !(dbName == NamespaceString::kLocalDb)
            // &&
            //     !(dbName == NamespaceString::kConfigDb)) {
            //          invariant(tenantId != boost::none);
            // }
            return NamespaceString(std::move(tenantId), ns);
        }
        auto nss = NamespaceString::parseFromStringExpectTenantIdInMultitenancyMode(ns);
        // TenantId could be prefixed, or passed in separately (or both) and namespace is always
        // constructed with the tenantId separately.
        if (tenantId != boost::none) {
            if (!nss.tenantId()) {
                return NamespaceString(std::move(tenantId), ns);
            }
            invariant(tenantId == nss.tenantId());
        }
        return nss;
    }
    invariant(tenantId == boost::none);
    return NamespaceString(boost::none, ns);
}

}  // namespace mongo
