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
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/ce/sampling_estimator.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"


namespace mongo::ce {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const size_t kSampleSize = 5;

class SamplingEstimatorForTesting : public SamplingEstimator {
public:
    using SamplingEstimator::SamplingEstimator;

    const std::vector<BSONObj>& getSample() {
        return _sample;
    }

    static std::unique_ptr<CanonicalQuery> makeCanonicalQuery(const NamespaceString& nss,
                                                              OperationContext* opCtx,
                                                              const size_t sampleSize) {
        return SamplingEstimator::makeCanonicalQuery(nss, opCtx, sampleSize);
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
            BSONObj obj = BSON("_id" << i);
            docs.push_back(obj);
        }
        return docs;
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

    SamplingEstimatorForTesting samplingEstimator(
        operationContext(), colls, kSampleSize, SamplingEstimator::SamplingStyle::kRandom);

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
        operationContext(), colls, kSampleSize, SamplingEstimator::SamplingStyle::kRandom);

    auto sample = samplingEstimator.getSample();
    ASSERT_EQUALS(sample.size(), kSampleSize);

    // Specifing a new sample size and re-sample. The old sample should be replaced by the new
    // sample of a different sample size.
    samplingEstimator.generateRandomSample(3);
    auto newSample = samplingEstimator.getSample();
    ASSERT_EQUALS(newSample.size(), 3);
}

}  // namespace mongo::ce
