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
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


// Failpoint which causes to hang "listIndexes" cmd after acquiring the DB lock.
MONGO_FAIL_POINT_DEFINE(hangBeforeListIndexes);

namespace mongo {

StatusWith<std::list<BSONObj>> listIndexes(OperationContext* opCtx,
                                           const NamespaceStringOrUUID& ns,
                                           ListIndexesInclude additionalInclude) {
    AutoGetCollectionForReadCommandMaybeLockFree collection(opCtx, ns);
    auto nss = collection.getNss();
    if (!collection) {
        return StatusWith<std::list<BSONObj>>(ErrorCodes::NamespaceNotFound,
                                              str::stream() << "ns does not exist: "
                                                            << collection.getNss().ns());
    }
    return StatusWith<std::list<BSONObj>>(
        listIndexesInLock(opCtx, collection.getCollection(), nss, additionalInclude));
}

std::list<BSONObj> listIndexesInLock(OperationContext* opCtx,
                                     const CollectionPtr& collection,
                                     const NamespaceString& nss,
                                     ListIndexesInclude additionalInclude) {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangBeforeListIndexes, opCtx, "hangBeforeListIndexes", []() {}, nss);

    std::vector<std::string> indexNames;
    std::list<BSONObj> indexSpecs;
    collection->getAllIndexes(&indexNames);

    if (collection->isClustered() && !collection->ns().isTimeseriesBucketsCollection()) {
        BSONObj collation;
        if (auto collator = collection->getDefaultCollator()) {
            collation = collator->getSpec().toBSON();
        }
        auto clusteredSpec = clustered_util::formatClusterKeyForListIndexes(
            collection->getClusteredInfo().value(), collation);
        if (additionalInclude == ListIndexesInclude::IndexBuildInfo) {
            indexSpecs.push_back(BSON("spec"_sd << clusteredSpec));
        } else {
            indexSpecs.push_back(clusteredSpec);
        }
    }
    for (size_t i = 0; i < indexNames.size(); i++) {
        auto spec = collection->getIndexSpec(indexNames[i]);
        auto durableBuildUUID = collection->getIndexBuildUUID(indexNames[i]);
        // The durable catalog will not have a build UUID for the given index name if it was
        // not being built with two-phase -- in this case we have no relevant index build info
        bool inProgressInformationExists =
            !collection->isIndexReady(indexNames[i]) && durableBuildUUID;
        switch (additionalInclude) {
            case ListIndexesInclude::Nothing:
                indexSpecs.push_back(spec);
                break;
            case ListIndexesInclude::BuildUUID:
                if (inProgressInformationExists) {
                    indexSpecs.push_back(
                        BSON("spec"_sd << spec << "buildUUID"_sd << *durableBuildUUID));
                } else {
                    indexSpecs.push_back(spec);
                }
                break;
            case ListIndexesInclude::IndexBuildInfo:
                if (inProgressInformationExists) {
                    // Constructs a sub-document "indexBuildInfo" in the following
                    // format with sample values:
                    //
                    // indexBuildInfo: {
                    //     buildUUID: UUID("00836550-d10e-4ec8-84df-cb5166bc085b"),
                    //     method: "Hybrid",
                    //     phase: 1,
                    //     phaseStr: "collection scan",
                    //     opid: 654,
                    //     resumable: true,
                    //     replicationState: {
                    //         state: "In progress"
                    //     }
                    // }
                    //
                    // The information here is gathered by querying the various index build
                    // classes accessible through the IndexBuildCoordinator interface. The
                    // example above is intended to provide a general idea of the information
                    // gathered for an in-progress index build and is subject to change.

                    BSONObjBuilder builder;
                    durableBuildUUID->appendToBuilder(&builder, "buildUUID"_sd);
                    IndexBuildsCoordinator::get(opCtx)->appendBuildInfo(*durableBuildUUID,
                                                                        &builder);
                    indexSpecs.push_back(
                        BSON("spec"_sd << spec << "indexBuildInfo"_sd << builder.obj()));
                } else {
                    indexSpecs.push_back(BSON("spec"_sd << spec));
                }
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }
    return indexSpecs;
}
std::list<BSONObj> listIndexesEmptyListIfMissing(OperationContext* opCtx,
                                                 const NamespaceStringOrUUID& nss,
                                                 ListIndexesInclude additionalInclude) {
    auto listStatus = listIndexes(opCtx, nss, additionalInclude);
    return listStatus.isOK() ? listStatus.getValue() : std::list<BSONObj>();
}
}  // namespace mongo
