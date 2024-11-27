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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/ce/sampling_estimator_impl.h"
#include "mongo/db/query/cost_based_ranker/estimates.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::ce {

using namespace mongo::cost_based_ranker;

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const size_t kSampleSize = 5;

class SamplingEstimatorForTesting : public SamplingEstimatorImpl {
public:
    using SamplingEstimatorImpl::SamplingEstimatorImpl;

    const std::vector<BSONObj>& getSample() {
        return _sample;
    }

    static std::unique_ptr<CanonicalQuery> makeCanonicalQuery(const NamespaceString& nss,
                                                              OperationContext* opCtx,
                                                              size_t sampleSize) {
        return SamplingEstimatorImpl::makeCanonicalQuery(nss, opCtx, sampleSize);
    }

    static size_t calculateSampleSize(SamplingConfidenceIntervalEnum ci, double marginOfError) {
        return SamplingEstimatorImpl::calculateSampleSize(ci, marginOfError);
    }

    double getCollCard() {
        return SamplingEstimatorImpl::getCollCard();
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
        double maxCard = std::min(card + moe * collCard, collCard);

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
        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), kTestNss, CollectionOptions()));
    }

    void tearDown() override {
        CatalogTestFixture::tearDown();
    }

    /*
     * Insert documents to the default collection.
     */
    void insertDocuments(const NamespaceString& nss, const std::vector<BSONObj> docs) {
        std::vector<InsertStatement> inserts{docs.begin(), docs.end()};

        AutoGetCollection agc(operationContext(), nss, LockMode::MODE_IX);
        {
            WriteUnitOfWork wuow{operationContext()};
            ASSERT_OK(collection_internal::insertDocuments(
                operationContext(), *agc, inserts.begin(), inserts.end(), nullptr /* opDebug */));
            wuow.commit();
        }
    }

    std::vector<BSONObj> createDocuments(int num) {
        std::vector<BSONObj> docs;
        for (int i = 0; i < num; i++) {
            BSONObj obj = BSON("_id" << i << "a" << i % 100 << "b" << i % 10);
            docs.push_back(obj);
        }
        return docs;
    }

    static CardinalityEstimate makeCardinalityEstimate(
        double estimate, EstimationSource source = EstimationSource::Code) {
        return CardinalityEstimate(CardinalityType{estimate}, source);
    }
};

TEST_F(SamplingEstimatorTest, SamplingCanonicalQueryTest) {
    const int64_t sampleSize = 500;
    // The samplingCQ is a different CQ than the one for the query being optimized. 'samplingCQ'
    // should contain information about the sample size and the same nss as the CQ for the query
    // being optimized.
    auto samplingCQ =
        SamplingEstimatorForTesting::makeCanonicalQuery(kTestNss, operationContext(), sampleSize);
    ASSERT_EQUALS(samplingCQ->getFindCommandRequest().getLimit(), sampleSize);
    ASSERT_EQUALS(kTestNss, samplingCQ->nss());
}

TEST_F(SamplingEstimatorTest, RandomSamplingProcess) {
    insertDocuments(kTestNss, createDocuments(10));

    AutoGetCollection collPtr(operationContext(), kTestNss, LockMode::MODE_IX);
    auto colls = MultipleCollectionAccessor(operationContext(),
                                            &collPtr.getCollection(),
                                            kTestNss,
                                            false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */,
                                            {});

    SamplingEstimatorForTesting samplingEstimator(operationContext(),
                                                  colls,
                                                  kSampleSize,
                                                  SamplingEstimatorImpl::SamplingStyle::kRandom,
                                                  makeCardinalityEstimate(10));

    auto sample = samplingEstimator.getSample();
    ASSERT_EQUALS(sample.size(), kSampleSize);
}

TEST_F(SamplingEstimatorTest, DrawANewSample) {
    insertDocuments(kTestNss, createDocuments(10));

    AutoGetCollection collPtr(operationContext(), kTestNss, LockMode::MODE_IX);
    auto colls = MultipleCollectionAccessor(operationContext(),
                                            &collPtr.getCollection(),
                                            kTestNss,
                                            false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */,
                                            {});

    // A sample was generated on construction with size being the pre-determined size.
    SamplingEstimatorForTesting samplingEstimator(
        operationContext(),
        colls,
        kSampleSize,
        SamplingEstimatorForTesting::SamplingStyle::kRandom,
        makeCardinalityEstimate(10));

    auto sample = samplingEstimator.getSample();
    ASSERT_EQUALS(sample.size(), kSampleSize);

    // Specifing a new sample size and re-sample. The old sample should be replaced by the new
    // sample of a different sample size.
    samplingEstimator.generateRandomSample(3);
    auto newSample = samplingEstimator.getSample();
    ASSERT_EQUALS(newSample.size(), 3);
}

