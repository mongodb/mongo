// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/ce/sampling/ce_multikey_dotted_path_support.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
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
    /**
     * Factory function for creating a 'SamplingEstimator' for use in differet calls into CBR.
     */
    static std::unique_ptr<SamplingEstimator> makeDefaultSamplingEstimator(
        const CanonicalQuery& cq,
        CardinalityEstimate collCard,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        const MultipleCollectionAccessor& collections,
        SamplingSourceEnum samplingSource = SamplingSourceEnum::kPersistentSample);

    /**
     * 'opCtx' is used to create a new CanonicalQuery for the sampling SBE plan.
     * 'collections' is needed to create a sampling SBE plan. 'samplingStyle' is the on-the-fly
     * method used when no persisted sample is loaded. 'samplingSource' controls whether to try
     * reading a persistent sample from `<db>.system.stats.samples` before falling back to
     * on-the-fly SBE sampling; 'analyze' passes kOnTheFlySample to bypass the persisted read.
     * 'persistentSampleMethod' independently picks which sampling technique to look for in the
     * persisted samples collection. Prefer the factory method above outside tests.
     */
    SamplingEstimatorImpl(
        OperationContext* opCtx,
        const MultipleCollectionAccessor& collections,
        const NamespaceString& nss,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        SamplingCEMethodEnum samplingStyle,
        CardinalityEstimate collectionCard,
        SamplingConfidenceIntervalEnum ci,
        double marginOfError,
        boost::optional<int> numChunks,
        boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx,
        SamplingSourceEnum samplingSource = SamplingSourceEnum::kPersistentSample,
        SamplingCEMethodEnum persistentSampleMethod = PersistentSampleCEMethod::kDataDefault);

    /*
     * Lets the caller specify an exact sample size, e.g. for a smaller preliminary-analysis
     * sample. Prefer the factory method above outside tests.
     */
    SamplingEstimatorImpl(
        OperationContext* opCtx,
        const MultipleCollectionAccessor& collections,
        const NamespaceString& nss,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        size_t sampleSize,
        SamplingCEMethodEnum samplingStyle,
        boost::optional<int> numChunks,
        CardinalityEstimate collectionCard,
        boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx,
        SamplingSourceEnum samplingSource = SamplingSourceEnum::kPersistentSample,
        SamplingCEMethodEnum persistentSampleMethod = PersistentSampleCEMethod::kDataDefault);

    /*
     * Convenience constructor that accepts a raw record count instead of a CardinalityEstimate,
     * for callers outside the cost_based_ranker module that cannot construct CardinalityType.
     */
    SamplingEstimatorImpl(
        OperationContext* opCtx,
        const MultipleCollectionAccessor& collections,
        const NamespaceString& nss,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        size_t sampleSize,
        SamplingCEMethodEnum samplingStyle,
        boost::optional<int> numChunks,
        long long numRecords,
        boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx,
        SamplingSourceEnum samplingSource = SamplingSourceEnum::kPersistentSample);

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
     * Estimates the number of distinct values of tuples of the given field names in the collection,
     * according to the equality semantics specified by each field in 'fields'.
     *
     * If 'bounds' is provided, only documents matching those bounds contribute to the estimate.
     * There must be one OrderedIntervalList per field, in the same order as 'fields'.
     *
     * Note: Does not support estimating NDV over array-valued fields.
     */
    CardinalityEstimate estimateNDV(
        const std::vector<FieldPathAndEqSemantics>& fields,
        boost::optional<std::span<const OrderedIntervalList>> bounds) const override;
    using SamplingEstimator::estimateNDV;

    /**
     * Estimates the number of distinct values of tuples of the given field names
     * a multikey index over the provided fields would contain, from the values
     * present in the sample.
     *
     * The sample documents are drawn from the collection; this is not exactly equivalent
     * to sampling from a multikey index, as each index key is not equally weighted or
     * drawn independently of "sibling" values in the same document.
     */
    CardinalityEstimate estimateNDVMultiKey(
        const std::vector<FieldPathAndEqSemantics>& fields,
        boost::optional<std::span<const OrderedIntervalList>> bounds) const override;
    using SamplingEstimator::estimateNDVMultiKey;

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
    inline size_t getSampleSize() const final {
        return _sampleSize;
    }

    /**
     * Returns the collected sample documents.
     */
    const std::vector<BSONObj>& getSample() const {
        return _sample;
    }

    /*
     * Returns the sampling metadata for the generated sample, which includes:
     * - the sampling technique
     * - the requested sample size
     * - the actual sample size
     * - the memory size of the sample in bytes
     * - the sampling source (persistent vs on-the-fly)
     * - the date and time when the sample was generated
     */
    SamplingMetadata getSamplingMetadata() const final;

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

        const auto bsonElmComparator =
            BSONElementComparator(BSONElementComparator::FieldNamesMode::kIgnore, nullptr);
        const auto hasher = BSONComparatorInterfaceBase<BSONElement>::Hasher(&bsonElmComparator);
        const auto equalTo = BSONComparatorInterfaceBase<BSONElement>::EqualTo(&bsonElmComparator);
        std::vector<MultiKeyDottedPathIterator> iterators;
        // TODO(SERVER-114759) Optimize non-multikey indices
        std::transform(bounds.fields.begin(),
                       bounds.fields.end(),
                       std::back_inserter(iterators),
                       [&](auto&& oil) { return MultiKeyDottedPathIterator(oil.name); });
        BSONElementSet elemSet(0, hasher, equalTo);

        // TODO(SERVER-114756): We can be more clever with retrieving the fields
        // by iterating the object and processing the fields in
        // the order they appear in the document.
        for (size_t sampleIdx = 0; sampleIdx < sample.size(); sampleIdx++) {
            size_t count = 1;

            for (size_t fieldIdx = 0; fieldIdx < iterators.size() && count > 0; fieldIdx++) {
                auto&& it = iterators[fieldIdx];
                const auto& oil = bounds.fields[fieldIdx];
                elemSet.clear();

                size_t elementCount = 0;
                BSONElement element = it.resetObj(&sample[sampleIdx]);
                bool hasNext;
                while (true) {
                    hasNext = it.hasNext();
                    if (elemSet.insert(element).second) {
                        elementCount += matchesInterval(oil, element);
                        if (elementCount > 0 && skipDuplicateMatches) {
                            break;
                        }
                    }
                    if (!hasNext) {
                        break;
                    }
                    element = it.getNext();
                }
                if (elementCount != 1) {
                    count = elementCount;
                }
            }

            callback(count);
        }
    }

    CardinalityEstimate getCollCard() const override {
        return _collectionCard;
    }

    /*
     * The sample size is calculated based on the confidence level and margin of error(MoE)
     * required.  n = Z^2 / W^2
     * where Z is the z-score for the confidence interval and
     * W is the width of the confidence interval, W = 2 * MoE.
     */
    static size_t calculateSampleSize(SamplingConfidenceIntervalEnum ci,
                                      double marginOfError,
                                      int32_t sampleSizeOverride = 0);

    /**
     * TODO SERVER-129240: Remove this helper once types are unified
     * Converts a SamplingCEMethodEnum value into its equivalent SamplingTechniqueEnum value.
     */
    static ce::SamplingTechniqueEnum samplingMethodToTechnique(
        SamplingCEMethodEnum samplingMethod) {
        switch (samplingMethod) {
            case SamplingCEMethodEnum::kRandom:
                return ce::SamplingTechniqueEnum::kRandom;
            case SamplingCEMethodEnum::kChunk:
                return ce::SamplingTechniqueEnum::kChunk;
            default:
                MONGO_UNREACHABLE;
        }
    }

