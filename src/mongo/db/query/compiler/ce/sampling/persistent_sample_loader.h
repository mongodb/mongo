// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <string>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::ce {
using namespace std::literals::string_view_literals;

inline constexpr std::string_view kSamplesCollectionName = "system.stats.samples"sv;
inline constexpr int kPersistentSampleSchemaVersion = 1;

/**
 * Serializes a sample into the minimum number of page documents such that each page stays under the
 * BSON size limit.
 *
 * Always returns at least one page (empty if `sample` is empty). Any single document too large to
 * fit on a page on its own is discarded. Errors if the discarded documents exceed the maximum
 * discardable fraction of the sample.
 */
std::vector<BSONObj> makePersistentSamplePageDocs(const UUID& collectionUuid,
                                                  SamplingTechniqueEnum method,
                                                  size_t sampleSize,
                                                  boost::optional<int> numChunks,
                                                  const std::vector<BSONObj>& sample,
                                                  Date_t createdAt);

/**
 * Builds the `_id` object for a persisted sample document
 *
 * Exposed so that both the read path (PersistentSampleLoader) and the write path (analyze command)
 * produce identical keys.
 */
BSONObj makePersistentSampleIdObj(const UUID& collectionUuid,
                                  SamplingTechniqueEnum method,
                                  size_t sampleSize,
                                  boost::optional<int> numChunks,
                                  int pageNo = 0);

/**
 * Returns the dotted path for a given sub-field of the `_id` object of a persisted sample document.
 */
inline std::string persistentSampleIdField(std::string_view subField) {
    return str::stream() << PersistentSampleDoc::k_idFieldName << "." << subField;
}

/**
 * Builds a filter matching all pages of a persisted sample with the given identity values.
 */
BSONObj makePersistentSampleAllPagesLookupFilter(const UUID& collectionUuid,
                                                 SamplingTechniqueEnum method,
                                                 size_t sampleSize,
                                                 boost::optional<int> numChunks);

StatusWith<PersistentSampleDoc> parsePersistentSample(const BSONObj& doc);

/**
 * Reassembles a full persistent sample from individual page documents.

 * Possible error codes:
 * - `NoSuchKey` if `pages` is empty.
 * - `UnsupportedFormat` if any page/the group of pages is malformed
 */
StatusWith<PersistentSampleDoc> reassemblePersistentSample(std::vector<BSONObj> pages);

/**
 * A loaded persistent sample, together with facts about the load that produced it.
 */
struct LoadedPersistentSample {
    PersistentSampleDoc sample;
    // Number of page documents scanned to reassemble `sample`.
    size_t pagesRead = 0;
};

/**
 * This class coordinates the loading of persisted samples.
 */
class PersistentSampleLoader {
public:
    /**
     * Looks up the persistent sample matching the given identity fields.
     *
     * Possible error codes:
     * - `NoSuchKey` if no document matches
     * - `UnsupportedFormat` if the document is found but malformed.
     */
    StatusWith<LoadedPersistentSample> tryLoad(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               const UUID& collectionUuid,
                                               SamplingTechniqueEnum method,
                                               size_t sampleSize,
                                               boost::optional<int> numChunks) const;
};

}  // namespace mongo::ce
