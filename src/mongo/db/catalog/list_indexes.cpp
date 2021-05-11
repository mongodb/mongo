/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/list_indexes.h"

#include <list>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/uuid.h"

// Failpoint which causes to hang "listIndexes" cmd after acquiring the DB lock.
MONGO_FAIL_POINT_DEFINE(hangBeforeListIndexes);

namespace mongo {

StatusWith<std::list<BSONObj>> listIndexes(OperationContext* opCtx,
                                           const NamespaceStringOrUUID& ns,
                                           boost::optional<bool> includeBuildUUIDs) {
    AutoGetCollectionForReadCommandMaybeLockFree collection(opCtx, ns);
    auto nss = collection.getNss();
    if (!collection) {
        return StatusWith<std::list<BSONObj>>(ErrorCodes::NamespaceNotFound,
                                              str::stream() << "ns does not exist: "
                                                            << collection.getNss().ns());
    }
    return StatusWith<std::list<BSONObj>>(
        listIndexesInLock(opCtx, collection.getCollection(), nss, includeBuildUUIDs));
}

std::list<BSONObj> listIndexesInLock(OperationContext* opCtx,
                                     const CollectionPtr& collection,
                                     const NamespaceString& nss,
                                     boost::optional<bool> includeBuildUUIDs) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IS));

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangBeforeListIndexes, opCtx, "hangBeforeListIndexes", []() {}, nss);

    return writeConflictRetry(opCtx, "listIndexes", nss.ns(), [&] {
        std::vector<std::string> indexNames;
        std::list<BSONObj> indexSpecs;
        collection->getAllIndexes(&indexNames);

        for (size_t i = 0; i < indexNames.size(); i++) {
            if (!includeBuildUUIDs.value_or(false) || collection->isIndexReady(indexNames[i])) {
                indexSpecs.push_back(collection->getIndexSpec(indexNames[i]));
                continue;
            }
            // The durable catalog will not have a build UUID for the given index name if it was
            // not being built with two-phase.
            const auto durableBuildUUID = collection->getIndexBuildUUID(indexNames[i]);
            if (!durableBuildUUID) {
                indexSpecs.push_back(collection->getIndexSpec(indexNames[i]));
                continue;
            }

            BSONObjBuilder builder;
            builder.append("spec"_sd, collection->getIndexSpec(indexNames[i]));
            durableBuildUUID->appendToBuilder(&builder, "buildUUID"_sd);
            indexSpecs.push_back(builder.obj());
        }
        return indexSpecs;
    });
}
std::list<BSONObj> listIndexesEmptyListIfMissing(OperationContext* opCtx,
                                                 const NamespaceStringOrUUID& nss,
                                                 boost::optional<bool> includeBuildUUIDs) {
    auto listStatus = listIndexes(opCtx, nss, includeBuildUUIDs);
    return listStatus.isOK() ? listStatus.getValue() : std::list<BSONObj>();
}
}  // namespace mongo