TEST_F(SamplingEstimatorTest, SampleSize) {
    std::map<std::pair<SamplingConfidenceIntervalEnum, double>, size_t> sampleSizes = {
        {std::make_pair(SamplingConfidenceIntervalEnum::k90, 2), 1691},
        {std::make_pair(SamplingConfidenceIntervalEnum::k95, 2), 2401},
        {std::make_pair(SamplingConfidenceIntervalEnum::k99, 2), 4147},
        {std::make_pair(SamplingConfidenceIntervalEnum::k90, 5), 271},
        {std::make_pair(SamplingConfidenceIntervalEnum::k95, 5), 384},
        {std::make_pair(SamplingConfidenceIntervalEnum::k99, 5), 664},
    };
    for (auto& el : sampleSizes) {
        auto size =
            SamplingEstimatorForTesting::calculateSampleSize(el.first.first, el.first.second);
        ASSERT_EQUALS(size, el.second);
    }
}

TEST_F(SamplingEstimatorTest, SampleSizeError) {
    const size_t card = 100;
    const size_t sampleSize = 400;

    AutoGetCollection collPtr(operationContext(), kTestNss, LockMode::MODE_IX);
    auto colls = MultipleCollectionAccessor(operationContext(),
                                            &collPtr.getCollection(),
                                            kTestNss,
                                            false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */,
                                            {});

    ASSERT_THROWS_CODE(SamplingEstimatorImpl(operationContext(),
                                             colls,
                                             sampleSize,
                                             SamplingEstimatorImpl::SamplingStyle::kRandom,
                                             makeCardinalityEstimate(card)),
                       DBException,
                       9406300);
}

TEST_F(SamplingEstimatorTest, EstimateCardinality) {
    const size_t card = 4000;
    insertDocuments(kTestNss, createDocuments(card));
    const size_t sampleSize = 400;

    AutoGetCollection collPtr(operationContext(), kTestNss, LockMode::MODE_IX);
    auto colls = MultipleCollectionAccessor(operationContext(),
                                            &collPtr.getCollection(),
                                            kTestNss,
                                            false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */,
                                            {});

    SamplingEstimatorForTesting samplingEstimator(
        operationContext(),
        colls,
        sampleSize,
        SamplingEstimatorForTesting::SamplingStyle::kRandom,
        makeCardinalityEstimate(card));

    {  // All documents in the collection satisfy the predicate.
        auto operand = BSON("$lt" << 100);
        LTMatchExpression lt("a"_sd, operand["$lt"]);
        auto cardinalityEstimate = samplingEstimator.estimateCardinality(&lt);

        CardinalityEstimate expectedEstimate =
            makeCardinalityEstimate(samplingEstimator.getCollCard());
        ASSERT_TRUE(cardinalityEstimate == expectedEstimate);
    }

    {  // Predicate with 50% selectivity.
        auto operand = BSON("$lt" << 50);
        LTMatchExpression lt("a"_sd, operand["$lt"]);
        auto cardinalityEstimate = samplingEstimator.estimateCardinality(&lt);

        samplingEstimator.assertEstimateInConfidenceInterval(cardinalityEstimate, 0.5 * card);
    }

    {  // Predicate with 20% selectivity.
        auto operand = BSON("$gte" << 80);
        GTEMatchExpression gte("a"_sd, operand["$gte"]);
        auto cardinalityEstimate = samplingEstimator.estimateCardinality(&gte);

        samplingEstimator.assertEstimateInConfidenceInterval(cardinalityEstimate, 0.2 * card);
    }

    {  // Equality predicate with 10% selectivity.
        auto operand = BSON("$eq" << 5);
        EqualityMatchExpression eq("b"_sd, operand["$eq"]);
        auto cardinalityEstimate = samplingEstimator.estimateCardinality(&eq);

        samplingEstimator.assertEstimateInConfidenceInterval(cardinalityEstimate, 0.1 * card);
    }
}

