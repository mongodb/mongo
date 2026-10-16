// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/sample_manager.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"
#include "mongo/db/query/compiler/ce/sampling/sample_spec.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <string>
#include <vector>

namespace mongo::ce {
namespace {

SampleSpec makeSpec(const NamespaceString& nss,
                    size_t sampleSize,
                    SamplingCEMethodEnum method = SamplingCEMethodEnum::kRandom,
                    SamplingSourceEnum source = SamplingSourceEnum::kPersistentSample,
                    boost::optional<int> numChunks = boost::none) {
    return SampleSpec{
        .nss = nss,
        .requestedSampleSize = sampleSize,
        .onTheFlySamplingMethod = method,
        .persistentSamplingMethod = method,
        .numChunks = numChunks,
        .samplingSource = source,
        .topLevelSampleFieldNames = {},
    };
}

size_t countSamplesCollDocs(OperationContext* opCtx, const NamespaceString& samplesNss) {
    DBDirectClient client(opCtx);
    return static_cast<size_t>(client.count(samplesNss));
}

CollectionOrViewAcquisition acquireCollectionForSampling(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    return acquireCollectionOrView(
        opCtx,
        CollectionOrViewAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_IX);
}

}  // namespace

TEST_F(SamplingEstimatorTest, SampleManagerGeneratesOnTheFlyWhenNoPersistedSample) {
    // TODO SERVER-124372: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss,
                    {BSON("_id" << 1 << "tag" << "live"),
                     BSON("_id" << 2 << "tag" << "live"),
                     BSON("_id" << 3 << "tag" << "live"),
                     BSON("_id" << 4 << "tag" << "live")});

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    // The collection holds more documents than the requested sample size, so this samples
    // randomly rather than scanning the whole collection.
    auto sample = manager.load(colls, makeSpec(_kTestNss, 3));
    ASSERT_EQUALS(sample.size(), 3u);
    for (const auto& doc : sample.docs()) {
        ASSERT_EQUALS(doc.getStringField("tag"), "live");
    }
    ASSERT_FALSE(sample.isPersisted());
    ASSERT_FALSE(sample.numPages().has_value());
    ASSERT_TRUE(sample.technique() == SamplingTechniqueEnum::kRandom);
}

TEST_F(SamplingEstimatorTest, SampleManagerGeneratesChunkSample) {
    // TODO SERVER-124372: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss,
                    {BSON("_id" << 1),
                     BSON("_id" << 2),
                     BSON("_id" << 3),
                     BSON("_id" << 4),
                     BSON("_id" << 5),
                     BSON("_id" << 6),
                     BSON("_id" << 7),
                     BSON("_id" << 8)});

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    auto sample = manager.load(
        colls,
        makeSpec(
            _kTestNss, 4, SamplingCEMethodEnum::kChunk, SamplingSourceEnum::kPersistentSample, 2));
    // A chunk starting at the last document of the collection cannot fill its whole chunk, so the
    // sample may be smaller than requested.
    ASSERT_GTE(sample.size(), 2u);
    ASSERT_LTE(sample.size(), 4u);
    ASSERT_TRUE(sample.technique() == SamplingTechniqueEnum::kChunk);
}

TEST_F(SamplingEstimatorTest, SampleManagerLoadsPersistedSample) {
    // TODO SERVER-124372: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss,
                    {BSON("_id" << 1 << "tag" << "live"),
                     BSON("_id" << 2 << "tag" << "live"),
                     BSON("_id" << 3 << "tag" << "live")});
    // Read the source collection UUID in a scope so its IS lock on the db is released before
    // we create the samples collection (which needs MODE_X on the same db).
    const UUID uuid = [&] {
        auto srcColl = acquireCollectionForSampling(operationContext(), _kTestNss);
        return srcColl.getCollectionPtr()->uuid();
    }();
    const std::vector<BSONObj> persistedDocs{BSON("_id" << 10 << "tag" << "persisted"),
                                             BSON("_id" << 11 << "tag" << "persisted")};
    createCollAndInsertDocuments(
        operationContext(),
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName),
        {buildPersistentSampleDoc(
            uuid, SamplingTechniqueEnum::kRandom, persistedDocs.size(), persistedDocs)},
        /*clustered=*/true);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    auto sample = manager.load(colls, makeSpec(_kTestNss, persistedDocs.size()));
    ASSERT_EQUALS(sample.size(), persistedDocs.size());
    for (const auto& doc : sample.docs()) {
        ASSERT_EQUALS(doc.getStringField("tag"), "persisted");
    }
    ASSERT_TRUE(sample.isPersisted());
    ASSERT_EQUALS(sample.numPages().value_or(0), 1u);
    ASSERT_TRUE(sample.technique() == SamplingTechniqueEnum::kRandom);
}

