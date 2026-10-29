// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/shard_role/shard_catalog/collection_record_store_options.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {
RecordStore::Options getRecordStoreOptions(const NamespaceString& nss,
                                           const CollectionOptions& collectionOptions,
                                           bool recordIdsReplicated) {
    RecordStore::Options recordStoreOptions;

    bool isClustered = collectionOptions.clusteredIndex.has_value();
    recordStoreOptions.keyFormat = isClustered ? KeyFormat::String : KeyFormat::Long;

    // Overwrites are disallowed for clustered collections and collections with replicated record
    // IDs to guarantee record uniqueness and prevent accidental overwrites of existing records.
    recordStoreOptions.allowOverwrite = !(isClustered || recordIdsReplicated);

    recordStoreOptions.isCapped = collectionOptions.capped;

    recordStoreOptions.isOplog = nss.isOplog();
    if (recordStoreOptions.isOplog) {
        // Only relevant for specialized oplog handling.
        recordStoreOptions.oplogMaxSize = collectionOptions.cappedSize;
    }

    bool isTimeseries = collectionOptions.timeseries.has_value();
    if (isTimeseries) {
        recordStoreOptions.customBlockCompressor =
            std::string{kDefaultTimeseriesCollectionCompressor};
        recordStoreOptions.forceUpdateWithFullDocument = isTimeseries;
    }

    recordStoreOptions.storageEngineCollectionOptions = collectionOptions.storageEngine;

    return recordStoreOptions;
}

}  // namespace mongo
