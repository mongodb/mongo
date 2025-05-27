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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/storage_stats_spec_gen.h"
#include "mongo/util/serialization_context.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

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
