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

ChangeStream::ChangeStream(ChangeStreamReadMode mode,
                           ChangeStreamType type,
                           boost::optional<NamespaceString> nss)
    : _mode(mode), _type(type), _nss(std::move(nss)) {

    if (_type == ChangeStreamType::kAllDatabases) {
        tassert(10656200, "NSS for all-databases change stream must be empty", !_nss.has_value());
    } else {
        tassert(10656201,
                "NSS for collection- or database-level change stream must not be empty",
                _nss.has_value());

        tassert(
            10656202,
            "NSS for collection- or database-level change stream must have the right granularity",
            (_type == ChangeStreamType::kDatabase) == _nss->isDbOnly());
    }
}

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
    const auto& nss = expCtx->getNamespaceString();

    return ChangeStream(fromIgnoreRemovedShardsParameter(static_cast<bool>(
                            expCtx->getChangeStreamSpec()->getIgnoreRemovedShards())),
                        getChangeStreamType(nss),
                        nss);
}

}  // namespace mongo
