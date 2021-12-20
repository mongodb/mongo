/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include <iosfwd>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/logv2/log_attr.h"

namespace mongo {

class TenantNamespace {
public:
    /**
     * Constructs an empty TenantNamespace.
     */
    TenantNamespace() : _tenantId(boost::none), _nss(), _tenantNsStr() {}

    /**
     * Constructs a TenantNamespace from the given tenantId and NamespaceString.
     *
     * If featureFlagRequireTenantID is set, tenantId is required.
     */
    TenantNamespace(boost::optional<mongo::OID> tenantId, NamespaceString nss) {
        // TODO SERVER-62114 Check instead if gMultitenancySupport is enabled.
        if (gFeatureFlagRequireTenantID.isEnabledAndIgnoreFCV())
            invariant(tenantId);

        _tenantId = tenantId;
        _nss = nss;
        _tenantNsStr = _tenantId
            ? boost::make_optional(_tenantId->toString() + "_" + _nss.toString())
            : boost::none;
    }

    /**
     * Constructs a TenantNamespace from the string "ns". When the server parameter
     * "multitenancySupport” is enabled, the tenantId will be parsed separately from the database
     * name. If it is disabled, the tenantId will be parsed as a prefix of the database name, and
     * the tenantId field will be empty. For example:
     * if “multitenancySupport” is enabled, "tenant1_dbA.collA" will be parsed as:
     * _tenantId = tenant1
     * _nss = NamespaceString(dbA.collA)
     *
     * if “multitenancySupport” is disabled, "tenant1_dbA.collA" will be parsed as:
     * _tenantId = boost::none
     * _nss = NamespaceString(tenant1_dbA.collA), and the _nss,db()
     *
     * This method should only be used when reading a namespace from disk. To construct a
     * TenantNamespace otherwise, use the standard constructor above.
     *
     * If featureFlagRequireTenantID is set, tenantId is required.
     */
    static TenantNamespace parseTenantNamespaceFromDisk(StringData ns) {
        if (!gMultitenancySupport) {
            return TenantNamespace(boost::none, NamespaceString(ns));
        }

        auto tenantDelim = ns.find('_');
        if (tenantDelim == std::string::npos)
            return TenantNamespace(boost::none, NamespaceString(ns));

        auto tenantId = OID(ns.substr(0, tenantDelim));
        auto nss = NamespaceString(ns.substr(tenantDelim + 1, ns.size() - 1 - tenantDelim));
        return TenantNamespace(tenantId, nss);
    }

    boost::optional<mongo::OID> tenantId() const {
        return _tenantId;
    }

    StringData db() const {
        return _nss.db();
    }

    StringData coll() const {
        return _nss.coll();
    }

    NamespaceString getNss() const {
        return _nss;
    }

    std::string toString() const {
        if (_tenantNsStr)
            return *_tenantNsStr;

        invariant(!_tenantId);
        return _nss.ns();
    }

    // Relops among `TenantNamespace`.
    friend bool operator==(const TenantNamespace& a, const TenantNamespace& b) {
        return a.toString() == b.toString();
    }
    friend bool operator!=(const TenantNamespace& a, const TenantNamespace& b) {
        return a.toString() != b.toString();
    }
    friend bool operator<(const TenantNamespace& a, const TenantNamespace& b) {
        return a.toString() < b.toString();
    }
    friend bool operator>(const TenantNamespace& a, const TenantNamespace& b) {
        return a.toString() > b.toString();
    }
    friend bool operator<=(const TenantNamespace& a, const TenantNamespace& b) {
        return a.toString() <= b.toString();
    }
    friend bool operator>=(const TenantNamespace& a, const TenantNamespace& b) {
        return a.toString() >= b.toString();
    }

    template <typename H>
    friend H AbslHashValue(H h, const TenantNamespace& tenantNs) {
        return H::combine(std::move(h), tenantNs.toString());
    }

    friend auto logAttrs(const TenantNamespace& nss) {
        return "tenantNamespace"_attr = nss;
    }

private:
    boost::optional<mongo::OID> _tenantId;
    NamespaceString _nss;
    boost::optional<std::string> _tenantNsStr;  // Only set if _tenantId exists
};

}  // namespace mongo
