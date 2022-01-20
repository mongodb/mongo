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

#include "mongo/db/tenant_namespace.h"

#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/server_feature_flags_gen.h"

namespace mongo {

TenantNamespace::TenantNamespace(boost::optional<mongo::TenantId> tenantId, NamespaceString nss) {
    // TODO SERVER-62114 Check instead if gMultitenancySupport is enabled.
    if (gFeatureFlagRequireTenantID.isEnabledAndIgnoreFCV())
        invariant(tenantId);

    _tenantId = tenantId;
    _nss = nss;
    _tenantNsStr = _tenantId ? boost::make_optional(_tenantId->toString() + "_" + _nss.toString())
                             : boost::none;
}

TenantNamespace TenantNamespace::parseTenantNamespaceFromDisk(StringData ns) {
    if (!gMultitenancySupport) {
        return TenantNamespace(boost::none, NamespaceString(ns));
    }

    auto tenantDelim = ns.find('_');
    if (tenantDelim == std::string::npos)
        return TenantNamespace(boost::none, NamespaceString(ns));

    const TenantId tenantId(OID(ns.substr(0, tenantDelim)));
    auto nss = NamespaceString(ns.substr(tenantDelim + 1, ns.size() - 1 - tenantDelim));
    return TenantNamespace(tenantId, nss);
}

}  // namespace mongo
