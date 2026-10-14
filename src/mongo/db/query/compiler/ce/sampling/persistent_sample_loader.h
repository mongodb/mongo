// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/db/query/compiler/ce/sampling/sample_loader.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::ce {
using namespace std::literals::string_view_literals;

inline constexpr std::string_view kSamplesCollectionName = "system.stats.samples"sv;
inline constexpr int kPersistentSampleSchemaVersion = 1;

/**
 * Builds the `_id` string for a persisted sample page document:
 *
 *     <collectionUuid>_<schemaVersion>_<samplingMethod>_<sampleSize>[_<numChunks>]_<pageNo>
 *
 * All pages of a given sample share an identical prefix up to (and including) the separator that
 * precedes the page number, so the pages of one sample - and only that sample - form a contiguous
 * run in lexicographic `_id` order. `pageNo` is zero-padded to the width of `sampleSize` so that
 * lexicographic order matches numeric order. This is enough padding because a sample can never
 * have more pages than it has documents, and it can never have more documents than `sampleSize`.
 */
std::string makePersistentSampleId(const UUID& collectionUuid,
                                   SamplingTechniqueEnum method,
                                   size_t sampleSize,
                                   boost::optional<int> numChunks,
                                   int pageNo = 0);

/**
 * Returns the inclusive [min, max] `_id` bounds spanning every page a sample with the given
 * identity values could have.
 */
std::pair<std::string, std::string> makePersistentSampleIdRange(const UUID& collectionUuid,
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
 * Loads a previously persisted sample matching the provided spec.
 */
class PersistentSampleLoader : public SampleLoader {
public:
    using SampleLoader::SampleLoader;

    /**
     * Loads the persisted sample matching the spec.
     *
     * Possible error codes:
     * - `NoSuchKey` if persistent samples are not usable (the feature flag is disabled, the spec
     *   forces on-the-fly sampling, the collection does not exist) or no document matches.
     * - `UnsupportedFormat` if the samples collection or the documents found in it are malformed.
     */
    StatusWith<Sample> tryLoad(const SampleSpec& spec) override;

protected:
    /**
     * Reports the sampling technique used.
     */
    SamplingTechniqueEnum technique(const SampleSpec& spec) const override {
        // TODO SERVER-136539
        const size_t collectionCard =
            static_cast<size_t>(_collections.lookupCollection(spec.nss)->numRecords(_opCtx));
        return spec.sampleSize >= collectionCard ? SamplingTechniqueEnum::kFullCollScan
                                                 : resolveSamplingTechnique(spec, collectionCard);
    }
};

std::unique_ptr<PersistentSampleLoader> makePersistentSampleLoader(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx);

}  // namespace mongo::ce
