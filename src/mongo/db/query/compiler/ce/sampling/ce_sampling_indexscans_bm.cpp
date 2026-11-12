// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_build_test_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/compiler/ce/ce_test_utils.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_rewrites.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_test_utils.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/expression_geo_index_mapping.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/query_knobs/query_knob_configuration_test_util.h"

#include <limits>

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::ce {

// Defining a constant seed value for data generation, the use of this ensure that point queries
// will match against at least one generated document in the dataset.
const size_t seed_value1 = 1724178;
const size_t seed_value2 = 8713211;

/**
 * This map defines a set of configurations for collection generation.
 * The keys of the map are used as part of the benchmark state inputs to create a variety of base
 * collection to test against.
 * Each configuration requires a vector of CollectionFieldConfigurations which represent the set of
 * "user defined" fields. Each field requires configuring its name, position in the collection as
 * well as type and distribution information.
 * The collection will contain as many fields as the maximum position of the user defined fields.
 * The remaining in-between fields are copies of the user defined fields with names with suffix an
 * underscore and a number.
 * When defining the fields in the collection, order the fields in increasing position order.
 */
std::map<int, std::vector<CollectionFieldConfiguration>> collectionFieldConfigurations = {
    {1,
     {CollectionFieldConfiguration(
         /*fieldName=*/"f0",
         /*fieldPositionInCollection=*/0,
         /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
         /*ndv=*/1000,
         /*dataDistribution=*/stats::DistrType::kUniform,
         /*seed=*/seed_value1)}},
    {2,
     {CollectionFieldConfiguration(
         /*fieldName=*/"f0",
         /*fieldPositionInCollection=*/50,
         /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
         /*ndv=*/1000,
         /*dataDistribution=*/stats::DistrType::kUniform,
         /*seed=*/seed_value1)}},
    {3,
     {
         CollectionFieldConfiguration(
             /*fieldName=*/"f0",
             /*fieldPositionInCollection=*/0,
             /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f1",
             /*fieldPositionInCollection=*/1,
             /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f2",
             /*fieldPositionInCollection=*/2,
             /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f3",
             /*fieldPositionInCollection=*/3,
             /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f4",
             /*fieldPositionInCollection=*/4,
             /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
     }},
    {4,
     {
         CollectionFieldConfiguration(
             /*fieldName=*/"f0",
             /*fieldPositionInCollection=*/50,
             /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f1",
             /*fieldPositionInCollection=*/51,
             /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f2",
             /*fieldPositionInCollection=*/52,
             /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f3",
             /*fieldPositionInCollection=*/53,
             /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f4",
             /*fieldPositionInCollection=*/54,
             /*fieldDataType=*/sbe::value::TypeTags::NumberInt64,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
     }},
    {5,
     {CollectionFieldConfiguration(
         /*fieldName=*/"f0",
         /*fieldPosition=*/0,
         /*fieldType=*/sbe::value::TypeTags::StringSmall,
         /*ndv=*/1000,
         /*dataDistribution=*/stats::DistrType::kUniform,
         /*seed=*/seed_value1)}},
    {6,
     {
         CollectionFieldConfiguration(
             /*fieldName=*/"f0",
             /*fieldPosition=*/0,
             /*fieldType=*/sbe::value::TypeTags::StringSmall,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f1",
             /*fieldPosition=*/1,
             /*fieldType=*/sbe::value::TypeTags::StringSmall,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f2",
             /*fieldPosition=*/2,
             /*fieldType=*/sbe::value::TypeTags::StringSmall,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f3",
             /*fieldPosition=*/3,
             /*fieldType=*/sbe::value::TypeTags::StringSmall,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
         CollectionFieldConfiguration(
             /*fieldName=*/"f4",
             /*fieldPosition=*/4,
             /*fieldType=*/sbe::value::TypeTags::StringSmall,
             /*ndv=*/1000,
             /*dataDistribution=*/stats::DistrType::kUniform,
             /*seed=*/seed_value1),
     }}};

/**
 * This map defines the set of attributes queries will evaluate predicates on.
 * The seeds represent the seeds for the lower and upper values for a range.
 * For a point query only the first seed is used whereas for range query both seeds are relevant.
 * For range queries, ensure that the two seed values differ, otherwise the queries become in
 * essence point queries.
 */

// Single field point queries.
auto queryConfig1 = WorkloadConfiguration(
    /* numberOfQueries */ 20,
    QueryConfiguration({DataFieldDefinition(
                           /* fieldName*/ "f0",
                           /* fieldType */ sbe::value::TypeTags::NumberInt64,
                           /* ndv */ 1000,
                           stats::DistrType::kUniform,
                           {seed_value1, 1724178})},
                       /* queryTypes */ {kPoint}));

// Single field range queries.
auto queryConfig2 = WorkloadConfiguration(
    /* numberOfQueries */ 20,
    QueryConfiguration({DataFieldDefinition(
                           /* fieldName*/ "f0",
                           /* fieldType */ sbe::value::TypeTags::NumberInt64,
                           /* ndv */ 1000,
                           stats::DistrType::kUniform,
                           {seed_value1, seed_value2})},
                       /* queryTypes */ {kRange}));

// Multi-field point queries.
auto queryConfig3 = WorkloadConfiguration(
    /*numberOfQueries=*/20,
    QueryConfiguration(
        {
            DataFieldDefinition(
                /* fieldName*/ "f0",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, 1724178}),
            DataFieldDefinition(
                /* fieldName*/ "f1",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, 1724178}),
            DataFieldDefinition(
                /* fieldName*/ "f2",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, 1724178}),
            DataFieldDefinition(
                /* fieldName*/ "f3",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, 1724178}),
            DataFieldDefinition(
                /* fieldName*/ "f4",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, 1724178}),
        },
        /* queryTypes */ {kPoint, kPoint, kPoint, kPoint, kPoint}));

