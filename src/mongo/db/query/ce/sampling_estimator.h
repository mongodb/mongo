/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"

namespace mongo::ce {

using Cardinality = double;

/**
 * This CE Estimator estimates cardinality of predicates by running a filter/MatchExpression against
 * a generated sample. The sample will be generated either in a random walk fashion or by a
 * chunk-based sampling method. The sample is generated once and is stored in memory for one
 * optimization request for a query
 */
class SamplingEstimator {
public:
    enum class SamplingStyle { kRandom, kChunk };

    /**
     * 'opCtx' is used to create a new CanonicalQuery for the sampling SBE plan.
     * 'collections' is needed to create a sampling SBE plan. 'samplingStyle' can specify the
     * sampling method.
     */
    SamplingEstimator(OperationContext* opCtx,
                      const MultipleCollectionAccessor& collections,
                      SamplingEstimator::SamplingStyle samplingStyle);

    /*
     * This constructor allows the caller to specify the sample size if necessary. This constructor
     * is useful when a certain scale of sample is more appropriate, for example, the planner wants
     * to do preliminary data distribution analysis with a small sample size. Testing cases may
     * require only a small sample.
     */
    SamplingEstimator(OperationContext* opCtx,
                      const MultipleCollectionAccessor& collections,
                      size_t sampleSize,
                      SamplingEstimator::SamplingStyle samplingStyle);
    ~SamplingEstimator();

    /**
     * Estimates the Cardinality of a filter/MatchExpression by running the given ME against the
     * sample.
     */
    Cardinality estimateCardinality(const MatchExpression* expr);

    /**
     * Batch Estimates the Cardinality of a vector of filter/MatchExpression by running the given
     * MEs against the sample.
     */
    std::vector<Cardinality> estimateCardinality(const std::vector<MatchExpression*>& expr);

    /*
     * Generates a sample using a random cursor. The caller can call this function to draw a sample
     * of 'sampleSize'. If it's a re-sample request, the old sample will be freed and replaced by
     * the new sample.
     */
    void generateRandomSample(size_t sampleSize);
    void generateRandomSample();

    /*
     * Generates a sample using a chunk-based sampling method. The sample consists of multiple
     * random chunks. Similar to the other sampling function, the caller can call this function to
     * re-sample. The old sample will be freed.
     */
    void generateChunkSample(size_t sampleSize);
    void generateChunkSample();

    /*
     * Returns the sample size calculated by SamplingEstimator.
     */
    inline size_t getSampleSize() {
        return _sampleSize;
    }

protected:
    /*
     * This helper creates a CanonicalQuery for the sampling plan.
     */
    static std::unique_ptr<CanonicalQuery> makeCanonicalQuery(const NamespaceString& nss,
                                                              OperationContext* opCtx,
                                                              size_t sampleSize);

    // The sample is stored in memory for estimating the cardinality of all predicates of one query
    // request. The sample will be freed on destruction of the SamplingEstimator instance or when a
    // re-sample is requested. A new sample will replace this.
    std::vector<BSONObj> _sample;

private:
    /**
     * Constructs a sampling SBE plan using the random-walk method.
     * The SBE plan consists of a sbe::ScanStage which uses a random cursor to read documents
     * randomly from the collection and a sbe::LimitSkipStage on the top of the scan stage to limit
     * '_sampleSize' of the documents for the sample.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
    generateRandomSamplingPlan(PlanYieldPolicy* sbeYieldPolicy);

    /*
     * The SamplingEstimator calculates the size of a sample based on the confidence level and
     * margin of error required.
     */
    size_t calculateSampleSize();

    OperationContext* _opCtx;
    // The collection the sampling plan runs against and is the one accessed by the query being
    // optimized.
    const MultipleCollectionAccessor& _collections;
    size_t _sampleSize;
};

}  // namespace mongo::ce