TEST_F(SamplingEstimatorTest, PersistedSampleCoveringCollectionReportsFullCollScan) {
    // TODO SERVER-124372: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    const UUID uuid = [&] {
        auto srcColl = acquireCollectionForSampling(operationContext(), _kTestNss);
        return srcColl.getCollectionPtr()->uuid();
    }();
    const std::vector<BSONObj> persistedDocs{BSON("_id" << 10 << "tag" << "persisted"),
                                             BSON("_id" << 11 << "tag" << "persisted")};
    // The sample is keyed on disk by the requested method, so it must still be looked up by
    // kRandom even though the requested size covers the whole collection.
    createCollAndInsertDocuments(
        operationContext(),
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName),
        {buildPersistentSampleDoc(
            uuid, SamplingTechniqueEnum::kRandom, persistedDocs.size(), persistedDocs)},
        /*clustered=*/true);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    // The source collection is smaller than the requested sample size, which puts this on the
    // full-collection-scan path.
    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    auto sample = manager.load(colls,
                               makeSpec(_kTestNss,
                                        persistedDocs.size(),
                                        SamplingCEMethodEnum::kRandom,
                                        SamplingSourceEnum::kPersistentSample));
    // The persisted sample was found...
    ASSERT_TRUE(sample.isPersisted());
    ASSERT_EQUALS(sample.size(), persistedDocs.size());
    for (const auto& doc : sample.docs()) {
        ASSERT_EQUALS(doc.getStringField("tag"), "persisted");
    }
    // ...but it is reported as a full collection scan, since that is how its contents were built.
    ASSERT_TRUE(sample.technique() == SamplingTechniqueEnum::kFullCollScan);
}

TEST_F(SamplingEstimatorTest, SampleManagerSkipsPersistedSampleForOnTheFlySource) {
    // TODO SERVER-124372: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    const UUID uuid = [&] {
        auto srcColl = acquireCollectionForSampling(operationContext(), _kTestNss);
        return srcColl.getCollectionPtr()->uuid();
    }();
    const std::vector<BSONObj> persistedDocs{BSON("_id" << 10 << "tag" << "persisted")};
    createCollAndInsertDocuments(
        operationContext(),
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName),
        {buildPersistentSampleDoc(
            uuid, SamplingTechniqueEnum::kRandom, persistedDocs.size(), persistedDocs)},
        /*clustered=*/true);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    auto sample = manager.load(colls,
                               makeSpec(_kTestNss,
                                        persistedDocs.size(),
                                        SamplingCEMethodEnum::kRandom,
                                        SamplingSourceEnum::kOnTheFlySample));
    ASSERT_EQUALS(sample.size(), 1u);
    ASSERT_EQUALS(sample.docs()[0].getStringField("tag"), "live");
    ASSERT_FALSE(sample.isPersisted());
}

TEST_F(SamplingEstimatorTest, SampleManagerLoadIsNotMemoized) {
    // TODO SERVER-124372: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss,
                    {BSON("_id" << 1 << "tag" << "live"), BSON("_id" << 2 << "tag" << "live")});
    const UUID uuid = [&] {
        auto srcColl = acquireCollectionForSampling(operationContext(), _kTestNss);
        return srcColl.getCollectionPtr()->uuid();
    }();
    // Create the samples collection up front with a page that does not match the spec; the
    // matching page is inserted only after the first tryLoad(), which must have missed.
    const NamespaceString samplesNss =
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName);
    createCollAndInsertDocuments(
        operationContext(),
        samplesNss,
        {buildPersistentSampleDoc(uuid,
                                  SamplingTechniqueEnum::kRandom,
                                  /*requestedSampleSize=*/7,
                                  {BSON("_id" << 100 << "tag" << "other")})},
        /*clustered=*/true);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    const auto spec = makeSpec(_kTestNss, 2);
    auto first = manager.load(colls, spec);
    ASSERT_FALSE(first.isPersisted());

    // Persist a sample matching the spec, then load again. The second load finds the newly
    // persisted one.
    insertDocuments(samplesNss,
                    {buildPersistentSampleDoc(uuid,
                                              SamplingTechniqueEnum::kRandom,
                                              2,
                                              {BSON("_id" << 10 << "tag" << "persisted"),
                                               BSON("_id" << 11 << "tag" << "persisted")})});
    auto second = manager.load(colls, spec);
    ASSERT_TRUE(second.isPersisted());
    ASSERT_EQUALS(second.size(), 2u);
    for (const auto& doc : second.docs()) {
        ASSERT_EQUALS(doc.getStringField("tag"), "persisted");
    }
}