// Multi-field range queries.
auto queryConfig4 = WorkloadConfiguration(
    /* numberOfQueries */ 20,
    QueryConfiguration(
        {
            DataFieldDefinition(
                /* fieldName*/ "f0",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f1",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f2",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f3",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f4",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
        },
        /* queryTypes */
        {kRange, kRange, kRange, kRange, kRange}));

// Single string field point queries.
auto queryConfig5 = WorkloadConfiguration(
    /* numberOfQueries */ 20,
    QueryConfiguration({DataFieldDefinition(
                           /* fieldName*/ "f0",
                           /* fieldType */ sbe::value::TypeTags::StringSmall,
                           /* ndv */ 1000,
                           stats::DistrType::kUniform,
                           {seed_value1, seed_value1})},
                       /* queryTypes */ {kPoint}));

// Single string field range queries.
auto queryConfig6 = WorkloadConfiguration(
    /* numberOfQueries */ 20,
    QueryConfiguration({DataFieldDefinition(
                           /* fieldName*/ "f0",
                           /* fieldType */ sbe::value::TypeTags::StringSmall,
                           /* ndv */ 1000,
                           stats::DistrType::kUniform,
                           {seed_value1, seed_value2})},
                       /* queryTypes */ {kRange}));

// Multi-field string point queries.
auto queryConfig7 = WorkloadConfiguration(
    /*numberOfQueries*/ 20,
    QueryConfiguration(
        {
            DataFieldDefinition(
                /* fieldName*/ "f0",
                /* fieldType */ sbe::value::TypeTags::StringSmall,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value1}),
            DataFieldDefinition(
                /* fieldName*/ "f1",
                /* fieldType */ sbe::value::TypeTags::StringSmall,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value1}),
            DataFieldDefinition(
                /* fieldName*/ "f2",
                /* fieldType */ sbe::value::TypeTags::StringSmall,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value1}),
            DataFieldDefinition(
                /* fieldName*/ "f3",
                /* fieldType */ sbe::value::TypeTags::StringSmall,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value1}),
            DataFieldDefinition(
                /* fieldName*/ "f4",
                /* fieldType */ sbe::value::TypeTags::StringSmall,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value1}),
        },
        /* queryTypes */ {kPoint, kPoint, kPoint, kPoint, kPoint}));