protected:
    /*
     * This helper creates a CanonicalQuery for the sampling plan. This CanonicalQuery is "empty"
     * because its sole purpose is to be passed to 'prepareSlotBasedExecutableTree()' as part of
     * preparing the sampling plan for execution in SBE. That function uses the CanonicalQuery to
     * bind input parameters, but this is a no-op for sampling CE.
     */
    static std::unique_ptr<CanonicalQuery> makeEmptyCanonicalQuery(
        const NamespaceString& nss,
        OperationContext* opCtx,
        boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx);

    /**
     * This helper calls the given callback for each document
     * in a given vector that matches the given bounds.
     * The callback should return true to continue iterating, or false to stop.
     */
    template <typename T>
    static void forDocumentsMatchingBounds(const IndexBounds& bounds,
                                           const std::vector<BSONObj>& docs,
                                           const T& callback)
    requires std::is_invocable_r_v<bool, T, const BSONObj&>
    {
        size_t idx = 0;
        bool stopped = false;
        forNumberKeysMatch(
            bounds,
            docs,
            [&](size_t matchCnt) {
                if (!stopped && matchCnt > 0) {
                    stopped = !callback(docs[idx]);
                }
                idx++;
            },
            true /*skipDuplicateMatches*/);
    }

    // The sample is stored in memory for estimating the cardinality of all predicates of one query
    // request. The sample will be freed on destruction of the SamplingEstimator instance or when a
    // re-sample is requested. A new sample will replace this.
    std::vector<BSONObj> _sample;
    size_t _sampleSize;
    bool _isSampleGenerated = false;
    // Lazily computed on the first estimateNDV() call. Counts the number of documents with
    // distinct _id values in the sample to detect duplicates from sampling with replacement.
    mutable boost::optional<size_t> _uniqueDocCount;
    // Set to true when tryLoadPersistentSample() successfully loads sample from stats collection.
    bool _wasSamplePersisted = false;
    SamplingCEMethodEnum _persistentSampleMethod;
    // On-the-fly sampling technique used when no persisted sample is loaded.
    SamplingCEMethodEnum _samplingStyle;

