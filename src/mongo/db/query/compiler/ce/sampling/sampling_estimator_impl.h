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

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/compiler/ce/sampling/ce_multikey_dotted_path_support.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/util/modules.h"

namespace mongo::ce {
/**
 * Helper function to extract the top level fields from a given MatchExpression.
 */
StringSet extractTopLevelFieldsFromMatchExpression(const MatchExpression* expr);

/**
 * This CE Estimator estimates cardinality of predicates by running a filter/MatchExpression against
 * a generated sample. The sample will be generated either in a random walk fashion or by a
 * chunk-based sampling method. The sample is generated once and is stored in memory for one
 * optimization request for a query
 */
class SamplingEstimatorImpl : public SamplingEstimator {
public:
    enum class SamplingStyle { kRandom = 1, kChunk = 2 };

    /**
     * Factory function for creating a 'SamplingEstimator' for use in differet calls into CBR.
     */
    static std::unique_ptr<SamplingEstimator> makeDefaultSamplingEstimator(
        CanonicalQuery& cq,
        CardinalityEstimate collCard,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        const MultipleCollectionAccessor& collections);

    /**
     * 'opCtx' is used to create a new CanonicalQuery for the sampling SBE plan.
     * 'collections' is needed to create a sampling SBE plan. 'samplingStyle' can specify the
     * sampling method. Prefer the factory method above outside tests.
     */
    SamplingEstimatorImpl(OperationContext* opCtx,
                          const MultipleCollectionAccessor& collections,
                          const NamespaceString& nss,
                          PlanYieldPolicy::YieldPolicy yieldPolicy,
                          SamplingStyle samplingStyle,
                          CardinalityEstimate collectionCard,
                          SamplingConfidenceIntervalEnum ci,
                          double marginOfError,
                          boost::optional<int> numChunks);

    /*
     * This constructor allows the caller to specify the sample size if necessary. This constructor
     * is useful when a certain scale of sample is more appropriate, for example, the planner wants
     * to do preliminary data distribution analysis with a small sample size. Testing cases may
     * require only a small sample. Prefer the factory method above outside tests.
     */
    SamplingEstimatorImpl(OperationContext* opCtx,
                          const MultipleCollectionAccessor& collections,
                          const NamespaceString& nss,
                          PlanYieldPolicy::YieldPolicy yieldPolicy,
                          size_t sampleSize,
                          SamplingStyle samplingStyle,
                          boost::optional<int> numChunks,
                          CardinalityEstimate collectionCard);

    ~SamplingEstimatorImpl() override;

    /**
     * Estimates the Cardinality of a filter/MatchExpression by running the given ME against the
     * sample.
     */
    CardinalityEstimate estimateCardinality(const MatchExpression* expr) const override;

    /**
     * Batch Estimates the Cardinality of a vector of filter/MatchExpression by running the given
     * MEs against the sample.
     */
    std::vector<CardinalityEstimate> estimateCardinality(
        const std::vector<const MatchExpression*>& expr) const override;

    /**
     * Estimates the number of keys scanned for the given IndexBounds. This function extracts all
     * index keys of a document in '_sample' and calculates the number of index keys scanned by
     * evaluating the index keys against the given IndexBounds.
     */
    CardinalityEstimate estimateKeysScanned(const IndexBounds& bounds) const override;

    std::vector<CardinalityEstimate> estimateKeysScanned(
        const std::vector<const IndexBounds*>& bounds) const override;

    /**
     * Estimate the number of RIDs which 'bounds' will return. Similar to 'estimateKeysScanned(..)',
     * this function evaluates index keys against the IndexBounds to determine the document
     * corresponding to that key matches the IndexBounds. If 'expr' is provided, the filter is
     * evaluated against the documents whose keys fall into the index bounds. This is used to
     * estimate a IndexScanNode with a residual filter.
     */
    CardinalityEstimate estimateRIDs(const IndexBounds& bounds,
                                     const MatchExpression* expr) const override;

    std::vector<CardinalityEstimate> estimateRIDs(
        const std::vector<const IndexBounds*>& bounds,
        const std::vector<const MatchExpression*>& expressions) const override;

    /**
     * Estimates the number of distinct values of tuples of the given field names in the collection.
     * Does not support estimating NDV over array-valued fields.
     */
    CardinalityEstimate estimateNDV(const std::vector<FieldPath>& fieldNames) const override;

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
     * Generates a sample of documents from the collection using random, chunk-based, sequential
     * scan or full collection scan sampling strategies based on configuration. 'projectionParams'
     * is a std::variant that specifies whether we want to project the top level fields in a sample.
     * If the variant type is TopLevelFieldsProjection we expect a set of top level fields that we
     * want to include in the sampled documents.
     */
    void generateSample(ce::ProjectionParams projectionParams) override;

    /*
     * Returns the sample size calculated by SamplingEstimator.
     */
    inline size_t getSampleSize() {
        return _sampleSize;
    }