// Multi-field string range queries.
auto queryConfig8 = WorkloadConfiguration(
    /*numberOfQueries*/ 20,
    QueryConfiguration(
        {
            DataFieldDefinition(
                /* fieldName*/ "f0",
                /* fieldType */ sbe::value::TypeTags::StringSmall,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f1",
                /* fieldType */ sbe::value::TypeTags::StringSmall,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f2",
                /* fieldType */ sbe::value::TypeTags::StringSmall,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f3",
                /* fieldType */ sbe::value::TypeTags::StringSmall,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f4",
                /* fieldType */ sbe::value::TypeTags::StringSmall,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
        },
        /* queryTypes */ {kRange, kRange, kRange, kRange, kRange}));

std::vector<std::pair<int, WorkloadConfiguration&>> queryConfigVector{
    {1, queryConfig1},
    {2, queryConfig2},
    {3, queryConfig3},
    {4, queryConfig4},
    {5, queryConfig5},
    {6, queryConfig6},
    {7, queryConfig7},
    {8, queryConfig8},
};

/**
 * This map defines a set of configurations for workload generation.
 */
std::map<int, WorkloadConfiguration> queryFieldsConfigurations(queryConfigVector.begin(),
                                                               queryConfigVector.end());

enum class IndexTypeBM {
    kRegular = 0,
    // A regular b-tree index estimated via the keystring path (forced via
    // 'internalQuerySamplingCEForceKeystringEstimation'): isolates the
    // keystring extraction overhead from the per-type transformation work.
    kRegularKeystring = 1,
    // Hashed indices can only have one hashed field.
    // We pick the first one to be hashed.
    kHashed = 2,
    kCollated = 3,
    kWildcard = 4,
};

/**
 * Translates the bounds built by 'getIndexBounds' for the benchmarked index type.
 */
IndexBounds translateIndexBoundsForIndexType(IndexTypeBM indexType,
                                             const WorkloadConfiguration& workloadConfig,
                                             IndexBounds bounds,
                                             const CollatorInterface* indexCollator) {
    switch (indexType) {
        case IndexTypeBM::kRegular:
        case IndexTypeBM::kRegularKeystring:
            break;
        case IndexTypeBM::kCollated:
            for (auto& oil : bounds.fields) {
                for (auto& interval : oil.intervals) {
                    BSONObjBuilder bob;
                    CollationIndexKey::collationAwareIndexKeyAppend(
                        interval.start, indexCollator, &bob);
                    CollationIndexKey::collationAwareIndexKeyAppend(
                        interval.end, indexCollator, &bob);
                    interval = Interval(bob.done(), interval.startInclusive, interval.endInclusive);
                }
            }
            break;
        case IndexTypeBM::kHashed: {
            // Only the first field is hashed.
            auto& oil = bounds.fields[0];
            for (auto& interval : oil.intervals) {
                invariant(interval.isPoint());
                interval =
                    IndexBoundsBuilder::makePointInterval(ExpressionMapping::hash(interval.start));
            }
            break;
        }
        case IndexTypeBM::kWildcard: {
            // Note that we don't create compound wildcard indices in this benchmark.
            OrderedIntervalList pathOil("$_path");
            pathOil.intervals.emplace_back(IndexBoundsBuilder::makePointInterval(
                BSON("" << workloadConfig.queryConfig.queryFields[0].fieldName)));
            bounds.fields.insert(bounds.fields.begin(), std::move(pathOil));
            break;
        }
    }
    return bounds;
}

IndexEntry createIndex(IndexTypeBM indexType,
                       const std::vector<CollectionFieldConfiguration>& fieldsConfig,
                       OperationContext* opCtx,
                       const NamespaceString& nss) {
    BSONObjBuilder keyBob;
    for (size_t i = 0; i < fieldsConfig.size(); i++) {
        const auto& field = fieldsConfig[i];
        // Hashed indexes allow at most one hashed field
        // which we always create on f0.
        if (indexType == IndexTypeBM::kHashed && i == 0) {
            keyBob.append(field.fieldName, "hashed");
        } else {
            keyBob.append(field.fieldName, 1);
        }
    }
    const char* indexName = "";
    BSONObj spec;
    switch (indexType) {
        case IndexTypeBM::kHashed:
            indexName = "hashed";
            spec = BSON("name" << indexName << "key" << keyBob.done() << "v" << 2);
            break;
        case IndexTypeBM::kRegularKeystring:
            indexName = "keystring";
            spec = BSON("name" << indexName << "key" << keyBob.done() << "v" << 2);
            break;
        case IndexTypeBM::kCollated:
            indexName = "collated";
            spec = BSON("name" << indexName << "key" << keyBob.done() << "collation"
                               << BSON("locale" << "en"
                                                << "strength" << 2)
                               << "v" << 2);
            break;
        case IndexTypeBM::kWildcard:
            indexName = "wildcard";
            spec = BSON("name" << indexName << "key" << BSON("$**" << 1) << "v" << 2);
            break;
        case IndexTypeBM::kRegular:
            indexName = "regular";
            spec = BSON("name" << indexName << "key" << keyBob.done() << "v" << 2);
            break;
    }
    ASSERT_OK(createIndexFromSpec(opCtx, nss.ns_forTest(), spec));

    auto indexAcq = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_IX);
    auto indexEntry = makeIndexEntry(opCtx, indexAcq.getCollectionPtr(), indexName);
    if (indexType == IndexTypeBM::kWildcard) {
        // The planner rewrites the wildcard entry's key pattern into the
        // {..., $_path: 1, 'path': 1, ...} shape during planning (see
        // 'finalizeWildcardIndexScanConfiguration'); mirror it here.
        indexEntry.keyPattern = BSON("$_path" << 1 << fieldsConfig[0].fieldName << 1);
        indexEntry.wildcardFieldPos = 1;
    }

    return indexEntry;
}

