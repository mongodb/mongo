// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/shard_role/shard_catalog/list_indexes.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


// Failpoint which causes to hang "listIndexes" cmd after acquiring the DB lock.
MONGO_FAIL_POINT_DEFINE(hangBeforeListIndexes);

namespace mongo {
using namespace std::literals::string_view_literals;

std::vector<BSONObj> listIndexesInLock(OperationContext* opCtx,
                                       const CollectionAcquisition& collectionAcquisition,
                                       ListIndexesInclude additionalInclude,
                                       bool isRawDataRequest) {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangBeforeListIndexes,
        opCtx,
        "hangBeforeListIndexes",
        []() {},
        collectionAcquisition.nss());

    const auto& collection = collectionAcquisition.getCollectionPtr();

    std::vector<std::string> indexNames;
    std::vector<BSONObj> indexSpecs;
    collection->getAllIndexes(&indexNames);

    const bool convertBucketsIndexesToTimeseriesIndexes =
        collection->isTimeseriesCollection() && !isRawDataRequest;

    const bool expandSimpleCollation =
        feature_flags::gFeatureFlagListIndexesAlwaysIncludesSimpleCollation
            .isEnabledUseLastLTSFCVWhenUninitialized(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

    if (collection->isClustered() && !collection->isTimeseriesCollection()) {
        BSONObj collation;
        if (auto collator = collection->getDefaultCollator()) {
            collation = collator->getSpec().toBSON();
        }
        auto clusteredSpec = clustered_util::formatClusterKeyForListIndexes(
            collection->getClusteredInfo().value(),
            collation,
            collection->getCollectionOptions().expireAfterSeconds,
            expandSimpleCollation);
        if (additionalInclude == ListIndexesInclude::kIndexBuildInfo) {
            indexSpecs.push_back(BSON("spec"sv << clusteredSpec));
        } else {
            indexSpecs.push_back(clusteredSpec);
        }
    }
    for (size_t i = 0; i < indexNames.size(); i++) {
        auto spec = collection->getIndexSpec(indexNames[i], expandSimpleCollation);
        if (convertBucketsIndexesToTimeseriesIndexes) {
            auto timeseriesSpec = timeseries::createTimeseriesIndexFromBucketsIndex(
                *collection->getTimeseriesOptions(), spec);
            if (!timeseriesSpec) {
                // This buckets index does not have an equivalent timeseries index (it may have been
                // created directly over the bucket fields using rawData), so omit it from the list.
                continue;
            }
            spec = *timeseriesSpec;
        }
        auto durableBuildUUID = collection->getIndexBuildUUID(indexNames[i]);
        // The durable catalog will not have a build UUID for the given index name if it was
        // not being built with two-phase -- in this case we have no relevant index build info
        bool inProgressInformationExists =
            !collection->isIndexReady(indexNames[i]) && durableBuildUUID;
        switch (additionalInclude) {
            case ListIndexesInclude::kNothing:
                indexSpecs.push_back(spec);
                break;
            case ListIndexesInclude::kBuildUUID:
                if (inProgressInformationExists) {
                    indexSpecs.push_back(
                        BSON("spec"sv << spec << "buildUUID"sv << *durableBuildUUID));
                } else {
                    indexSpecs.push_back(spec);
                }
                break;
            case ListIndexesInclude::kIndexBuildInfo:
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
                    durableBuildUUID->appendToBuilder(&builder, "buildUUID"sv);
                    IndexBuildsCoordinator::get(opCtx)->appendBuildInfo(*durableBuildUUID,
                                                                        &builder);
                    indexSpecs.push_back(
                        BSON("spec"sv << spec << "indexBuildInfo"sv << builder.obj()));
                } else {
                    indexSpecs.push_back(BSON("spec"sv << spec));
                }
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(10083506);
        }
    }
    return indexSpecs;
}
std::vector<BSONObj> listIndexesEmptyListIfMissing(OperationContext* opCtx,
                                                   const NamespaceStringOrUUID& nss,
                                                   ListIndexesInclude additionalInclude,
                                                   bool isRawDataRequest) {
    // TODO SERVER-104759: switch to normal acquireCollection once 9.0 becomes last LTS
    auto [collection, _] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, nss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);
    if (!collection.exists()) {
        return {};
    }

    return listIndexesInLock(opCtx, collection, additionalInclude, isRawDataRequest);
}
}  // namespace mongo
