// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Tests for serving NDV estimates from persisted field statistics (analyze mode "ndv") in
 * SamplingEstimatorImpl::estimateNDV(), including all fallbacks to the sample-based estimate.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/ce/ndv/field_stats.h"
#include "mongo/db/query/compiler/ce/ndv/field_stats_gen.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mongo::ce {

class PersistentNDVTest : public SamplingEstimatorTest {
protected:
    static constexpr size_t kCollCard = 500;
    static constexpr size_t kSampleSize = 100;
    // Distinct values of field "a" in the test data; far from the persisted values below, so a
    // test can always tell which path produced an estimate.
    static constexpr int kDataNDV = 7;
    static constexpr long long kPersistedNDV = 123;
    // Fixed creation time so metadata assertions can compare exactly.
    const Date_t kCreatedAt = Date_t::fromMillisSinceEpoch(42000);

    void setUp() override {
        SamplingEstimatorTest::setUp();
        insertTestDocuments();
    }

    void insertTestDocuments() {
        std::vector<BSONObj> docs;
        for (size_t i = 0; i < kCollCard; ++i) {
            docs.push_back(BSON("_id" << static_cast<int>(i) << "a"
                                      << static_cast<int>(i % kDataNDV) << "b"
                                      << static_cast<int>(i)));
        }
        insertDocuments(_kTestNss, std::move(docs));
    }

    CollectionOrViewAcquisition acquireTestCollection() {
        return acquireCollectionOrView(
            operationContext(),
            CollectionOrViewAcquisitionRequest::fromOpCtx(
                operationContext(), _kTestNss, AcquisitionPrerequisites::kRead),
            LockMode::MODE_IS);
    }

    UUID collectionUuid() {
        return acquireTestCollection().getCollectionPtr()->uuid();
    }

    NamespaceString statsNss() {
        return NamespaceString::createNamespaceString_forTest(
            _kTestNss.dbName(), NamespaceString::kStatsFieldStatsCollectionName);
    }

    void insertStatsDoc(const BSONObj& doc) {
        const auto status =
            storageInterface()->createCollection(operationContext(), statsNss(), {});
        if (status != ErrorCodes::NamespaceExists) {
            ASSERT_OK(status);
        }
        insertDocuments(statsNss(), {doc});
    }

    /**
     * Persists a well-formed field-stats document for 'path' of the test collection.
     */
    BSONObj makeStatsDoc(long long ndv,
                         const std::string& path = "a",
                         boost::optional<UUID> uuid = boost::none) {
        const UUID collUuid = uuid.value_or(collectionUuid());

        NdvSketch sketch;
        sketch.setNdv(ndv);
        sketch.setRegisters(std::vector<std::uint8_t>(1 << 14));
        sketch.setPrecision(14);
        NdvStats ndvStats;
        ndvStats.setSketches({std::move(sketch)});

        FieldStatsDoc doc;
        doc.set_id(makeFieldStatsId(collUuid, {path}));
        doc.setSchemaVersion(kFieldStatsSchemaVersion);
        doc.setCollectionUuid(collUuid);
        doc.setSortedFieldPaths(std::vector<std::string>{path});
        doc.setCreatedAt(kCreatedAt);
        doc.setNdv(std::move(ndvStats));
        return doc.toBSON();
    }

    void persistNDV(long long ndv,
                    const std::string& path = "a",
                    boost::optional<UUID> uuid = boost::none) {
        insertStatsDoc(makeStatsDoc(ndv, path, uuid));
    }

    /**
     * Persists a document for path "a" whose ndv section carries 'sketchCount' sketches. A
     * single-field statistic must carry exactly one; other counts are malformed.
     */
    void persistWithSketchCount(int sketchCount) {
        const UUID collUuid = collectionUuid();

        NdvStats ndvStats;
        std::vector<NdvSketch> sketches;
        for (int i = 0; i < sketchCount; ++i) {
            NdvSketch sketch;
            sketch.setNdv(kPersistedNDV);
            sketch.setRegisters(std::vector<std::uint8_t>(1 << 14));
            sketch.setPrecision(14);
            sketches.push_back(std::move(sketch));
        }
        ndvStats.setSketches(std::move(sketches));

        FieldStatsDoc doc;
        doc.set_id(makeFieldStatsId(collUuid, {"a"}));
        doc.setSchemaVersion(kFieldStatsSchemaVersion);
        doc.setCollectionUuid(collUuid);
        doc.setSortedFieldPaths(std::vector<std::string>{"a"});
        doc.setCreatedAt(Date_t::now());
        doc.setNdv(std::move(ndvStats));
        insertStatsDoc(doc.toBSON());
    }

