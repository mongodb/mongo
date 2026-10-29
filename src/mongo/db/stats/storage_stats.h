// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/storage_stats_spec_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/serialization_context.h"

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Appends to 'builder' storage related statistics for the collection represented by 'nss'. This
 * method will have different implementations depending of the existence of 'filterObj'.
 *
 * If 'filterObj' doesn't exist (filterObj == boost::none), the method will retrieve all the
 * storage related statistics.
 *
 * For the case that 'filterObj' exists, the method will return filtered stats depending on the
 * fields specified in the filter:
 *      1.- In order to append to 'builder' the RecordStats for the collection, the 'filterObj' must
 * contain at least 1 field from the following list:
 *          - numOrphanDocs
 *          - size
 *          - timeseries
 *          - count
 *          - avgObjSize
 *      2.- In order to append to 'builder' the RecordStore for the collection, the 'filterObj' must
 * contain at least 1 field from the following list:
 *          - storageSize
 *          - freeStorageSize
 *          - capped
 *          - max
 *          - maxSize
 *      3.- In order to append to 'builder' the InProgressIndexesStats for the collection, the
 * 'filterObj' must contain at least 1 field from the following list:
 *          - nindexes
 *          - indexDetails
 *          - indexBuilds
 *          - totalIndexSize
 *          - indexSizes
 *      4.- In order to append to 'builder' the TotalSize for the collection, the 'filterObj' must
 * contain at least 1 field from the following list:
 *          - totalSize
 *          - scaleFactor
 *
 * Params:
 *      - opCtx
 *      - nss: Fully qualified namespace.
 *      - spec: Includes options such as "scale" (default = 1) and "verbose".
 *      - builder out; object the stats will be appended to.
 *      - filterObj: BSONObj to request specific storage stats. If 'filterObj' is 'boost:none', all
 * possible storage stats will be appended to 'builder' parameter. The filterObj must follow this
 * pattern:
 *          filter = {
 *                storageStats : {
 *                     field1ToShow: <bool>,
 *                     field2ToShow: <bool>,
 *                     ...
 *                }
 *          }
 *
 * returns Status, (note "NamespaceNotFound" will fill result with 0-ed stats)
 */
Status appendCollectionStorageStats(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const StorageStatsSpec& spec,
                                    const SerializationContext& serializationCtx,
                                    BSONObjBuilder* builder,
                                    const boost::optional<BSONObj>& filterObj = boost::none);

/**
 * Appends the collection record count to 'builder' for the collection represented by 'nss'.
 */
Status appendCollectionRecordCount(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   BSONObjBuilder* builder);

};  // namespace mongo
