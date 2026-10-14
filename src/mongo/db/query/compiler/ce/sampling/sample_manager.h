// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/ce/sampling/sample.h"
#include "mongo/db/query/compiler/ce/sampling/sample_spec.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::ce {

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
 * Summary statistics for a sample
 */
struct PersistedSampleSummary {
    SamplingTechniqueEnum samplingMethod;
    size_t docsPersisted;
    size_t pagesPersisted;
    size_t bytesPersisted;
};

/**
 * This class coordinates loading and persisting samples.
 */
class SampleManager {
public:
    SampleManager(OperationContext* opCtx,
                  PlanYieldPolicy::YieldPolicy yieldPolicy,
                  boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx);

    /**
     * Returns either:
     * - the matching persisted sample if one exists (skipped if the spec explicitly asks for
     * an on-the-fly sample).
     * - a sample generated on the fly.
     * depending on the provided specifications.
     *
     * 'collections' holds the acquisitions the sample is loaded against. It is borrowed from the
     * caller and must outlive this call.
     */
    Sample load(const MultipleCollectionAccessor& collections, const SampleSpec& spec);

    /**
     * Draws a new sample for 'spec' from the source collection and persists it, replacing any
     * existing sample with the same identity. 'spec' must request an on-the-fly sample.
     *
     * Returns summary statistics of the sample persisted if successful.
     */
    StatusWith<PersistedSampleSummary> refresh(const SampleSpec& spec);

private:
    /**
     * Serializes 'sample' into page document(s) and atomically persists them, replacing the
     * existing sample if one exists.
     * Returns summary statistics of the sample persisted if successful.
     */
    StatusWith<PersistedSampleSummary> persistAndReplaceExisting(const SampleSpec& spec,
                                                                 const Sample& sample,
                                                                 const UUID& collectionUuid,
                                                                 Date_t createdAt);

    OperationContext* _opCtx;
    PlanYieldPolicy::YieldPolicy _yieldPolicy;
    boost::intrusive_ptr<const ExpressionContext> _customerQueryExpCtx;
};

}  // namespace mongo::ce