TEST_F(SamplingEstimatorTest, SampleManagerUsesFullCollScanWhenSampleSizeCoversCollection) {
    // TODO SERVER-124372: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss,
                    {BSON("_id" << 1 << "tag" << "live"), BSON("_id" << 2 << "tag" << "live")});

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    // Requesting more documents than the collection holds makes a full collection scan the
    // cheapest way to produce the sample.
    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    auto sample = manager.load(
        colls,
        makeSpec(
            _kTestNss, 5, SamplingCEMethodEnum::kRandom, SamplingSourceEnum::kPersistentSample));
    ASSERT_EQUALS(sample.size(), 2u);
    ASSERT_TRUE(sample.technique() == SamplingTechniqueEnum::kFullCollScan);
}

TEST_F(SamplingEstimatorTest, SampleManagerRefreshGeneratesAndPersistsSample) {
    // TODO SERVER-124372: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss,
                    {BSON("_id" << 1 << "tag" << "live"),
                     BSON("_id" << 2 << "tag" << "live"),
                     BSON("_id" << 3 << "tag" << "live"),
                     BSON("_id" << 4 << "tag" << "live")});

    // refresh() acquires the source collection itself, so the caller does not need one.
    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    const auto summary = manager.refresh(
        makeSpec(_kTestNss, 2, SamplingCEMethodEnum::kRandom, SamplingSourceEnum::kOnTheFlySample));
    ASSERT_OK(summary);
    ASSERT_EQUALS(summary.getValue().docsPersisted, 2u);
    ASSERT_EQUALS(summary.getValue().pagesPersisted, 1u);
    ASSERT_GREATER_THAN(summary.getValue().bytesPersisted, 0u);
    ASSERT_TRUE(summary.getValue().samplingMethod == SamplingTechniqueEnum::kRandom);

    // The persisted sample can be read back through the persistent loader.
    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    auto loaded = loader.tryLoad(makeSpec(_kTestNss, 2));
    ASSERT_OK(loaded.getStatus());
    ASSERT_EQUALS(loaded.getValue().size(), 2u);
    ASSERT_TRUE(loaded.getValue().isPersisted());
}

TEST_F(SamplingEstimatorTest, SampleManagerRefreshFailsIfCollectionDoesNotExist) {
    const auto noSuchNss = NamespaceString::createNamespaceString_forTest("TestDB", "NoSuchColl");
    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    ASSERT_THROWS_CODE(
        manager.refresh(makeSpec(
            noSuchNss, 10, SamplingCEMethodEnum::kRandom, SamplingSourceEnum::kOnTheFlySample)),
        DBException,
        13097202);
}

TEST_F(SamplingEstimatorTest, SampleManagerRefreshFailsOnTimeseriesCollection) {
    const auto timeseriesNss =
        NamespaceString::createNamespaceString_forTest("TestDB", "TimeseriesColl");
    CollectionOptions options;
    options.timeseries = TimeseriesOptions(/*timeField=*/"time");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), timeseriesNss, options));

    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    ASSERT_THROWS_CODE(
        manager.refresh(makeSpec(
            timeseriesNss, 10, SamplingCEMethodEnum::kRandom, SamplingSourceEnum::kOnTheFlySample)),
        DBException,
        ErrorCodes::IllegalOperation);
}

