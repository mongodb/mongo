/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/s/balancer_stats_registry.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/timeseries_stats.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/stdx/unordered_map.h"

#include "mongo/db/stats/storage_stats.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC


namespace mongo {
namespace {

enum class StorageStatsGroups {
    kRecordStatsField,
    kRecordStoreField,
    kInProgressIndexesField,
    kTotalSizeField,
};

// Mapping possible 'filterObj' fields and their corresponding output groups. For a whole group to
// be part of the output, it is only necessary that one field it contains is included in the filter.
const stdx::unordered_map<std::string, StorageStatsGroups> _mapStorageStatsFieldsToGroup = {
    {"numOrphanDocs", StorageStatsGroups::kRecordStatsField},
    {"size", StorageStatsGroups::kRecordStatsField},
    {"timeseries", StorageStatsGroups::kRecordStatsField},
    {"count", StorageStatsGroups::kRecordStatsField},
    {"avgObjSize", StorageStatsGroups::kRecordStatsField},
    {"storageSize", StorageStatsGroups::kRecordStoreField},
    {"freeStorageSize", StorageStatsGroups::kRecordStoreField},
    {"capped", StorageStatsGroups::kRecordStoreField},
    {"max", StorageStatsGroups::kRecordStoreField},
    {"maxSize", StorageStatsGroups::kRecordStoreField},
    {"nindexes", StorageStatsGroups::kInProgressIndexesField},
    {"indexDetails", StorageStatsGroups::kInProgressIndexesField},
    {"indexBuilds", StorageStatsGroups::kInProgressIndexesField},
    {"totalIndexSize", StorageStatsGroups::kInProgressIndexesField},
    {"indexSizes", StorageStatsGroups::kInProgressIndexesField},
    {"totalSize", StorageStatsGroups::kTotalSizeField},
    {"scaleFactor", StorageStatsGroups::kTotalSizeField}};

// Append to 'result' the stats related to record stats.
void _appendRecordStats(OperationContext* opCtx,
                        const CollectionPtr& collection,
                        const NamespaceString& collNss,
                        bool isNamespaceAlwaysUnsharded,
                        int scale,
                        bool isTimeseries,
                        BSONObjBuilder* result) {
    static constexpr auto kOrphanCountField = "numOrphanDocs"_sd;
    long long size = collection->dataSize(opCtx) / scale;
    result->appendNumber("size", size);

    long long numRecords = collection->numRecords(opCtx);
    if (isTimeseries) {
        BSONObjBuilder bob(result->subobjStart("timeseries"));
        bob.append("bucketsNs", NamespaceStringUtil::serialize(collNss));
        bob.appendNumber("bucketCount", numRecords);
        if (numRecords) {
            bob.append("avgBucketSize", collection->averageObjectSize(opCtx));
        }
        auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);
        timeseries::bucket_catalog::appendExecutionStats(
            bucketCatalog, collNss.getTimeseriesViewNamespace(), bob);
        TimeseriesStats::get(collection.get()).append(&bob);
    } else {
        result->appendNumber("count", numRecords);
        if (numRecords) {
            result->append("avgObjSize", collection->averageObjectSize(opCtx));
        }
    }

    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
        !isNamespaceAlwaysUnsharded) {
        result->appendNumber(
            kOrphanCountField,
            BalancerStatsRegistry::get(opCtx)->getCollNumOrphanDocsFromDiskIfNeeded(
                opCtx, collection->uuid()));
    } else {
        result->appendNumber(kOrphanCountField, 0);
    }
}

/**
 * The collection stats is in the shape of
 * {
 *   wiredTiger : {
 *     uri: "..."
 *     ...
 *   }
 * }
 * Returns a document only if "uri" is in the original document
 */
BSONObj filterQECustomStats(BSONObj obj) {
    if (obj.firstElementFieldName() == "wiredTiger"_sd) {
        auto uriElement = obj.firstElement()["uri"_sd];
        if (uriElement.ok()) {
            return BSON("wiredTiger"_sd << BSON("uri"_sd << uriElement));
        }
    }

    return BSONObj();
}

/**
 * The index stats is in the shape of
 * {
 *   uri: "..."
 *   ...
 * }
 *
 * Returns a document only if "uri" is in the original document
 */
BSONObj filterQEIndexStats(BSONObj obj) {
    auto uriElement = obj["uri"_sd];
    if (uriElement.ok()) {
        return BSON("uri"_sd << uriElement);
    }

    return BSONObj();
}