void estimatesSanityCheck(
    OperationContext* opCtx,
    const WorkloadConfiguration& workloadConfig,
    const std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>>& allQueryIntervals,
    IndexTypeBM indexType,
    const IndexEntry& indexEntry,
    SamplingEstimatorImpl& samplingEstimator) {
    // One interval per query field, as in the timed loop below
    // ('allQueryIntervals' is one vector of intervals per query field, which getIndexBounds
    // does not support).
    std::vector<std::pair<stats::SBEValue, stats::SBEValue>> firstQueryIntervals;
    for (size_t fieldIdx = 0; fieldIdx < workloadConfig.queryConfig.queryFields.size();
         fieldIdx++) {
        firstQueryIntervals.push_back(allQueryIntervals[fieldIdx][0]);
    }
    auto regularBounds = getIndexBounds(workloadConfig.queryConfig, firstQueryIntervals);
    auto translatedBounds = translateIndexBoundsForIndexType(
        indexType, workloadConfig, regularBounds, indexEntry.collator);
    const auto regularEstimate = [&] {
        QueryKnobGuardForTest guard{
            opCtx, "internalQuerySamplingCEForceKeystringEstimation", false};
        // The reference estimate: a plain b-tree index over the query fields. Its projection-path
        // estimate is what the exotic indexes' keystring estimates must agree with.
        return samplingEstimator.estimateRIDs(
            regularBounds, nullptr, cost_based_ranker::buildSimpleIndexEntry({"a"}));
    }();
    // The keystring-path estimate runs with the ambient knob value, i.e. exactly as the timed
    // loop below estimates it: that configuration is what this check is sanity-checking.
    const auto keyStringEstimate =
        samplingEstimator.estimateRIDs(translatedBounds, nullptr, indexEntry);
    switch (indexType) {
        case IndexTypeBM::kHashed:
        case IndexTypeBM::kWildcard:
        case IndexTypeBM::kRegularKeystring:
            ASSERT_EQUALS(regularEstimate.toDouble(), keyStringEstimate.toDouble());
            break;
        case IndexTypeBM::kCollated:
            // regularEstimate does not consider collation.
            // For point queries: collated query must find at last as many documents.
            // This does not hold for range queries
            // eg. "a" matches {$gt: "B"} with the simple collation but not under strength: 1.
            if (std::all_of(workloadConfig.queryConfig.queryTypes.begin(),
                            workloadConfig.queryConfig.queryTypes.end(),
                            [](QueryType type) { return type == kPoint; })) {
                ASSERT_GTE(keyStringEstimate.toDouble(), regularEstimate.toDouble());
            }
            break;
        case IndexTypeBM::kRegular:
            MONGO_UNREACHABLE;
    }
}