    /**
     * Runs 'fn' with an estimator over the test collection and a generated sample. The estimator
     * holds references to the collection acquisition, so all three must share the same scope.
     */
    template <typename F>
    void withEstimator(F&& fn) {
        auto coll = acquireTestCollection();
        auto colls = MultipleCollectionAccessor(
            coll, {}, false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */);

        SamplingEstimatorForTesting estimator(operationContext(),
                                              colls,
                                              _kTestNss,
                                              PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                              kSampleSize,
                                              SamplingCEMethodEnum::kRandom,
                                              boost::none /* numChunks */,
                                              makeCardinalityEstimate(kCollCard),
                                              nullptr /* customerQueryExpCtx */);
        estimator.generateSample(ce::NoProjection{});
        fn(estimator);
    }

    /**
     * Shorthand for the single-field estimate the tests assert on.
     */
    double estimateForA() {
        double result = 0;
        withEstimator([&](SamplingEstimatorForTesting& estimator) {
            result = estimator.estimateNDV({{.path = "a"}}).toDouble();
        });
        return result;
    }

    unittest::ServerParameterGuard _featureFlag{"featureFlagPersistentStats", true};
    unittest::ServerParameterGuard _knob{"internalQueryEnablePersistentNDVStats", true};
};

TEST_F(PersistentNDVTest, MetadataRecordsServedPaths) {
    persistNDV(kPersistedNDV);
    withEstimator([&](SamplingEstimatorForTesting& estimator) {
        ASSERT_EQ(estimator.getNumPersistedNDVStatsUsed(), 0u);

        // Two calls, one metadata entry: memoized re-reads must not duplicate it.
        estimator.estimateNDV({{.path = "a"}});
        estimator.estimateNDV({{.path = "a"}});
        ASSERT_EQ(estimator.getNumPersistedNDVStatsUsed(), 1u);

        const auto meta = estimator.getPersistedNDVMetadata();
        ASSERT_EQ(meta.size(), 1u);
        ASSERT_EQ(meta.front().sortedFieldPaths, std::vector<std::string>{"a"});
        ASSERT_EQ(meta.front().createdAt, kCreatedAt);
    });
}

TEST_F(PersistentNDVTest, MetadataIsSortedByFieldPaths) {
    persistNDV(kPersistedNDV, "a");
    persistNDV(kPersistedNDV, "b");
    withEstimator([&](SamplingEstimatorForTesting& estimator) {
        // Request in reverse order; the metadata must come back sorted.
        estimator.estimateNDV({{.path = "b"}});
        estimator.estimateNDV({{.path = "a"}});

        const auto meta = estimator.getPersistedNDVMetadata();
        ASSERT_EQ(meta.size(), 2u);
        ASSERT_EQ(meta[0].sortedFieldPaths, std::vector<std::string>{"a"});
        ASSERT_EQ(meta[1].sortedFieldPaths, std::vector<std::string>{"b"});
    });
}

TEST_F(PersistentNDVTest, MetadataEmptyWithoutServedEstimates) {
    // No persisted statistics: the fallback estimate must not record metadata.
    withEstimator([&](SamplingEstimatorForTesting& estimator) {
        estimator.estimateNDV({{.path = "a"}});
        ASSERT_TRUE(estimator.getPersistedNDVMetadata().empty());
    });
}

TEST_F(PersistentNDVTest, ServesPersistedNDV) {
    persistNDV(kPersistedNDV);
    ASSERT_EQ(estimateForA(), kPersistedNDV);
}