TEST_F(SamplingEstimatorTest, SampleManagerRefreshReplacesMatchingSample) {
    // TODO SERVER-124372: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss,
                    {BSON("_id" << 1 << "tag" << "first"),
                     BSON("_id" << 2 << "tag" << "first"),
                     BSON("_id" << 3 << "tag" << "first")});
    const NamespaceString samplesNss =
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName);

    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    const auto spec =
        makeSpec(_kTestNss, 2, SamplingCEMethodEnum::kRandom, SamplingSourceEnum::kOnTheFlySample);
    const auto summary = manager.refresh(spec);
    ASSERT_OK(summary);
    ASSERT_EQUALS(summary.getValue().docsPersisted, 2u);
    ASSERT_EQUALS(summary.getValue().pagesPersisted, 1u);
    ASSERT_GREATER_THAN(summary.getValue().bytesPersisted, 0u);
    ASSERT_TRUE(summary.getValue().samplingMethod == SamplingTechniqueEnum::kRandom);
    ASSERT_EQUALS(countSamplesCollDocs(operationContext(), samplesNss), 1u);

    // Collect a different sample with the same identity and persist it: it must replace the
    // existing pages, not add to them (a clustered collection would reject a duplicate _id).
    DBDirectClient client(operationContext());
    client.remove(_kTestNss, BSONObj());
    insertDocuments(_kTestNss,
                    {BSON("_id" << 4 << "tag" << "second"),
                     BSON("_id" << 5 << "tag" << "second"),
                     BSON("_id" << 6 << "tag" << "second")});
    ASSERT_OK(manager.refresh(spec));
    ASSERT_EQUALS(countSamplesCollDocs(operationContext(), samplesNss), 1u);

    // Read the sample back through the persistent loader.
    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    auto loaded = loader.tryLoad(makeSpec(_kTestNss, 2));
    ASSERT_OK(loaded.getStatus());
    ASSERT_EQUALS(loaded.getValue().size(), 2u);
    for (const auto& doc : loaded.getValue().docs()) {
        ASSERT_EQUALS(doc.getStringField("tag"), "second");
    }
}

TEST_F(SamplingEstimatorTest, SampleManagerRefreshUsesRequestedMethodForFullCollScan) {
    // TODO SERVER-124372: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss,
                    {BSON("_id" << 1 << "tag" << "live"), BSON("_id" << 2 << "tag" << "live")});

    // Requesting more documents than the collection holds yields a full collection scan, but the
    // persisted sample must still be keyed by the requested method so a read can find it.
    SampleManager manager(operationContext(),
                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                          nullptr /*customerQueryExpCtx*/);
    const auto summary = manager.refresh(
        makeSpec(_kTestNss, 5, SamplingCEMethodEnum::kRandom, SamplingSourceEnum::kOnTheFlySample));
    ASSERT_OK(summary);
    ASSERT_TRUE(summary.getValue().samplingMethod == SamplingTechniqueEnum::kFullCollScan);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    auto loaded = loader.tryLoad(makeSpec(
        _kTestNss, 5, SamplingCEMethodEnum::kRandom, SamplingSourceEnum::kPersistentSample));
    ASSERT_OK(loaded.getStatus());
    ASSERT_EQUALS(loaded.getValue().size(), 2u);
}