/**
 * Evaluate the performance of estimating IndexScanNode CE with index bounds.
 */
void BM_RunCardinalityEstimationOnSampleWithIndexBounds(benchmark::State& state) {
    auto fieldsConfig = collectionFieldConfigurations[state.range(1)];
    DataConfiguration dataConfig(
        /*size=*/state.range(0),
        /*allDataFieldConfig=*/fieldsConfig);

    // Setup query types.
    WorkloadConfiguration& workloadConfig = queryFieldsConfigurations[state.range(4)];
    const auto indexType = static_cast<IndexTypeBM>(state.range(5));

    // Cannot use hashed indices if the first field has a range predicate.
    if (indexType == IndexTypeBM::kHashed && workloadConfig.queryConfig.queryTypes[0] == kRange) {
        state.SkipWithError("Cannot use range queries with a hashed-indexed field.");
        return;
    }

    // Cannot use wildcard indices if the query has multiple fields.
    // This is a limitation of this benchmark: for multi-field queries,
    // we try to construct index bounds for each, but the wildcard index
    // we construct ($**) only supports one.
    if (indexType == IndexTypeBM::kWildcard && workloadConfig.queryConfig.queryTypes.size() > 1) {
        state.SkipWithError("Wildcard indices with multi-field workloads not supported.");
        return;
    }

    // Generate data and populate source collection
    SamplingEstimatorTest samplingEstimatorTest;
    initializeSamplingEstimator(dataConfig, samplingEstimatorTest);
    auto opCtx = samplingEstimatorTest.getOperationContext();

    // Force the keystring path for the kRegularKeystring rows.
    QueryKnobGuardForTest forceKeystringsGuard{opCtx,
                                               "internalQuerySamplingCEForceKeystringEstimation",
                                               indexType == IndexTypeBM::kRegularKeystring};
    // Measure extraction itself even for the wide-document workloads that normally fall back.
    QueryKnobGuardForTest maxKeysGuard{
        opCtx, "internalQuerySamplingCEMaxKeystringsPerDocument", std::numeric_limits<int>::max()};

    const NamespaceString nss = samplingEstimatorTest._kTestNss;
    auto indexEntry = createIndex(indexType, fieldsConfig, opCtx, nss);

    auto acquisition = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, samplingEstimatorTest._kTestNss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_IX);
    MultipleCollectionAccessor collection = MultipleCollectionAccessor(acquisition);

    // Translate the sample size definition to corresponding sample size.
    auto sampleSize = translateSampleDefToActualSampleSize(
        /*sampleSizeDef=*/static_cast<SampleSizeDef>(state.range(2)));

    // Translate the number of chunks variable to both number of chunks and sampling algo.
    // This benchmark given as input numOfChunks <= 0 will use kRandom.
    auto samplingStyle =
        iniitalizeSamplingAlgoBasedOnChunks(/*samplingAlgo-numOfChunks*/ state.range(3));

    // Create sample from the provided collection.
    SamplingEstimatorImpl samplingEstimator(
        samplingEstimatorTest.getOperationContext(),
        collection,
        collection.getMainCollection()->ns(),
        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
        sampleSize,
        samplingStyle.first,
        samplingStyle.second,
        SamplingEstimatorTest::makeCardinalityEstimate(dataConfig.size),
        /*customerQueryExpCtx=*/nullptr);
    samplingEstimator.generateSample(ce::NoProjection{});

    // Generate query intervals.
    // One vector element per query field, each element is a vector of numberOfQueries intervals.
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> allQueryIntervals =
        generateMultiFieldIntervals(workloadConfig);

    if (indexType != IndexTypeBM::kRegular) {
        estimatesSanityCheck(samplingEstimatorTest.getOperationContext(),
                             workloadConfig,
                             allQueryIntervals,
                             indexType,
                             indexEntry,
                             samplingEstimator);
    }

    size_t i = 0;
    for (auto _ : state) {
        state.PauseTiming();
        // Create index bounds combining intervals for all query fields.
        std::vector<std::pair<stats::SBEValue, stats::SBEValue>> queryIntervals;
        for (size_t fieldIdx = 0; fieldIdx < workloadConfig.queryConfig.queryFields.size();
             fieldIdx++) {
            queryIntervals.push_back(allQueryIntervals[fieldIdx][i]);
        }
        IndexBounds bounds = translateIndexBoundsForIndexType(
            indexType,
            workloadConfig,
            getIndexBounds(workloadConfig.queryConfig, queryIntervals),
            indexEntry.collator);
        state.ResumeTiming();

        benchmark::DoNotOptimize(samplingEstimator.estimateRIDs(bounds, nullptr, indexEntry));
        i = (i + 1) % workloadConfig.numberOfQueries;
    }
    state.SetItemsProcessed(state.iterations());
}

