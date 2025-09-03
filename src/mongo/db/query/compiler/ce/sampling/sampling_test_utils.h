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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/ce/ce_test_utils.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {

class SamplingEstimatorForTesting : public SamplingEstimatorImpl {
public:
    using SamplingEstimatorImpl::SamplingEstimatorImpl;

    const std::vector<BSONObj>& getSample() {
        return _sample;
    }

    static size_t calculateSampleSize(SamplingConfidenceIntervalEnum ci, double marginOfError) {
        return SamplingEstimatorImpl::calculateSampleSize(ci, marginOfError);
    }

    double getCollCard() {
        return SamplingEstimatorImpl::getCollCard();
    }

    static bool matches(const OrderedIntervalList& oil, BSONElement val) {
        return SamplingEstimatorImpl::matches(oil, val);
    }

    static std::vector<BSONObj> getIndexKeys(const IndexBounds& bounds, const BSONObj& doc) {
        return SamplingEstimatorImpl::getIndexKeys(bounds, doc);
    }

    // Help function to compute the margin of error for the given sample size. The z parameter
    // corresponds to the confidence %.
    double marginOfError(double z) {
        return z * sqrt(0.25 / getSampleSize());
    }

    // Help function to compute confidence interval for a cardinality value. The z parameter
    // corresponds to the confidence %. Default value of z is for 95% confidence.
    std::pair<CardinalityEstimate, CardinalityEstimate> confidenceInterval(double card,
                                                                           double z = 1.96) {
        auto moe = marginOfError(z);
        double collCard = getCollCard();

        double minCard = std::max(card - moe * collCard, 0.0);
        // maxCard could be greater than collCard if we're estimating the index keys scanned.
        double maxCard = card + moe * collCard;

        CardinalityEstimate expectedEstimateMin(mongo::cost_based_ranker::CardinalityType{minCard},
                                                mongo::cost_based_ranker::EstimationSource::Code);
        CardinalityEstimate expectedEstimateMax(mongo::cost_based_ranker::CardinalityType{maxCard},
                                                mongo::cost_based_ranker::EstimationSource::Code);
        return std::make_pair(expectedEstimateMin, expectedEstimateMax);
    }

    void assertEstimateInConfidenceInterval(CardinalityEstimate estimate, double expectedCard) {
        auto expectedInterval = confidenceInterval(expectedCard);
        bool estimateInInterval =
            (estimate >= expectedInterval.first && estimate <= expectedInterval.second);
        if (!estimateInInterval) {
            // This is a functionality test. Print the error in case the estimate is outside of the
            // confidence interval.
            double error = abs(estimate.cardinality().v() - expectedCard) / getCollCard();
            std::cout << "=== " << estimate.toString() << ", Interval = ("
                      << expectedInterval.first.cardinality().v() << ", "
                      << expectedInterval.second.cardinality().v() << "), Error " << error * 100
                      << "%" << std::endl;
        }
    }
};

class SamplingEstimatorTest : public CatalogTestFixture {
public:
    void setUp() override {
        _kTestNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), _kTestNss, CollectionOptions()));
    }

    void tearDown() override {
        CatalogTestFixture::tearDown();
    }

    /**
     * Insert documents to the default collection.
     */
    void insertDocuments(const NamespaceString& nss,
                         std::vector<BSONObj> docs,
                         int batchSize = 50000);

    /**
     * Create a vector of "num" copies of pre-defined document.
     */
    std::vector<BSONObj> createDocuments(int num);

    void createIndex(const BSONObj& spec);

    /**
     * Generate a vector of BSONObj based on the input data.
     *
     * @param data is a vector of vector of values. The inner vectors represent the values for
     * each field (i.e., a column), and the outer vector represents a collection of fields.
     * @param fieldConfig The function combines of the provided values from "data" according the the
     * vector of field configurations, each field configuration represents a field. Specifically, it
     * extracts the field's name and position in the collection.
     *
     * The collection will contain as many fields as the maximum position of the user defined
     * fields in "fieldConfig". The remaining in-between fields are copies of the user defined
     * fields with names with suffix an underscore and a number. When defining the fields in the
     * collection, order the fields in increasing position order.
     */
    static std::vector<BSONObj> createDocumentsFromSBEValue(
        std::vector<std::vector<stats::SBEValue>> data,
        std::vector<CollectionFieldConfiguration> fieldConfig);

    static CardinalityEstimate makeCardinalityEstimate(
        double estimate,
        cost_based_ranker::EstimationSource source = cost_based_ranker::EstimationSource::Code) {
        return CardinalityEstimate(CardinalityType{estimate}, source);
    }

    void _doTest() override {}

    OperationContext* getOperationContext() const {
        return operationContext();
    }

    /**
     * Helper to create sampling estimator for unit tests.
     */
    SamplingEstimatorForTesting createSamplingEstimatorForTesting(
        size_t collCard, size_t sampleSize, ce::ProjectionParams projectionParams);

    NamespaceString _kTestNss;
};

