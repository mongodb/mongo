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

#include "mongo/db/local_catalog/collection_uuid_mismatch.h"

#include "mongo/base/string_data.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch_info.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/type_traits/decay.hpp>

namespace mongo {

void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const Collection* coll,
                                 const boost::optional<UUID>& uuid) {
    checkCollectionUUIDMismatch(opCtx, *CollectionCatalog::get(opCtx), ns, coll, uuid);
}

void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const CollectionPtr& coll,
                                 const boost::optional<UUID>& uuid) {
    checkCollectionUUIDMismatch(opCtx, *CollectionCatalog::get(opCtx), ns, coll.get(), uuid);
}

void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const CollectionCatalog& catalog,
                                 const NamespaceString& ns,
                                 const CollectionPtr& coll,
                                 const boost::optional<UUID>& uuid) {
    checkCollectionUUIDMismatch(opCtx, catalog, ns, coll.get(), uuid);
}

void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const CollectionCatalog& catalog,
                                 const NamespaceString& ns,
                                 const Collection* coll,
                                 const boost::optional<UUID>& uuid) {
    if (!uuid) {
        return;
    }
    // TODO SERVER-101784 Remove the code below once 9.0 becomes LTS and legacy time-series
    // collection are no more.
    auto nsForLogging = (ns.isTimeseriesBucketsCollection()) ? ns.getTimeseriesViewNamespace() : ns;
    auto actualNamespace = catalog.lookupNSSByUUID(opCtx, *uuid);
    uassert(
        (CollectionUUIDMismatchInfo{ns.dbName(),
                                    *uuid,
                                    std::string{nsForLogging.coll()},
                                    actualNamespace && actualNamespace->isEqualDb(ns)
                                        ? boost::make_optional(std::string{actualNamespace->coll()})
                                        : boost::none}),
        "Collection UUID does not match that specified",
        coll && coll->uuid() == *uuid);
}

}  // namespace mongo