TEST_F(SamplingEstimatorTest, EstimateCardinalityLogicalExpressions) {
    const size_t card = 4000;
    insertDocuments(kTestNss, createDocuments(card));
    const size_t sampleSize = 400;

    AutoGetCollection collPtr(operationContext(), kTestNss, LockMode::MODE_IX);
    auto colls = MultipleCollectionAccessor(operationContext(),
                                            &collPtr.getCollection(),
                                            kTestNss,
                                            false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */,
                                            {});

    SamplingEstimatorForTesting samplingEstimator(
        operationContext(),
        colls,
        sampleSize,
        SamplingEstimatorForTesting::SamplingStyle::kRandom,
        makeCardinalityEstimate(card));

    {  // Range predicate on "a" with 20% selectivity: a > 40 && a < 60.
        auto operand1 = BSON("$gte" << 40);
        auto operand2 = BSON("$lt" << 60);

        auto pred1 = std::make_unique<GTEMatchExpression>("a"_sd, operand1["$gte"]);
        auto pred2 = std::make_unique<LTMatchExpression>("a"_sd, operand2["$lt"]);

        auto andExpr = AndMatchExpression{};
        andExpr.add(std::move(pred1));
        andExpr.add(std::move(pred2));

        auto cardinalityEstimate = samplingEstimator.estimateCardinality(&andExpr);
        samplingEstimator.assertEstimateInConfidenceInterval(cardinalityEstimate, 0.2 * card);
    }

    {  // Conjunction of two predicates with ~4% selectivity: a < 40 && b = 5.
        auto operand1 = BSON("$lt" << 40);
        auto operand2 = BSON("$eq" << 5);

        auto pred1 = std::make_unique<LTMatchExpression>("a"_sd, operand1["$lt"]);
        auto pred2 = std::make_unique<EqualityMatchExpression>("b"_sd, operand2["$eq"]);

        auto andExpr = AndMatchExpression{};
        andExpr.add(std::move(pred1));
        andExpr.add(std::move(pred2));

        auto cardinalityEstimate = samplingEstimator.estimateCardinality(&andExpr);
        samplingEstimator.assertEstimateInConfidenceInterval(cardinalityEstimate, 0.04 * card);
    }

    {  // Disjunction of two predicates with ~28% selectivity: a < 20 || b = 5.
        auto operand1 = BSON("$lt" << 20);
        auto operand2 = BSON("$eq" << 5);

        auto pred1 = std::make_unique<LTMatchExpression>("a"_sd, operand1["$lt"]);
        auto pred2 = std::make_unique<EqualityMatchExpression>("b"_sd, operand2["$eq"]);

        auto orExpr = OrMatchExpression{};
        orExpr.add(std::move(pred1));
        orExpr.add(std::move(pred2));

        auto cardinalityEstimate = samplingEstimator.estimateCardinality(&orExpr);
        samplingEstimator.assertEstimateInConfidenceInterval(cardinalityEstimate, 0.28 * card);
    }

    {  // Exclusive disjunction of two predicates with ~20% selectivity: b = 3 || b = 5.
        auto operand1 = BSON("$eq" << 3);
        auto operand2 = BSON("$eq" << 5);

        auto pred1 = std::make_unique<EqualityMatchExpression>("b"_sd, operand1["$eq"]);
        auto pred2 = std::make_unique<EqualityMatchExpression>("b"_sd, operand2["$eq"]);

        auto orExpr = OrMatchExpression{};
        orExpr.add(std::move(pred1));
        orExpr.add(std::move(pred2));

        auto cardinalityEstimate = samplingEstimator.estimateCardinality(&orExpr);
        samplingEstimator.assertEstimateInConfidenceInterval(cardinalityEstimate, 0.20 * card);
    }
}

TEST_F(SamplingEstimatorTest, EstimateCardinalityMultipleExpressions) {
    const size_t card = 4000;
    insertDocuments(kTestNss, createDocuments(card));
    const size_t sampleSize = 400;

    AutoGetCollection collPtr(operationContext(), kTestNss, LockMode::MODE_IX);
    auto colls = MultipleCollectionAccessor(operationContext(),
                                            &collPtr.getCollection(),
                                            kTestNss,
                                            false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */,
                                            {});

    SamplingEstimatorForTesting samplingEstimator(
        operationContext(),
        colls,
        sampleSize,
        SamplingEstimatorForTesting::SamplingStyle::kRandom,
        makeCardinalityEstimate(card));

    auto operand1 = BSON("$lt" << 30);
    LTMatchExpression lt("a"_sd, operand1["$lt"]);
    auto operand2 = BSON("$gt" << 8);
    GTMatchExpression gt("b"_sd, operand2["$gt"]);

    auto operand3 = BSON("$lte" << 12);
    auto operand4 = BSON("$eq" << 99);
    auto pred1 = std::make_unique<LTEMatchExpression>("a"_sd, operand3["$lte"]);
    auto pred2 = std::make_unique<EqualityMatchExpression>("a"_sd, operand4["$eq"]);
    auto orExpr = OrMatchExpression{};
    orExpr.add(std::move(pred1));
    orExpr.add(std::move(pred2));

    std::vector<MatchExpression*> expressions;
    expressions.push_back(&lt);
    expressions.push_back(&gt);
    expressions.push_back(&orExpr);

    std::vector<double> expectedSel = {0.3, 0.1, 0.14};

    auto estimates = samplingEstimator.estimateCardinality(expressions);

    for (size_t i = 0; i < estimates.size(); i++) {
        samplingEstimator.assertEstimateInConfidenceInterval(estimates[i], expectedSel[i] * card);
    }
}
}  // namespace mongo::ce
