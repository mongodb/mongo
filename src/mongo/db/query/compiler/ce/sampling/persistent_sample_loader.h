// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/util/uuid.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo::ce {
using namespace std::literals::string_view_literals;

inline constexpr std::string_view kSamplesCollectionName = "system.stats.samples"sv;
inline constexpr int kPersistentSampleSchemaVersion = 1;

/**
 * Builds the `_id` object for a persisted sample document
 *
 * Exposed so that both the read path (PersistentSampleLoader) and the write path (analyze command)
 * produce identical keys.
 */
BSONObj makePersistentSampleIdObj(const UUID& collectionUuid,
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