    /**
     * For each document in a given sample, this helper calculates the number of
     * index keys which satisfy 'bounds', which may be >1 in the case of multi-key
     * indexes, and calls the given callback.
     * 'skipDuplicateMatches' is used when estimating the number of matching RIDs
     * which can be 0 or 1 per document.
     */
    template <typename T>
    static void forNumberKeysMatch(const IndexBounds& bounds,
                                   const std::vector<BSONObj>& sample,
                                   const T& callback,
                                   bool skipDuplicateMatches = false)
    requires std::invocable<T, size_t>
    {
        // TODO(SERVER-114758) Refactor skipDuplicateMatches=false into a
        // separate public method that calls an internal one.
        using BSONElementSet =
            absl::flat_hash_set<BSONElement,
                                BSONComparatorInterfaceBase<BSONElement>::Hasher,
                                BSONComparatorInterfaceBase<BSONElement>::EqualTo>;

        if (sample.size() == 0) {
            return;
        }
        const auto bsonElmComparator =
            BSONElementComparator(BSONElementComparator::FieldNamesMode::kIgnore, nullptr);
        const auto hasher = BSONComparatorInterfaceBase<BSONElement>::Hasher(&bsonElmComparator);
        const auto equalTo = BSONComparatorInterfaceBase<BSONElement>::EqualTo(&bsonElmComparator);
        std::vector<MultiKeyDottedPathIterator> iterators;
        // TODO(SERVER-114759) Optimize non-multikey indices
        std::transform(
            bounds.fields.begin(),
            bounds.fields.end(),
            std::back_inserter(iterators),
            [&](auto&& oil) { return MultiKeyDottedPathIterator(&sample[0], oil.name); });
        BSONElementSet elemSet(0, hasher, equalTo);

        // TODO(SERVER-114756): We can be more clever with retrieving the fields
        // by iterating the object and processing the fields in
        // the order they appear in the document.
        for (size_t sampleIdx = 0; sampleIdx < sample.size(); sampleIdx++) {
            size_t count = 1;

            for (size_t fieldIdx = 0; fieldIdx < iterators.size() && count > 0; fieldIdx++) {
                auto&& it = iterators[fieldIdx];
                const auto& oil = bounds.fields[fieldIdx];
                it.resetObj(&sample[sampleIdx]);
                elemSet.clear();

                auto [element, isLast] = it.nextElement();
                // The first element will always be there
                size_t elementCount = 0;
                while (true) {
                    if (elemSet.insert(element).second) {
                        elementCount += matches(oil, element);
                        if (elementCount > 0 && skipDuplicateMatches) {
                            break;
                        }
                    }
                    if (isLast) {
                        break;
                    }
                    std::tie(element, isLast) = it.nextElement();
                }
                if (elementCount != 1) {
                    count = elementCount;
                }
            }

            callback(count);
        }
    }

    double getCollCard() const override {
        return _collectionCard.toDouble();
    }

protected:
    /*
     * This helper creates a CanonicalQuery for the sampling plan. This CanonicalQuery is “empty”
     * because its sole purpose is to be passed to ‘prepareSlotBasedExecutableTree()’ as part of
     * preparing the sampling plan for execution in SBE. That function uses the CanonicalQuery to
     * bind input parameters, but this is a no-op for sampling CE.
     */
    static std::unique_ptr<CanonicalQuery> makeEmptyCanonicalQuery(const NamespaceString& nss,
                                                                   OperationContext* opCtx);

    /*
     * The sample size is calculated based on the confidence level and margin of error(MoE)
     * required.  n = Z^2 / W^2
     * where Z is the z-score for the confidence interval and
     * W is the width of the confidence interval, W = 2 * MoE.
     */
    static size_t calculateSampleSize(SamplingConfidenceIntervalEnum ci, double marginOfError);

    /**
     * This helper checks if an element is within the given Interval.
     */
    static bool matches(const Interval& interval, BSONElement val);

    /**
     * This helper checks if an element is within any of the list of Interval.
     */
    static bool matches(const OrderedIntervalList& oil, BSONElement val);

    /**
     * This helper calls the given callback for each document
     * in a given vector that matches the given bounds.
     */
    template <typename T>
    static void forDocumentsMatchingBounds(const IndexBounds& bounds,
                                           const std::vector<BSONObj>& docs,
                                           const T& callback)
    requires std::invocable<T, const BSONObj&>
    {
        size_t idx = 0;
        forNumberKeysMatch(
            bounds,
            docs,
            [&](size_t matchCnt) {
                if (matchCnt > 0) {
                    callback(docs[idx]);
                }
                idx++;
            },
            true /*skipDuplicateMatches*/);
    }

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

    /**
     * Constructs a sampling SBE plan using the chunk-based method.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
    generateChunkSamplingPlan(PlanYieldPolicy* sbeYieldPolicy);

    /**
     * Generates a sample by doing a full "CollScan" against the target collection. This sample is
     * generated when the collection size is smaller than the required sample size.
     */
    void generateFullCollScanSample();

    /**
     * This function executes the sampling query and generates the sample from the documents
     * produced by the query.
     */
    void executeSamplingQueryAndSample(
        std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>& plan,
        std::unique_ptr<CanonicalQuery> cq,
        std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy);

    /**
     * Generates a sample by sequentially scanning documents from the start of the target
     * collection. The sample is generated from the first '_sampleSize' documents of the collection.
     * This sampling method is only used for testing purposes where a repeatable sample is needed.
     */
    void generateSampleBySeqScanningForTesting();

    /*
     * The SamplingEstimator calculates the size of a sample based on the confidence level and
     * margin of error required.
     */
    size_t calculateSampleSize();

    OperationContext* _opCtx;
    // The collection the sampling plan runs against and is the one accessed by the query being
    // optimized.
    const MultipleCollectionAccessor& _collections;
    NamespaceString _nss;
    PlanYieldPolicy::YieldPolicy _yieldPolicy;
    SamplingStyle _samplingStyle;
    size_t _sampleSize;
    // The set of top level fields that we want to include in the sampled documents.
    StringSet _topLevelSampleFieldNames;
    bool _isSampleGenerated = false;

    boost::optional<int> _numChunks;

    CardinalityEstimate _collectionCard;
};

}  // namespace mongo::ce
