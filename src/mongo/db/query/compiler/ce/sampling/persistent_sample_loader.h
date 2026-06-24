/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/util/uuid.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo::ce {

inline constexpr std::string_view kSamplesCollectionName = "system.stats.samples"_sd;
inline constexpr int kPersistentSampleSchemaVersion = 1;

/**
 * Builds the _id key for a persisted sample document.
 *
 * Format: <UUID>_<method>_<sampleSize>_v<schemaVersion>
 *     or: <UUID>_chunk<numChunks>_<sampleSize>_v<schemaVersion>
 *
 * Exposed so that both the read path (PersistentSampleLoader) and the write path (analyze
 * command) produce identical keys.
 */
std::string buildPersistentSampleId(const UUID& collectionUuid,
                                    SamplingTechniqueEnum method,
                                    size_t sampleSize,
                                    boost::optional<int> numChunks);

StatusWith<PersistentSampleDoc> parsePersistentSample(const BSONObj& doc);

/**
 * This class coordinates the loading of persisted samples from the
 * `<dbName>.system.stats.samples` collection.
 */
class PersistentSampleLoader {
public:
    /**
     * Looks up the persistent sample in `<dbName>.system.stats.samples` matching the given
     * identity fields
     *
     * Possible error codes:
     * - `NoSuchKey` if no document matches
     * - `UnsupportedFormat` if the document is found but malformed.
     */
    StatusWith<PersistentSampleDoc> tryLoad(OperationContext* opCtx,
                                            const DatabaseName& dbName,
                                            const UUID& collectionUuid,
                                            SamplingTechniqueEnum method,
                                            size_t sampleSize,
                                            boost::optional<int> numChunks) const;
};

}  // namespace mongo::ce