// Append to 'result' the stats related to record store.
void _appendRecordStore(OperationContext* opCtx,
                        const CollectionPtr& collection,
                        bool verbose,
                        int scale,
                        bool numericOnly,
                        BSONObjBuilder* result) {
    const RecordStore* recordStore = collection->getRecordStore();
    auto storageSize =
        static_cast<long long>(recordStore->storageSize(opCtx, result, verbose ? 1 : 0));
    result->appendNumber("storageSize", storageSize / scale);
    result->appendNumber("freeStorageSize",
                         static_cast<long long>(recordStore->freeStorageSize(opCtx)) / scale);

    const bool isCapped = collection->isCapped();
    result->appendBool("capped", isCapped);
    if (isCapped) {
        result->appendNumber("max", collection->getCappedMaxDocs());
        result->appendNumber("maxSize", collection->getCappedMaxSize() / scale);
    }

    bool redactForQE = collection.get()->getCollectionOptions().encryptedFieldConfig ||
        collection.get()->ns().isFLE2StateCollection();
    if (redactForQE) {
        BSONObjBuilder filteredQEBuilder;
        if (numericOnly) {
            recordStore->appendNumericCustomStats(opCtx, &filteredQEBuilder, scale);
        } else {
            recordStore->appendAllCustomStats(opCtx, &filteredQEBuilder, scale);
        }

        result->appendElements(filterQECustomStats(filteredQEBuilder.obj()));

        return;
    }

    if (numericOnly) {
        recordStore->appendNumericCustomStats(opCtx, result, scale);
    } else {
        recordStore->appendAllCustomStats(opCtx, result, scale);
    }
}

// Append to 'result' the stats related to inProgress indexes.
void _appendInProgressIndexesStats(OperationContext* opCtx,
                                   const CollectionPtr& collection,
                                   int scale,
                                   BSONObjBuilder* result) {
    const IndexCatalog* indexCatalog = collection->getIndexCatalog();
    BSONObjBuilder indexDetails;
    std::vector<std::string> indexBuilds;

    bool redactForQE = collection.get()->getCollectionOptions().encryptedFieldConfig ||
        collection.get()->ns().isFLE2StateCollection();

    auto numIndexes = indexCatalog->numIndexesTotal();
    if (collection->isClustered() && !collection->ns().isTimeseriesBucketsCollection()) {
        // There is an implicit 'clustered' index on a clustered collection. Increment the total
        // index count to reflect that.
        numIndexes++;

        BSONObj collation;
        if (auto collator = collection->getDefaultCollator()) {
            collation = collator->getSpec().toBSON();
        }
        auto clusteredSpec = clustered_util::formatClusterKeyForListIndexes(
            collection->getClusteredInfo().value(), collation);
        auto indexSpec = collection->getClusteredInfo()->getIndexSpec();
        auto nameOptional = indexSpec.getName();
        // An index name is always expected.
        invariant(nameOptional);
        indexDetails.append(*nameOptional, clusteredSpec);
    }
    result->append("nindexes", numIndexes);

    auto it = indexCatalog->getIndexIterator(
        opCtx, IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);
    while (it->more()) {
        const IndexCatalogEntry* entry = it->next();
        const IndexDescriptor* descriptor = entry->descriptor();
        const IndexAccessMethod* iam = entry->accessMethod();
        invariant(iam);

        BSONObjBuilder bob;
        if (iam->appendCustomStats(opCtx, &bob, scale)) {
            if (redactForQE) {
                indexDetails.append(descriptor->indexName(), filterQEIndexStats(bob.obj()));
            } else {
                indexDetails.append(descriptor->indexName(), bob.obj());
            }
        }

        // Not all indexes in the collection stats may be visible or consistent with our
        // snapshot. For this reason, it is unsafe to check `isReady` on the entry, which
        // asserts that the index's in-memory state is consistent with our snapshot.
        if (!entry->isPresentInMySnapshot(opCtx)) {
            continue;
        }

        if (!entry->isReadyInMySnapshot(opCtx)) {
            indexBuilds.push_back(descriptor->indexName());
        }
    }

    BSONObjBuilder indexSizes;
    long long indexSize = collection->getIndexSize(opCtx, &indexSizes, scale);

    result->append("indexDetails", indexDetails.obj());
    result->append("indexBuilds", indexBuilds);
    result->appendNumber("totalIndexSize", indexSize / scale);
    result->append("indexSizes", indexSizes.obj());
}

// Append to 'result' the total size and the scale factor.
void _appendTotalSize(OperationContext* opCtx,
                      const CollectionPtr& collection,
                      bool verbose,
                      int scale,
                      BSONObjBuilder* result) {
    const RecordStore* recordStore = collection->getRecordStore();
    auto storageSize =
        static_cast<long long>(recordStore->storageSize(opCtx, result, verbose ? 1 : 0));
    BSONObjBuilder indexSizes;
    long long indexSize = collection->getIndexSize(opCtx, &indexSizes, scale);

    result->appendNumber("totalSize", (storageSize + indexSize) / scale);
    result->append("scaleFactor", scale);
}
}  // namespace

