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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/ce/sampling_estimator.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::optimizer::ce {

class SamplingEstimatorForTesting : public SamplingEstimator {
public:
    const CanonicalQuery& getCanonicalQuery() {
        return *_cq;
    }
    using SamplingEstimator::SamplingEstimator;
};

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

class SamplingEstimatorTest : public ServiceContextTest {
public:
    void setUp() override {
        _opCtx = makeOperationContext();
        CollectionMock coll(kTestNss);
        CollectionPtr collPtr(&coll);
        _colls = MultipleCollectionAccessor(_opCtx.get(),
                                            &collPtr,
                                            kTestNss,
                                            false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */,
                                            {});
    }

    void tearDown() override {
        _opCtx.reset();
    }

protected:
    ServiceContext::UniqueOperationContext _opCtx;
    MultipleCollectionAccessor _colls;
};

TEST_F(SamplingEstimatorTest, SamplingCanonicalQueryTest) {
    // A sample as well as the CanonicalQuery required is generated on construction.
    SamplingEstimatorForTesting samplingEstimator(
        _opCtx.get(), kTestNss, _colls, SamplingEstimator::SamplingStyle::kRandom);
    // The samplingCQ is a different CQ than the one for the query being optimized. 'samplingCQ'
    // should contain information about the sample size and the same nss as the CQ for the query
    // being optimized.
    const CanonicalQuery& samplingCQ = samplingEstimator.getCanonicalQuery();
    // TODO SERVER-94063: Update the sample size. 500 is the current default amount.
    int64_t sampleSize = 500;
    ASSERT_EQUALS(*samplingCQ.getFindCommandRequest().getLimit(), sampleSize);
    ASSERT_EQUALS(kTestNss, samplingCQ.nss());
}

}  // namespace mongo::optimizer::ce