private:
    /**
     * Constructs a sampling SBE plan using the random-walk method.
     * The SBE plan consists of a sbe::ScanStage which uses a random cursor to read documents
     * randomly from the collection and a sbe::LimitSkipStage on the top of the scan stage to limit
     * '_sampleSize' of the documents for the sample.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
    generateRandomSamplingPlan(PlanYieldPolicySBE* sbeYieldPolicy);

    /**
     * Constructs a sampling SBE plan using the chunk-based method.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
    generateChunkSamplingPlan(PlanYieldPolicySBE* sbeYieldPolicy);

    /**
     * Generates a sample by doing a full "CollScan" against the target collection. This sample is
     * generated when the collection size is smaller than the required sample size.
     */
    void generateFullCollScanSample();

    /**
     * Builds and runs an on-the-fly sampling plan for 'technique'.
     */
    void generateSampleForTechnique(SamplingTechniqueEnum technique);

    /**
     * This function executes the sampling query and generates the sample from the documents
     * produced by the query.
     */
    void executeSamplingQueryAndSample(
        std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>& plan,
        std::unique_ptr<CanonicalQuery> cq,
        std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy);

    /**
     * Generates a repeatable sample for testing using one of the deterministic scan-based
     * techniques:
     *   - kSeqScan: sequentially scan from the start of the collection and take the first
     *     '_sampleSize' documents.
     *   - kStrides: sequentially scan and keep documents whose _id satisfies
     *     shardHash(_id) % M == 0, where M = max(1, collCard / sampleSize). shardHash uses
     *     BSONElementHasher, which is stable across process restarts. The result is capped at
     *     '_sampleSize' documents.
     */
    void generateSampleForTesting(SamplingTechniqueEnum technique);

    /**
     * Helper to get SamplingTechnique from test-only knobs
     */
    boost::optional<SamplingTechniqueEnum> getTestOnlySamplingModeIfSet();

    /**
     * If '_samplingSource' is kPersistentSample, attempts to load a previously persisted sample
     * for the given method + size from `<db>.system.stats.samples` using a DBDirectClient point
     * lookup. Returns Status::OK() and populates '_sample' and '_sampleSize' on hit. Returns a
     * non-OK status otherwise: NoSuchKey for a clean miss or misconfiguration, another code for
     * a malformed doc. The caller is responsible for logging non-NoSuchKey failures and falling
     * back to SBE sampling.
     */
    Status tryLoadPersistentSample(SamplingTechniqueEnum method);

    OperationContext* _opCtx;
    // The collection the sampling plan runs against and is the one accessed by the query being
    // optimized.
    const MultipleCollectionAccessor& _collections;
    boost::intrusive_ptr<const ExpressionContext> _customerQueryExpCtx;
    NamespaceString _nss;
    PlanYieldPolicy::YieldPolicy _yieldPolicy;
    // The set of top level fields that we want to include in the sampled documents.
    StringSet _topLevelSampleFieldNames;

    boost::optional<int> _numChunks;

    CardinalityEstimate _collectionCard;

    // Controls whether persistent samples are consulted before falling back to SBE sampling.
    // 'analyze' constructs its estimator with kOnTheFlySample so it always collects a fresh sample
    // (otherwise a refresh would just re-read the sample it's about to replace).
    SamplingSourceEnum _samplingSource;

    // The timestamp when the sample was created. For persisted samples this is read from the
    // stored document; for on-the-fly samples it is set to Date_t::now() at the end of
    // generateSample(). Always valid after generateSample() completes.
    boost::optional<Date_t> _sampleCreatedAt;
    // The number of documents requested when generateSample() was called. May differ from the
    // actual sample size (_sampleSize) in the following cases:
    //   1. The collection is smaller than the requested sample size (full collection scan used).
    //   2. Chunk-based sampling: if a random cursor lands on the last document in the collection,
    //      no full chunk can be collected for that cursor, so the actual sample is smaller.
    size_t _requestedSampleSize = 0;
    // The actual sampling strategy used. Set by generateSample() before dispatch.
    boost::optional<SamplingTechniqueEnum> _usedSamplingTechnique;
};

}  // namespace mongo::ce