Status appendCollectionStorageStats(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const StorageStatsSpec& storageStatsSpec,
                                    BSONObjBuilder* result,
                                    const boost::optional<BSONObj>& filterObj) {
    auto scale = storageStatsSpec.getScale().value_or(1);
    bool verbose = storageStatsSpec.getVerbose();
    bool waitForLock = storageStatsSpec.getWaitForLock();
    bool numericOnly = storageStatsSpec.getNumericOnly();
    static constexpr auto kStorageStatsField = "storageStats"_sd;

    const auto bucketNss = nss.makeTimeseriesBucketsNamespace();
    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);
    const auto isTimeseries = nss.isTimeseriesBucketsCollection() ||
        catalog->lookupCollectionByNamespace(opCtx, bucketNss);
    const auto collNss =
        (isTimeseries && !nss.isTimeseriesBucketsCollection()) ? std::move(bucketNss) : nss;

    auto failed = [&](const DBException& ex) {
        LOGV2_DEBUG(3088801,
                    2,
                    "Failed to retrieve storage statistics",
                    logAttrs(collNss),
                    "error"_attr = ex);
        return Status::OK();
    };

    boost::optional<AutoGetCollectionForReadCommandMaybeLockFree> autoColl;
    try {
        autoColl.emplace(
            opCtx,
            collNss,
            AutoGetCollection::Options{}.deadline(waitForLock ? Date_t::max() : Date_t::now()));
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>& ex) {
        return failed(ex);
    } catch (const ExceptionFor<ErrorCodes::MaxTimeMSExpired>& ex) {
        return failed(ex);
    }

    const auto& collection = autoColl->getCollection();  // Will be set if present
    if (!collection) {
        result->appendNumber("size", 0);
        result->appendNumber("count", 0);
        result->appendNumber("numOrphanDocs", 0);
        result->appendNumber("storageSize", 0);
        result->append("totalSize", 0);
        result->append("nindexes", 0);
        result->appendNumber("totalIndexSize", 0);
        result->append("indexDetails", BSONObj());
        result->append("indexSizes", BSONObj());
        result->append("scaleFactor", scale);
        return {ErrorCodes::NamespaceNotFound,
                "Collection [" + collNss.toString() + "] not found."};
    }

    // We will parse all 'filterObj' into different groups of data to compute. This groups will be
    // marked and appended to the vector 'groupsToCompute'. In addition, if the filterObj doesn't
    // exist (filterObj == boost::none), we will retrieve all stats for all fields.
    std::vector<StorageStatsGroups> groupsToCompute;
    if (filterObj) {
        // Case where exists a filterObj that specifies one or more groups to compute from the
        // storage stats.
        BSONObj stats = filterObj.get();
        if (stats.hasField(kStorageStatsField)) {
            BSONObj storageStats = stats.getObjectField(kStorageStatsField);
            for (const auto& element : storageStats) {
                if (element.Bool() && _mapStorageStatsFieldsToGroup.count(element.fieldName())) {
                    groupsToCompute.push_back(
                        _mapStorageStatsFieldsToGroup.at(element.fieldName()));
                }
            }
        }
    } else {
        // Case where filterObj doesn't exist. We will append to 'groupsToCompute' all existing
        // groups to retrieve all possible fields.
        groupsToCompute = {StorageStatsGroups::kRecordStatsField,
                           StorageStatsGroups::kRecordStoreField,
                           StorageStatsGroups::kInProgressIndexesField,
                           StorageStatsGroups::kTotalSizeField};
    }

    // Iterate elements from 'groupsToCompute' to compute only the demanded groups of fields.
    for (const auto& group : groupsToCompute) {
        switch (group) {
            case StorageStatsGroups::kRecordStatsField:
                _appendRecordStats(opCtx,
                                   collection,
                                   collNss,
                                   nss.isNamespaceAlwaysUnsharded(),
                                   scale,
                                   isTimeseries,
                                   result);
                break;
            case StorageStatsGroups::kRecordStoreField:
                _appendRecordStore(opCtx, collection, verbose, scale, numericOnly, result);
                break;
            case StorageStatsGroups::kInProgressIndexesField:
                _appendInProgressIndexesStats(opCtx, collection, scale, result);
                break;
            case StorageStatsGroups::kTotalSizeField:
                _appendTotalSize(opCtx, collection, verbose, scale, result);
                break;
        }
    }
    return Status::OK();
}

Status appendCollectionRecordCount(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   BSONObjBuilder* result) {
    AutoGetCollectionForReadCommandMaybeLockFree collection(opCtx, nss);
    if (!collection) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection [" << nss.toString() << "] not found."};
    }

    result->appendNumber("count", static_cast<long long>(collection->numRecords(opCtx)));

    return Status::OK();
}
}  // namespace mongo
