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

namespace mongo::optimizer::ce {

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
     * 'opCtx' and 'nss' are used to create a new CanonicalQuery for the sampling SBE plan. 'nss' is
     * is the NamespaceString of the collection accessed by the query being optimized. 'collections'
     * is needed to create a sampling SBE plan. 'samplingStyle' can specify the sampling method.
     */
    SamplingEstimator(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const MultipleCollectionAccessor& collections,
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
     * Generates a sample using a random cursor. The caller can call this function to re-sample.
     * The old sample will be freed.
     */
    void generateRandomSample();

    /*
     * Generates a sample using a chunk-based sampling method. The sample consists of multiple
     * random chunks. Similar to the other sampling function, the caller can call this function to
     * re-sample. The old sample will be freed.
     */
    void generateChunkSample();

    /*
     * Returns the sample size calculated by SamplingEstimator.
     */
    inline size_t getSampleSize() {
        return _sampleSize;
    }

protected:
    // This CanonicalQuery is for the sampling SBE plan. This is needed to construct and execute the
    // sampling plan.
    std::unique_ptr<CanonicalQuery> _cq;

private:
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

    // The sample is stored in memory for estimating the cardinality of all predicates of one query
    // request. The sample will be freed on destruction of the SamplingEstimator instance or when a
    // re-sample is requested. A new sample will replace this.
    std::vector<BSONObj> _sample;
};

}  // namespace mongo::optimizer::ce