/**
 * Evaluate the performance of estimating IndexScanNode CE with match expression.
 */
void BM_RunCardinalityEstimationOnSampleWithMatchExpressions(benchmark::State& state) {
    auto fieldsConfig = collectionFieldConfigurations[state.range(1)];
    DataConfiguration dataConfig(
        /*size=*/state.range(0),
        /*allDataFieldConfig=*/fieldsConfig);

    // Setup query types.
    WorkloadConfiguration& workloadConfig = queryFieldsConfigurations[state.range(4)];

    // Generate data and populate source collection
    SamplingEstimatorTest samplingEstimatorTest;
    initializeSamplingEstimator(dataConfig, samplingEstimatorTest);

    // Initialize collection accessor
    auto opCtx = samplingEstimatorTest.getOperationContext();
    auto acquisition = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, samplingEstimatorTest._kTestNss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_IX);
    MultipleCollectionAccessor collection = MultipleCollectionAccessor(acquisition);

    // Translate the sample size definition to corresponding sample size.
    auto sampleSize = translateSampleDefToActualSampleSize(
        /*sampleSizeDef=*/static_cast<SampleSizeDef>(state.range(2)));

    // Translate the number of chunks variable to both number of chunks and sampling algo.
    // This benchmark given as input numOfChunks <= 0 will use kRandom.
    auto samplingStyle =
        iniitalizeSamplingAlgoBasedOnChunks(/*samplingAlgo-numOfChunks*/ state.range(3));

    // Create sample from the provided collection.
    SamplingEstimatorImpl samplingEstimator(
        samplingEstimatorTest.getOperationContext(),
        collection,
        collection.getMainCollection()->ns(),
        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
        sampleSize,
        samplingStyle.first,
        samplingStyle.second,
        SamplingEstimatorTest::makeCardinalityEstimate(dataConfig.size),
        /*customerQueryExpCtx=*/nullptr);
    samplingEstimator.generateSample(ce::NoProjection{});

    // Generate query intervals.
    // One vector element per query field, each element is a vector of numberOfQueries intervals.
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> allQueryIntervals =
        generateMultiFieldIntervals(workloadConfig);

    size_t i = 0;
    for (auto _ : state) {
        state.PauseTiming();
        // Create index bounds combining intervals for all query fields.
        std::vector<std::pair<stats::SBEValue, stats::SBEValue>> queryIntervals;
        for (size_t fieldIdx = 0; fieldIdx < workloadConfig.queryConfig.queryFields.size();
             fieldIdx++) {
            queryIntervals.push_back(allQueryIntervals[fieldIdx][i]);
        }
        IndexBounds bounds = getIndexBounds(workloadConfig.queryConfig, queryIntervals);
        state.ResumeTiming();

        auto matchExpr = cost_based_ranker::getMatchExpressionFromBounds(bounds, nullptr);
        benchmark::DoNotOptimize(samplingEstimator.estimateCardinality(matchExpr.get()));
        i = (i + 1) % workloadConfig.numberOfQueries;
    }
    state.SetItemsProcessed(state.iterations());
}

