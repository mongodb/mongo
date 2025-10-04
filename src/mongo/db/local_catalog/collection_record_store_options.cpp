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
#include "mongo/db/local_catalog/collection_record_store_options.h"

#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {
RecordStore::Options getRecordStoreOptions(const NamespaceString& nss,
                                           const CollectionOptions& collectionOptions) {
    RecordStore::Options recordStoreOptions;

    bool isClustered = collectionOptions.clusteredIndex.has_value();
    recordStoreOptions.keyFormat = isClustered ? KeyFormat::String : KeyFormat::Long;
    recordStoreOptions.allowOverwrite = isClustered ? false : true;

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