TEST_F(PersistentNDVTest, FallsBackWhenDisabled) {
    persistNDV(kPersistedNDV);

    {
        unittest::ServerParameterGuard knobOff("internalQueryEnablePersistentNDVStats", false);
        ASSERT_NE(estimateForA(), kPersistedNDV);
    }
    {
        unittest::ServerParameterGuard flagOff("featureFlagPersistentStats", false);
        ASSERT_NE(estimateForA(), kPersistedNDV);
    }
}

TEST_F(PersistentNDVTest, ServesBothEqualitySemanticsIdentically) {
    // The persisted sketch counts null and missing as distinct values ($expr semantics). Regular
    // equality (null == missing) gets the same number, off by at most one, which the design
    // accepts for single fields. This test pins that both semantics are served from the sketch.
    persistNDV(kPersistedNDV);
    withEstimator([&](SamplingEstimatorForTesting& estimator) {
        ASSERT_EQ(estimator.estimateNDV({{.path = "a", .isExprEq = true}}).toDouble(),
                  kPersistedNDV);
        ASSERT_EQ(estimator.estimateNDV({{.path = "a", .isExprEq = false}}).toDouble(),
                  kPersistedNDV);
    });
}

TEST_F(PersistentNDVTest, FallsBackForMultipleFields) {
    persistNDV(kPersistedNDV);
    withEstimator([&](SamplingEstimatorForTesting& estimator) {
        ASSERT_NE(estimator.estimateNDV({{.path = "a"}, {.path = "b"}}).toDouble(), kPersistedNDV);
    });
}

TEST_F(PersistentNDVTest, FallsBackWithBounds) {
    persistNDV(kPersistedNDV);
    withEstimator([&](SamplingEstimatorForTesting& estimator) {
        OrderedIntervalList oil("a");
        oil.intervals.push_back(Interval(BSON("" << 0 << "" << 3), true, true));
        const std::vector<OrderedIntervalList> bounds{oil};
        ASSERT_NE(estimator.estimateNDV({{.path = "a"}}, std::span(bounds)).toDouble(),
                  kPersistedNDV);
    });
}

TEST_F(PersistentNDVTest, FallsBackWithoutStatsDoc) {
    // No stats persisted at all: the estimate comes from the sample and is near the real NDV.
    const auto estimate = estimateForA();
    ASSERT_NE(estimate, kPersistedNDV);
    ASSERT_LTE(estimate, kCollCard);
}

TEST_F(PersistentNDVTest, FallsBackForDifferentCollectionUuid) {
    // Statistics persisted for a dropped incarnation of the collection must not be served.
    persistNDV(kPersistedNDV, "a", UUID::gen());
    ASSERT_NE(estimateForA(), kPersistedNDV);
}

TEST_F(PersistentNDVTest, ClampsToCollectionCardinality) {
    persistNDV(10 * kCollCard);
    ASSERT_EQ(estimateForA(), kCollCard);
}

TEST_F(PersistentNDVTest, ZeroSketchesFallsBack) {
    persistWithSketchCount(0);
    ASSERT_NE(estimateForA(), kPersistedNDV);
}

TEST_F(PersistentNDVTest, MultipleSketchesFallBack) {
    persistWithSketchCount(2);
    ASSERT_NE(estimateForA(), kPersistedNDV);
}

TEST_F(PersistentNDVTest, NegativeNdvFallsBack) {
    persistNDV(-1);
    const auto estimate = estimateForA();
    ASSERT_GT(estimate, 0);
    ASSERT_LTE(estimate, kCollCard);
}

TEST_F(PersistentNDVTest, MalformedStatsDocFallsBack) {
    // A document with the right _id but the wrong schema must degrade to the sample-based
    // estimate, not fail the query.
    insertStatsDoc(BSON("_id" << makeFieldStatsId(collectionUuid(), {"a"}) << "garbage" << 1));
    ASSERT_NE(estimateForA(), kPersistedNDV);
}

TEST_F(PersistentNDVTest, MissingNdvSectionFallsBack) {
    // The ndv section is required by the schema, so a stored document lacking it fails to
    // parse on the read path and must not be served.
    insertStatsDoc(makeStatsDoc(kPersistedNDV).removeField("ndv"));
    ASSERT_NE(estimateForA(), kPersistedNDV);
}

}  // namespace mongo::ce
