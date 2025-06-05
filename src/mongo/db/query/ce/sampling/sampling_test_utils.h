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
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/ce/ce_test_utils.h"
#include "mongo/db/query/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/stats/value_utils.h"
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
            BSONObj obj = BSON("_id" << i << "a" << i % 100 << "b" << i % 10 << "arr"
                                     << BSON_ARRAY(10 << 20 << 30 << 40 << 50) << "nil" << BSONNULL
                                     << "obj" << BSON("nil" << BSONNULL));
            docs.push_back(obj);
        }
        return docs;
    }

    static std::vector<BSONObj> createDocumentsFromSBEValue(std::vector<stats::SBEValue> data,
                                                            int numberOfFields = 1) {

        tassert(10472401,
                "Number of fields should be fewer than the letters of the English alphabet (<= 26)",
                numberOfFields <= 26);

        std::vector<BSONObj> docs;
        for (size_t i = 0; i < data.size(); i++) {

            BSONObjBuilder builder;
            stats::addSbeValueToBSONBuilder(stats::makeInt64Value(i), "_id", builder);

            for (int instance = 0; instance < numberOfFields; instance++) {
                std::string attributeName;

                // Create simple attribute names starting from 'a' based on ascii codes (i.e.,
                // starting from ascii 97 (a) to ascii 122 (z))
                attributeName = (char)(97 + instance);
                stats::addSbeValueToBSONBuilder(data[i], attributeName, builder);
            }

            docs.push_back(builder.obj());
        }
        return docs;
    }

    static BSONObj createBSONObjOperandWithSBEValue(std::string str, stats::SBEValue value) {
        BSONObjBuilder builder;
        stats::addSbeValueToBSONBuilder(value, str, builder);
        return builder.obj();
    }

    static CardinalityEstimate makeCardinalityEstimate(
        double estimate,
        cost_based_ranker::EstimationSource source = cost_based_ranker::EstimationSource::Code) {
        return CardinalityEstimate(CardinalityType{estimate}, source);
    }

    void _doTest() override {}

    OperationContext* getOperationContext() const {
        return operationContext();
    }

    NamespaceString _kTestNss;
};

struct SamplingEstimationBenchmarkConfiguration : public BenchmarkConfiguration {

    int numberOfFields;
    int sampleSize;
    boost::optional<int> numChunks;
    SamplingEstimatorImpl::SamplingStyle samplingAlgo;

    SamplingEstimationBenchmarkConfiguration(size_t size,
                                             DataDistributionEnum dataDistribution,
                                             DataType dataType,
                                             boost::optional<size_t> ndv,
                                             boost::optional<QueryType> queryType,
                                             size_t numberOfFields,
                                             size_t kSampleSize,
                                             int numOfChunks,
                                             boost::optional<size_t> numberOfQueries = boost::none)
        : BenchmarkConfiguration(size, dataDistribution, dataType, queryType, ndv, numberOfQueries),
          numberOfFields(numberOfFields),
          sampleSize(kSampleSize) {

        // Translate the number of chunks variable to both number of chunks and sampling algo.
        // This benchmark given as input numOfChunks <= 0 will use kRandom.
        if (numOfChunks <= 0) {
            samplingAlgo = SamplingEstimatorImpl::SamplingStyle::kRandom;
            numChunks = boost::none;
        } else {
            samplingAlgo = SamplingEstimatorImpl::SamplingStyle::kChunk;
            numChunks = numOfChunks;
        }
    };
};


void generateData(SamplingEstimationBenchmarkConfiguration& configuration,
                  size_t seedData,
                  std::vector<stats::SBEValue>& data);

}  // namespace mongo::ce