TEST_F(SamplingEstimatorTest, PersistentLoaderRejectsNonClusteredSamplesCollection) {
    // TODO SERVER-124372: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    const UUID uuid = [&] {
        auto srcColl = acquireCollectionForSampling(operationContext(), _kTestNss);
        return srcColl.getCollectionPtr()->uuid();
    }();
    const std::vector<BSONObj> persistedDocs{BSON("_id" << 10 << "tag" << "persisted")};

    createCollAndInsertDocuments(
        operationContext(),
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName),
        {buildPersistentSampleDoc(
            uuid, SamplingTechniqueEnum::kRandom, persistedDocs.size(), persistedDocs)},
        /*clustered=*/false);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    auto loaded = loader.tryLoad(makeSpec(_kTestNss, persistedDocs.size()));
    ASSERT_EQUALS(loaded.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

// ── makePersistentSamplePageDocs ──────────────────────────────────────────────────────────────

namespace {


std::vector<BSONObj> makeRandomSamplePageDocs(const std::vector<BSONObj>& sample,
                                              size_t requestedSampleSize) {
    return makePersistentSamplePageDocs(UUID::gen(),
                                        SamplingTechniqueEnum::kRandom,
                                        requestedSampleSize,
                                        /*numChunks=*/boost::none,
                                        sample,
                                        Date_t::now());
}

// Returns the ids of every doc across all pages of `pages` in order.
std::vector<int> collectPagedDocIds(const std::vector<BSONObj>& pages) {
    std::vector<int> ids;
    for (const auto& page : pages) {
        auto swParsed = parsePersistentSample(page);
        ASSERT_OK(swParsed.getStatus());
        for (const auto& doc : swParsed.getValue().getDocs()) {
            ids.push_back(doc["_id"].numberInt());
        }
    }
    return ids;
}

// Builds a sample of `numDocs` small docs with ids [0, numDocs), replacing the docs at
// `oversizedIdxs` with docs too large to ever fit on a page.
std::vector<BSONObj> makeSampleWithOversizedDocs(int numDocs,
                                                 const std::vector<int>& oversizedIdxs) {
    std::vector<BSONObj> sample;
    sample.reserve(numDocs);
    for (int i = 0; i < numDocs; ++i) {
        const bool oversized =
            std::find(oversizedIdxs.begin(), oversizedIdxs.end(), i) != oversizedIdxs.end();
        sample.push_back(makeSizedDoc(i, oversized ? BSONObjMaxUserSize : 1024));
    }
    return sample;
}

void assertOversizedDocsAbsent(const std::vector<BSONObj>& pages,
                               const std::vector<int>& oversizedIdxs) {
    const auto ids = collectPagedDocIds(pages);
    for (int oversizedIdx : oversizedIdxs) {
        ASSERT_TRUE(std::find(ids.begin(), ids.end(), oversizedIdx) == ids.end())
            << "oversized doc " << oversizedIdx << " should have been discarded";
    }
}

TEST(ReassemblePersistentSample, PagesWithMismatchedDiscardCountRejected) {
    const UUID uuid = UUID::gen();
    // page 0 declares numDocsDiscarded 1, page 1 declares numDocsDiscarded 2.
    auto result = reassemblePersistentSample({
        buildPage(uuid,
                  SamplingTechniqueEnum::kRandom,
                  /*requestedSampleSize=*/4,
                  /*docs=*/{BSON("a" << 0)},
                  /*pageNo=*/0,
                  /*numChunks=*/boost::none,
                  /*numDocsDiscarded=*/1),
        buildPage(uuid,
                  SamplingTechniqueEnum::kRandom,
                  /*requestedSampleSize=*/4,
                  /*docs=*/{BSON("a" << 0)},
                  /*pageNo=*/1,
                  /*numChunks=*/boost::none,
                  /*numDocsDiscarded=*/2),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "persistent sample pages disagree on their identity fields");
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

// ── makePersistentSamplePageDocs
// ───────────────────────────────────────────────────────────────

TEST(MakePersistentSamplePageDocs, ScatteredDiscardsDoNotAddPages) {
    // Discards spread through the sample must not each force a page boundary: what remains is 27
    // tiny docs, which belong on a single page.
    // 3 out of the 30 docs (<= 10%) will be discarded.
    const auto sample = makeSampleWithOversizedDocs(/*numDocs=*/30, /*oversizedIdxs=*/{1, 14, 27});
    const auto pages = makeRandomSamplePageDocs(sample, sample.size());
    ASSERT_EQ(pages.size(), 1u);
    ASSERT_EQ(collectPagedDocIds(pages).size(), 27u);
    assertOversizedDocsAbsent(pages, {1, 14, 27});

    auto swParsed = parsePersistentSample(pages[0]);
    ASSERT_OK(swParsed.getStatus());
    ASSERT_EQ(swParsed.getValue().getNumDocsDiscarded(), 3);
}

TEST(MakePersistentSamplePageDocs, DiscardsAtExactlyThresholdSucceed) {
    // Exactly 10% of the sample is unpersistable, which is still allowed.
    const auto sample = makeSampleWithOversizedDocs(/*numDocs=*/10, /*oversizedIdxs=*/{3});
    const auto pages = makeRandomSamplePageDocs(sample, sample.size());
    ASSERT_EQ(collectPagedDocIds(pages).size(), 9u);
    assertOversizedDocsAbsent(pages, {3});
}

TEST(MakePersistentSamplePageDocs, DiscardsAboveThresholdFail) {
    // 2 of 10 docs (20%) unpersistable exceeds the 10% budget, so the whole sample fails.
    const auto sample = makeSampleWithOversizedDocs(/*numDocs=*/10, /*oversizedIdxs=*/{3, 8});
    ASSERT_THROWS_CODE(makeRandomSamplePageDocs(sample, sample.size()), DBException, 13106000);
}

TEST(MakePersistentSamplePageDocs, SoleOversizedDocFails) {
    // A single-doc sample has no discard budget at all: 1 of 1 is 100%.
    std::vector<BSONObj> sample{makeSizedDoc(0, BSONObjMaxUserSize)};
    ASSERT_THROWS_CODE(makeRandomSamplePageDocs(sample, sample.size()), DBException, 13106000);
}

TEST(MakePersistentSamplePageDocs, DiscardBudgetIsRelativeToTheSampleNotTheRequestedSampleSize) {
    // Only 5 docs were actually sampled even though 100 were requested, so the budget is 0 docs.
    const auto sample = makeSampleWithOversizedDocs(/*numDocs=*/5, /*oversizedIdxs=*/{2});
    ASSERT_THROWS_CODE(
        makeRandomSamplePageDocs(sample, /*requestedSampleSize=*/100), DBException, 13106000);
}

TEST(MakePersistentSamplePageDocs, DiscardFractionIsControlledByServerParameter) {
    // 2 of 10 docs (20%) are unpersistable: rejected at the 10% default, but allowed once the
    // knob is raised to permit it.
    const auto sample = makeSampleWithOversizedDocs(/*numDocs=*/10, /*oversizedIdxs=*/{3, 8});
    ASSERT_THROWS_CODE(makeRandomSamplePageDocs(sample, sample.size()), DBException, 13106000);

    {
        unittest::ServerParameterGuard guard{"internalQueryMaxPersistentSampleDiscardFraction",
                                             0.2};
        const auto pages = makeRandomSamplePageDocs(sample, sample.size());
        ASSERT_EQ(collectPagedDocIds(pages).size(), 8u);
        assertOversizedDocsAbsent(pages, {3, 8});
    }

    {
        // Lowering the knob to 0 forbids discarding anything at all, even a single doc.
        unittest::ServerParameterGuard guard{"internalQueryMaxPersistentSampleDiscardFraction",
                                             0.0};
        const auto oneOversized =
            makeSampleWithOversizedDocs(/*numDocs=*/10, /*oversizedIdxs=*/{3});
        ASSERT_THROWS_CODE(
            makeRandomSamplePageDocs(oneOversized, oneOversized.size()), DBException, 13106000);
    }
}

TEST(MakePersistentSamplePageDocs, DiscardedDocAtEndOfSampleDoesNotProduceEmptyTrailingPage) {
    // The oversized doc is discarded from a fresh page which then has nothing else to hold; that
    // page must not be persisted.
    std::vector<BSONObj> sample;
    for (int i = 0; i < 20; ++i) {
        sample.push_back(makeSizedDoc(i, 1024));
    }
    sample.push_back(makeSizedDoc(20, BSONObjMaxUserSize));

    const auto pages = makeRandomSamplePageDocs(sample, sample.size());
    ASSERT_EQ(pages.size(), 1u);
    ASSERT_EQ(collectPagedDocIds(pages).size(), 20u);
    assertOversizedDocsAbsent(pages, {20});
}

TEST(MakePersistentSamplePageDocs, DiscardsAcrossMultiplePagesRoundTrip) {
    // Large but persistable docs force several pages; the unpersistable ones among them are
    // discarded without disturbing paging.
    constexpr int kNumDocs = 40;
    std::vector<BSONObj> sample;
    sample.reserve(kNumDocs);
    for (int i = 0; i < kNumDocs; ++i) {
        // ids 11 and 29 (2 of 40 == 5%) will be discarded.
        sample.push_back(
            makeSizedDoc(i, (i == 11 || i == 29) ? BSONObjMaxUserSize : 1 * 1024 * 1024));
    }

    const auto pages = makeRandomSamplePageDocs(sample, kNumDocs);
    ASSERT_GTE(pages.size(), 3u);
    for (size_t pageNo = 0; pageNo < pages.size(); ++pageNo) {
        auto swParsed = parsePersistentSample(pages[pageNo]);
        ASSERT_OK(swParsed.getStatus());
        ASSERT_EQ(swParsed.getValue().getPageNo(), static_cast<int>(pageNo));
        ASSERT_LTE(pages[pageNo].objsize(), BSONObjMaxUserSize);
        ASSERT_GTE(swParsed.getValue().getDocs().size(), 1u);
    }

    auto swReassembled = reassemblePersistentSample(pages);
    ASSERT_OK(swReassembled.getStatus());
    ASSERT_EQ(swReassembled.getValue().getDocs().size(), static_cast<size_t>(kNumDocs) - 2);
    ASSERT_EQ(swReassembled.getValue().getNumDocsDiscarded(), 2);

    assertOversizedDocsAbsent(pages, {11, 29});
}

TEST(MakePersistentSamplePageDocs, LargeDocJustUnderMaxFitsOnOwnPage) {
    // A doc just under the BSON size limit should still land on its own page.
    std::vector<BSONObj> sample{makeSizedDoc(0, BSONObjMaxUserSize - 4096)};
    const auto pages = makeRandomSamplePageDocs(sample, sample.size());
    ASSERT_EQ(pages.size(), 1u);
    ASSERT_LT(pages[0].objsize(), BSONObjMaxUserSize);
}

TEST(MakePersistentSamplePageDocs, NumDocsDiscardedFitsOnAPageFilledToTheLastByte) {
    // Begin the sample with one large document, then add some tiny ones until the first page has
    // only a few bytes to spare -- less than numDocsDiscarded needs. Leftovers will spill into a
    // second page.
    std::vector<BSONObj> sample{makeSizedDoc(0, BSONObjMaxUserSize - 2048)};
    for (int i = 1; i <= 200; ++i) {
        sample.push_back(BSON("_id" << i));
    }

    const auto pages = makeRandomSamplePageDocs(sample, sample.size());
    ASSERT_EQ(pages.size(), 2u);

    // We only care about the first page because it's filled as much as possible. We want to be
    // sure that, with numDocsDiscarded appended, the page doesn't exceed the limit but comes close.
    ASSERT_GT(pages[0].objsize(), BSONObjMaxUserSize - 64);
    ASSERT_LTE(pages[0].objsize(), BSONObjMaxUserSize);

    auto swParsed = parsePersistentSample(pages[0]);
    ASSERT_OK(swParsed.getStatus());
    ASSERT_EQ(swParsed.getValue().getNumDocsDiscarded(), 0);
}

TEST(MakePersistentSamplePageDocs, NumDocsDiscardedReservationDefersADocToANewPage) {
    // Begin the sample with one large document and then enough tiny filler documents so that
    // exactly one page is filled as much as possible (including the space reserved for the
    // `numDocsDiscarded` field).
    std::vector<BSONObj> sample{makeSizedDoc(0, BSONObjMaxUserSize - 2048)};
    for (int i = 1; i <= 98; ++i) {
        sample.push_back(BSON("_id" << i));
    }

    // Because space is reserved for the `numDocsDiscarded` field, this document won't fit on the
    // first page.
    const BSONObj deferredDoc = BSON("_id" << 100);
    sample.push_back(deferredDoc);

    const auto pages = makeRandomSamplePageDocs(sample, sample.size());
    ASSERT_EQ(pages.size(), 2u);

    // Make sure the first page is filled as much as possible, so that `deferredDoc` must be on the
    // second page.
    ASSERT_GT(pages[0].objsize(), BSONObjMaxUserSize - 64);
    ASSERT_LTE(pages[0].objsize(), BSONObjMaxUserSize);

    auto swParsed = parsePersistentSample(pages[1]);
    ASSERT_OK(swParsed.getStatus());
    ASSERT_EQ(swParsed.getValue().getDocs().size(), 1u);
    ASSERT_EQ(swParsed.getValue().getDocs()[0]["_id"].numberInt(), 100);
    ASSERT_EQ(swParsed.getValue().getNumDocsDiscarded(), 0);
}

TEST(MakePersistentSamplePageDocs, EmptySampleProducesSingleEmptyPage) {
    const auto pages = makeRandomSamplePageDocs({}, /*requestedSampleSize=*/10);
    ASSERT_EQ(pages.size(), 1u);
    auto swParsed = parsePersistentSample(pages[0]);
    ASSERT_OK(swParsed.getStatus());
    ASSERT_EQ(swParsed.getValue().getDocs().size(), 0u);
    ASSERT_EQ(swParsed.getValue().getNumDocsDiscarded(), 0);
}

TEST(MakePersistentSamplePageDocs, PagesAreNumberedSequentiallyFromZero) {
    constexpr int kNumDocs = 40;
    std::vector<BSONObj> sample;
    for (int i = 0; i < kNumDocs; ++i) {
        sample.push_back(makeSizedDoc(i, 1 * 1024 * 1024));
    }
    const auto pages = makeRandomSamplePageDocs(sample, kNumDocs);
    ASSERT_GTE(pages.size(), 3u);
    std::string previousId;
    for (size_t pageNo = 0; pageNo < pages.size(); ++pageNo) {
        auto swParsed = parsePersistentSample(pages[pageNo]);
        ASSERT_OK(swParsed.getStatus());
        ASSERT_EQ(swParsed.getValue().getPageNo(), static_cast<int>(pageNo));
        // The read path orders pages by `_id` and checks their contiguity with 'pageNo', so
        // ascending 'pageNo' must come with ascending `_id`.
        ASSERT_LT(previousId, swParsed.getValue().get_id());
        previousId = swParsed.getValue().get_id();
        ASSERT_LT(pages[pageNo].objsize(), BSONObjMaxUserSize);
    }
}

TEST(MakePersistentSamplePageDocs, ManySmallDocsStayUnderBsonMax) {
    constexpr int kNumDocs = 30000;
    std::vector<BSONObj> sample;
    sample.reserve(kNumDocs);
    for (int i = 0; i < kNumDocs; ++i) {
        // ~1.1KB each: total ~33MB, forcing multiple pages of ~15k docs each.
        sample.push_back(makeSizedDoc(i, 1152));
    }
    const auto pages = makeRandomSamplePageDocs(sample, kNumDocs);
    ASSERT_GTE(pages.size(), 3u);
    for (const auto& page : pages) {
        ASSERT_LTE(page.objsize(), BSONObjMaxUserSize);
    }
}

TEST(MakePersistentSamplePageDocs, RoundTripsAllDocsAcrossPages) {
    constexpr int kNumDocs = 5000;
    std::vector<BSONObj> sample;
    sample.reserve(kNumDocs);
    for (int i = 0; i < kNumDocs; ++i) {
        sample.push_back(makeSizedDoc(i, 4096));
    }
    const auto pages = makeRandomSamplePageDocs(sample, kNumDocs);
    ASSERT_GTE(pages.size(), 2u);

    auto swReassembled = reassemblePersistentSample(pages);
    ASSERT_OK(swReassembled.getStatus());
    ASSERT_EQ(swReassembled.getValue().getDocs().size(), static_cast<size_t>(kNumDocs));
}

TEST(MakePersistentSamplePageDocs, ProducesParseableRandomSampleDoc) {
    const UUID uuid = UUID::gen();
    const Date_t now = Date_t::now();
    std::vector<BSONObj> sample{BSON("a" << 1), BSON("a" << 2)};

    const auto pages = makePersistentSamplePageDocs(
        uuid, SamplingTechniqueEnum::kRandom, 1000, boost::none, sample, now);
    ASSERT_EQ(pages.size(), 1u);

    auto swParsed = parsePersistentSample(pages[0]);
    ASSERT_OK(swParsed.getStatus());
    const PersistentSampleDoc& parsed = swParsed.getValue();
    ASSERT_EQ(parsed.getRequestedSampleSize(), 1000);
    ASSERT(parsed.getSamplingMethod() == SamplingTechniqueEnum::kRandom);
    ASSERT_EQ(parsed.getPageNo(), 0);
    ASSERT_EQ(parsed.getDocs().size(), 2u);
    ASSERT_EQ(parsed.getDocs()[0]["a"].numberInt(), 1);
}

TEST(MakePersistentSamplePageDocs, ProducesParseableChunkSampleDoc) {
    const UUID uuid = UUID::gen();
    const Date_t now = Date_t::now();
    std::vector<BSONObj> sample{BSON("a" << 1), BSON("a" << 2)};
    const boost::optional<int> numChunks{4};

    const auto pages = makePersistentSamplePageDocs(
        uuid, SamplingTechniqueEnum::kChunk, 1000, numChunks, sample, now);
    ASSERT_EQ(pages.size(), 1u);

    auto swParsed = parsePersistentSample(pages[0]);
    ASSERT_OK(swParsed.getStatus());
    const PersistentSampleDoc& parsed = swParsed.getValue();
    ASSERT(parsed.getSamplingMethod() == SamplingTechniqueEnum::kChunk);
    ASSERT_TRUE(parsed.getNumChunks().has_value());
    ASSERT_EQ(parsed.getNumChunks().value(), numChunks.value());
    ASSERT_EQ(parsed.getPageNo(), 0);
    ASSERT_EQ(parsed.getDocs().size(), 2u);
    ASSERT_EQ(parsed.getDocs()[0]["a"].numberInt(), 1);
    ASSERT_EQ(parsed.getDocs()[1]["a"].numberInt(), 2);
}

TEST(MakePersistentSamplePageDocs, EveryPageReportsSampleWideDiscardCount) {
    constexpr int kNumDocs = 40;
    std::vector<BSONObj> sample;
    sample.reserve(kNumDocs);
    for (int i = 0; i < kNumDocs; ++i) {
        // ids 11 and 29 (2 of 40 == 5%) will be discarded.
        sample.push_back(
            makeSizedDoc(i, (i == 11 || i == 29) ? BSONObjMaxUserSize : 1 * 1024 * 1024));
    }

    const auto pages = makeRandomSamplePageDocs(sample, kNumDocs);
    ASSERT_EQ(pages.size(), 3u);
    for (size_t pageNo = 0; pageNo < pages.size(); ++pageNo) {
        auto swParsed = parsePersistentSample(pages[pageNo]);
        ASSERT_OK(swParsed.getStatus());
        ASSERT_EQ(swParsed.getValue().getNumDocsDiscarded(), 2);
    }
}

}  // namespace

}  // namespace mongo::ce
