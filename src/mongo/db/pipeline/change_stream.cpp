/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/change_stream.h"

#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
boost::optional<NamespaceString> convertedNss(ChangeStreamType type,
                                              const boost::optional<NamespaceString>& nss) {
    if (type == ChangeStreamType::kAllDatabases) {
        // Cluster-wide change streams have to be opened on the "admin" database.
        // Convert this to a Namespace without a value.
        tassert(10656200,
                "NSS for all-databases change stream must be empty",
                !nss.has_value() || nss->isAdminDB());
        return {};
    }

    tassert(10656201,
            "NSS for collection- or database-level change stream must not be empty",
            nss.has_value());

    if (type == ChangeStreamType::kDatabase && nss->isCollectionlessAggregateNS()) {
        // Convert 'dbName.$cmd.aggregate' namespace to just 'dbName'.
        return NamespaceString(nss->dbName());
    }

    tassert(10656202,
            "NSS for collection- or database-level change stream must have the right granularity",
            (type == ChangeStreamType::kDatabase) == nss->isDbOnly());

    // Return original namespace as is.
    return nss;
}

}  // namespace
ChangeStream::ChangeStream(ChangeStreamReadMode mode,
                           ChangeStreamType type,
                           const boost::optional<NamespaceString>& nss)
    : _mode(mode), _type(type), _nss(convertedNss(type, nss)) {}

ChangeStreamReadMode ChangeStream::getReadMode() const {
    return _mode;
}

ChangeStreamType ChangeStream::getChangeStreamType() const {
    return _type;
}

boost::optional<NamespaceString> ChangeStream::getNamespace() const {
    return _nss;
}

ChangeStreamType ChangeStream::getChangeStreamType(const NamespaceString& nss) {
    // If we have been permitted to run on admin, 'allChangesForCluster' must be true.
    return (nss.isAdminDB() ? ChangeStreamType::kAllDatabases
                            : (nss.isCollectionlessAggregateNS() ? ChangeStreamType::kDatabase
                                                                 : ChangeStreamType::kCollection));
}

ChangeStream ChangeStream::buildFromExpressionContext(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    tassert(10743905,
            "expecting changeStreamSpec to be present in ExpressionContext",
            expCtx->getChangeStreamSpec().has_value());

    const auto& nss = expCtx->getNamespaceString();

    return ChangeStream(fromIgnoreRemovedShardsParameter(static_cast<bool>(
                            expCtx->getChangeStreamSpec()->getIgnoreRemovedShards())),
                        getChangeStreamType(nss),
                        nss);
}

}  // namespace mongo