// Single int field.
BENCHMARK(BM_RunCardinalityEstimationOnSampleWithIndexBounds)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "queryFieldConfig",
        "indexType",
    })
    ->ArgsProduct({/*dataSize*/ {10000},
                   /*dataFieldsConfiguration*/ {1, 2},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting5),
                    static_cast<int>(SampleSizeDef::ErrorSetting2)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*queryFieldConfig*/ {1, 2},
                   /*indexType*/
                   {static_cast<int>(IndexTypeBM::kRegular),
                    static_cast<int>(IndexTypeBM::kRegularKeystring),
                    static_cast<int>(IndexTypeBM::kHashed),
                    static_cast<int>(IndexTypeBM::kWildcard)}})
    ->Unit(benchmark::kMillisecond);

// Multiple int fields in the docs.
BENCHMARK(BM_RunCardinalityEstimationOnSampleWithIndexBounds)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "queryFieldConfig",
        "indexType",
    })
    ->ArgsProduct({/*dataSize*/ {10000},
                   /*dataFieldsConfiguration*/ {3, 4},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting5),
                    static_cast<int>(SampleSizeDef::ErrorSetting2)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*queryFieldConfig*/ {1, 2, 3, 4},
                   /*indexType*/
                   {static_cast<int>(IndexTypeBM::kRegular),
                    static_cast<int>(IndexTypeBM::kRegularKeystring),
                    static_cast<int>(IndexTypeBM::kHashed),
                    static_cast<int>(IndexTypeBM::kWildcard)}})
    ->Unit(benchmark::kMillisecond);

// Single string field.
BENCHMARK(BM_RunCardinalityEstimationOnSampleWithIndexBounds)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "queryFieldConfig",
        "indexType",
    })
    ->ArgsProduct({/*dataSize*/ {10000},
                   /*dataFieldsConfiguration*/ {5},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting5),
                    static_cast<int>(SampleSizeDef::ErrorSetting2)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*queryFieldConfig*/ {5, 6},
                   /*indexType*/
                   {static_cast<int>(IndexTypeBM::kRegular),
                    static_cast<int>(IndexTypeBM::kRegularKeystring),
                    static_cast<int>(IndexTypeBM::kHashed),
                    static_cast<int>(IndexTypeBM::kCollated),
                    static_cast<int>(IndexTypeBM::kWildcard)}})
    ->Unit(benchmark::kMillisecond);

// Five string fields in the docs.
BENCHMARK(BM_RunCardinalityEstimationOnSampleWithIndexBounds)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "queryFieldConfig",
        "indexType",
    })
    ->ArgsProduct({/*dataSize*/ {10000},
                   /*dataFieldsConfiguration*/ {6},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting5),
                    static_cast<int>(SampleSizeDef::ErrorSetting2)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*queryFieldConfig*/ {5, 6, 7, 8},
                   /*indexType*/
                   {static_cast<int>(IndexTypeBM::kRegular),
                    static_cast<int>(IndexTypeBM::kRegularKeystring),
                    static_cast<int>(IndexTypeBM::kHashed),
                    static_cast<int>(IndexTypeBM::kCollated),
                    static_cast<int>(IndexTypeBM::kWildcard)}})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_RunCardinalityEstimationOnSampleWithMatchExpressions)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {10000},
                   /*dataFieldsConfiguration*/ {1, 2},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting5),
                    static_cast<int>(SampleSizeDef::ErrorSetting2)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*queryFieldConfig*/ {1, 2}})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_RunCardinalityEstimationOnSampleWithMatchExpressions)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {10000},
                   /*dataFieldsConfiguration*/ {3, 4},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting5),
                    static_cast<int>(SampleSizeDef::ErrorSetting2)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*queryFieldConfig*/ {3, 4}})
    ->Unit(benchmark::kMillisecond);

}  // namespace mongo::ce