enum class SampleSizeDef { ErrorSetting1 = 1, ErrorSetting2 = 2, ErrorSetting5 = 3 };
enum class IndexCombinationTestSettings { kNone = 0, kSingleFieldIdx = 1, kAllIdxes = 2 };

struct PlanRankingExecutionStatistics {
    std::vector<double> multiplannerExecTimes;
    std::map<int, std::map<SampleSizeDef, std::vector<double>>> cbrExecTimes;
    std::map<SampleSizeDef, std::vector<double>> bareCQEvalExecTimes;
    std::map<SampleSizeDef, int> cbrSampleSizes;
    std::vector<double> selectivities;
};

size_t translateSampleDefToActualSampleSize(SampleSizeDef sampleSizeDef);

std::pair<SamplingEstimatorImpl::SamplingStyle, boost::optional<int>>
iniitalizeSamplingAlgoBasedOnChunks(int numOfChunks);

/**
 * Sampling accuracy test extension used as a vessel to generate samples over collections and
 * calculate the accuracy of cardinality estimation over those samples.
 * Each test instance represents a specific dataset (collection) configuration (size, data types)
 * and can run a variety of sample and query configurations
 */
class SamplingAccuracyTest : public CatalogTestFixture {
public:
    SamplingAccuracyTest() : CatalogTestFixture() {}

    void runSamplingEstimatorTestConfiguration(
        DataConfiguration dataConfig,
        WorkloadConfiguration queryConfig,
        std::vector<SampleSizeDef> sampleSizes,
        std::vector<std::pair<SamplingEstimatorImpl::SamplingStyle, boost::optional<int>>>
            samplingAlgoAndChunks,
        bool printResults = true);
};

/**
 * Compare the set of fields the input workload configuration requires with the provided data
 * configuration. Returns true if all the fields required by the workload configuration appear in
 * the data configuration.
 */
bool dataConfigurationCoversQueryWorkload(DataConfiguration& dataConfig,
                                          WorkloadConfiguration& workloadConfig);

void initializeSamplingEstimator(DataConfiguration& configuration,
                                 SamplingEstimatorTest& samplingEstimatorTest);


void createCollAndInsertDocuments(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const std::vector<BSONObj>& docs);

/**
 * Given a MatchExpression and a vector of BSONObj, evaluate the MatchExpression against all the
 * BSONObjs and return the number of matching documents.
 * This function also takes an optional argument to limit the number of documents to evaluate, if it
 * is set, the function will only evaluate the 'limit' first documents from the vector.
 */
int evaluateMatchExpressionAgainstDataWithLimit(std::unique_ptr<MatchExpression> expr,
                                                const std::vector<BSONObj>& data,
                                                boost::optional<size_t> limit = boost::none);

/**
 * Given a set of fields that are candidates to be used to build indexes on, create a variety of
 * combinations of said fields defining single field and composite indexes. The second argument
 * "IndexCombinationTestSetting" defines the variety of combinations to create. 'kNone' will not
 * create any index, 'kSingleFieldIdx' will return only the single field indexes i.e., a vector of
 * vectors containing one object, 'kAllIndexes' will create all *in-sequence* combinations of the
 * set of given fields.
 */
std::vector<std::vector<std::string>> getIndexCombinations(
    const std::vector<std::string>& candidateIndexFields, IndexCombinationTestSettings setting);

std::string createIndexesAccordingToConfiguration(
    SamplingEstimatorTest& planRankingTest,
    std::vector<std::vector<std::string>>& indexCombinations);

std::unique_ptr<CanonicalQuery> createCanonicalQueryFromMatchExpression(
    SamplingEstimatorTest& planRankingTest, std::unique_ptr<MatchExpression> matchExpression);

ErrorCalculationSummary runQueries(WorkloadConfiguration queryConfig,
                                   std::vector<BSONObj>& bsonData,
                                   const SamplingEstimatorImpl* ceSample);

void printResult(DataConfiguration dataConfig,
                 int sampleSize,
                 WorkloadConfiguration queryConfig,
                 const std::pair<SamplingEstimatorImpl::SamplingStyle, boost::optional<int>>&
                     samplingAlgoAndChunks,
                 ErrorCalculationSummary error);

/**
 * Helper function to create index bounds from generated query intervals.
 */
IndexBounds getIndexBounds(const QueryConfiguration& queryConfig,
                           std::vector<std::pair<stats::SBEValue, stats::SBEValue>>& intervals);
}  // namespace mongo::ce
