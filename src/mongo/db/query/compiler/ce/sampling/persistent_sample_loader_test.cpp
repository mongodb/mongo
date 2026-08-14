// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <string>
#include <vector>

namespace mongo::ce {
namespace {

const BSONObj kStubSampleDoc = BSON("_id" << 1);
const std::vector<BSONObj> kStubSampleDocs{kStubSampleDoc};

// ── makePersistentSampleId ────────────────────────────────────────────────────────────────────

// A fixed UUID so the tests below can spell out the expected `_id` strings in full.
const UUID kUuid = uassertStatusOK(UUID::parse("01234567-89ab-cdef-0123-456789abcdef"));

// The layout the tests pin down. schemaVersion is part of the identity so a schema bump can never
// match a stale document, and it follows the UUID so that every sample of one collection is
// contiguous in the clustered collection.
std::string expectedId(std::string_view method, std::string_view tail) {
    return str::stream() << kUuid.toString() << "_" << kPersistentSampleSchemaVersion << "_"
                         << method << "_" << tail;
}

TEST(MakePersistentSampleId, LayoutForEachSamplingTechnique) {
    ASSERT_EQ(makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none),
              expectedId("random", "384_000"));
    ASSERT_EQ(makePersistentSampleId(kUuid, SamplingTechniqueEnum::kSeqScan, 384, boost::none),
              expectedId("seqScan", "384_000"));
    ASSERT_EQ(makePersistentSampleId(kUuid, SamplingTechniqueEnum::kStrides, 384, boost::none),
              expectedId("strides", "384_000"));
    // numChunks is only part of the key for the chunk technique, and sits between the sample size
    // and the page number.
    ASSERT_EQ(makePersistentSampleId(kUuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/10),
              expectedId("chunk", "384_10_000"));
}

// We may use full coll scan to generate a sample but it is never persisted under that technique.
// We'd use the requested techinque instead.
DEATH_TEST_REGEX(MakePersistentSampleIdDeathTest, FullCollScanIsRejected, "12832700") {
    makePersistentSampleId(kUuid, SamplingTechniqueEnum::kFullCollScan, 384, boost::none);
}

DEATH_TEST_REGEX(MakePersistentSampleIdDeathTest, PageNoWiderThanSampleSizeIsRejected, "13321000") {
    makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 10, boost::none, /*pageNo=*/100);
}

TEST(MakePersistentSampleId, IsDeterministic) {
    ASSERT_EQ(makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 1000, boost::none, 7),
              makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 1000, boost::none, 7));
}

TEST(MakePersistentSampleId, PageNoDefaultsToZero) {
    ASSERT_EQ(makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none),
              makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none, 0));
}

TEST(MakePersistentSampleId, PageNoIsZeroPaddedToTheWidthOfSampleSize) {
    // A sample can hold at most one page per sampled document, so padding the page number to the
    // width of the sample size is always enough to keep every page the same length.
    ASSERT_EQ(makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 5, boost::none, 4),
              expectedId("random", "5_4"));
    ASSERT_EQ(makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none, 7),
              expectedId("random", "384_007"));
    ASSERT_EQ(makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none, 383),
              expectedId("random", "384_383"));
    ASSERT_EQ(makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 10000, boost::none, 42),
              expectedId("random", "10000_00042"));
}

TEST(MakePersistentSampleId, EveryIdOfASampleHasTheSameLength) {
    // Equal length is what makes lexicographic order coincide with numeric page order.
    const size_t length =
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 1000, boost::none).size();
    for (int pageNo : {0, 1, 9, 10, 99, 100, 999}) {
        ASSERT_EQ(
            makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 1000, boost::none, pageNo)
                .size(),
            length);
    }
}

TEST(MakePersistentSampleId, PagesOfSameSampleSortInPageNoOrder) {
    std::string previous;
    for (int pageNo = 0; pageNo < 384; ++pageNo) {
        const auto id =
            makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none, pageNo);
        ASSERT_LT(previous, id);
        previous = id;
    }
}

TEST(MakePersistentSampleId, EveryIdentityFieldChangesTheId) {
    const auto baseline =
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/10, 1);
    ASSERT_NE(baseline,
              makePersistentSampleId(
                  UUID::gen(), SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/10, 1));
    ASSERT_NE(baseline,
              makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none, 1));
    ASSERT_NE(
        baseline,
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kChunk, 385, /*numChunks=*/10, 1));
    ASSERT_NE(
        baseline,
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/20, 1));
    ASSERT_NE(
        baseline,
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/10, 2));
}

// ── makePersistentSampleIdRange ───────────────────────────────────────────────────────────────

TEST(MakePersistentSampleIdRange, BoundsAreThePageNumberExtremes) {
    const auto [minId, maxId] =
        makePersistentSampleIdRange(kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none);
    ASSERT_EQ(minId, expectedId("random", "384_000"));
    ASSERT_EQ(maxId, expectedId("random", "384_999"));

    const auto [chunkMin, chunkMax] =
        makePersistentSampleIdRange(kUuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/10);
    ASSERT_EQ(chunkMin, expectedId("chunk", "384_10_000"));
    ASSERT_EQ(chunkMax, expectedId("chunk", "384_10_999"));
}

TEST(MakePersistentSampleIdRange, BracketsEveryPageOfTheSample) {
    const auto [minId, maxId] =
        makePersistentSampleIdRange(kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none);
    ASSERT_LTE(minId, maxId);
    // 383 is the highest page a 384-document sample can have.
    for (int pageNo : {0, 1, 42, 383}) {
        const auto id =
            makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none, pageNo);
        ASSERT_LTE(minId, id);
        ASSERT_LTE(id, maxId);
    }
}

TEST(MakePersistentSampleIdRange, ExcludesOtherSamples) {
    const auto [minId, maxId] =
        makePersistentSampleIdRange(kUuid, SamplingTechniqueEnum::kRandom, 1, boost::none);

    // A sample whose size is a numeric prefix of another's must not fall inside its range: this is
    // the footgun that motivated moving off object-valued _ids.
    const std::vector<std::string> outsiders{
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 10, boost::none, 9),
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 100, boost::none, 0),
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kChunk, 1, /*numChunks=*/1),
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kSeqScan, 1, boost::none),
        makePersistentSampleId(UUID::gen(), SamplingTechniqueEnum::kRandom, 1, boost::none),
    };
    for (const auto& id : outsiders) {
        ASSERT_TRUE(id < minId || id > maxId) << "id unexpectedly inside range: " << id;
    }
}

// ── makePersistentSampleAllPagesLookupFilter ─────────────────────────────────────────────────────

TEST(makePersistentSampleAllPagesLookupFilter, IsAnInclusiveIdRange) {
    const UUID uuid = UUID::gen();
    const auto [minId, maxId] =
        makePersistentSampleIdRange(uuid, SamplingTechniqueEnum::kRandom, 384, boost::none);
    ASSERT_BSONOBJ_EQ(makePersistentSampleAllPagesLookupFilter(
                          uuid, SamplingTechniqueEnum::kRandom, 384, boost::none),
                      BSON("_id" << BSON("$gte" << minId << "$lte" << maxId)));
}

TEST(makePersistentSampleAllPagesLookupFilter, IncludesNumChunksForChunkTechnique) {
    const UUID uuid = UUID::gen();
    const auto [minId, maxId] =
        makePersistentSampleIdRange(uuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/10);
    ASSERT_BSONOBJ_EQ(makePersistentSampleAllPagesLookupFilter(
                          uuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/10),
                      BSON("_id" << BSON("$gte" << minId << "$lte" << maxId)));
    // The chunk count is part of the shared prefix, so a different one yields a disjoint range.
    const auto [otherMin, otherMax] =
        makePersistentSampleIdRange(uuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/20);
    ASSERT_TRUE(otherMin > maxId || otherMax < minId);
}

// ── parsePersistentSample ─────────────────────────────────────────────────────────────────────

TEST(ParsePersistentSample, EmptyDocReturnsNoSuchKey) {
    auto result = parsePersistentSample(BSONObj{});
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::NoSuchKey);
}

TEST(ParsePersistentSample, ValidRandomSamplePopulatesAllFields) {
    const UUID uuid = UUID::gen();
    const std::vector<BSONObj> docs{BSON("_id" << 1 << "a" << 10), BSON("_id" << 2 << "a" << 20)};
    const auto sampleDoc =
        buildPersistentSampleDoc(uuid, SamplingTechniqueEnum::kRandom, docs.size(), docs);
    auto result = parsePersistentSample(sampleDoc);
    ASSERT_OK(result.getStatus());
    const auto& sample = result.getValue();

    ASSERT_EQUALS(sample.getCollectionUuid(), uuid.toString());
    ASSERT_EQUALS(sample.getSamplingMethod(), SamplingTechniqueEnum::kRandom);
    ASSERT_EQUALS(static_cast<size_t>(sample.getSampleSize()), docs.size());
    ASSERT_FALSE(sample.getNumChunks().has_value());
    ASSERT_EQUALS(sample.getPageNo(), 0);

    ASSERT_EQUALS(sample.getCreatedAt(),
                  sampleDoc[PersistentSampleDoc::kCreatedAtFieldName].date());
    ASSERT_EQUALS(sample.getDocs().size(), docs.size());
    ASSERT_BSONOBJ_EQ(sample.getDocs()[0], docs[0]);
    ASSERT_BSONOBJ_EQ(sample.getDocs()[1], docs[1]);
}

TEST(ParsePersistentSample, ChunkSamplePopulatesNumChunks) {
    const std::vector<BSONObj> docs{BSON("x" << 1), BSON("x" << 2)};
    auto result = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(), SamplingTechniqueEnum::kChunk, docs.size(), docs, /*numChunks=*/1));
    ASSERT_OK(result.getStatus());
    const auto& sample = result.getValue();
    ASSERT_EQUALS(sample.getSamplingMethod(), SamplingTechniqueEnum::kChunk);
    ASSERT_TRUE(sample.getNumChunks().has_value());
    ASSERT_EQUALS(sample.getNumChunks().value(), 1);
}

TEST(ParsePersistentSample, RejectsDocsArrayLargerThanSampleSize) {
    // 'docs' must not contain more entries than 'sampleSize'. Fewer is allowed: chunk sampling
    // may collect less than the requested size when a chunk starts near end-of-collection.
    const std::vector<BSONObj> docs{BSON("a" << 1), BSON("a" << 2)};

    // docs.size() > sampleSize — rejected.
    auto docsExceedDeclared = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(), SamplingTechniqueEnum::kRandom, /*sampleSize=*/docs.size() - 1, docs));
    ASSERT_NOT_OK(docsExceedDeclared.getStatus());
    ASSERT_EQUALS(docsExceedDeclared.getStatus().code(), ErrorCodes::UnsupportedFormat);

    // docs.size() < sampleSize — allowed (underfull chunk sample).
    auto underFull = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(), SamplingTechniqueEnum::kRandom, /*sampleSize=*/docs.size() + 1, docs));
    ASSERT_OK(underFull.getStatus());
}

TEST(ParsePersistentSample, RejectsEmptySample) {
    auto result = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(), SamplingTechniqueEnum::kRandom, /*sampleSize=*/0, /*docs=*/{}));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsMissingSchemaVersion) {
    BSONObjBuilder b;
    b.append(PersistentSampleDoc::kCollectionUuidFieldName, UUID::gen().toString());
    b.append(PersistentSampleDoc::kSampleSizeFieldName,
             static_cast<long long>(kStubSampleDocs.size()));
    b.append(PersistentSampleDoc::kSamplingMethodFieldName,
             idlSerialize(SamplingTechniqueEnum::kRandom));
    b.appendDate(PersistentSampleDoc::kCreatedAtFieldName, Date_t::now());
    BSONArrayBuilder arr(b.subarrayStart(PersistentSampleDoc::kDocsFieldName));
    for (const auto& d : kStubSampleDocs) {
        arr.append(d);
    }
    arr.done();
    auto result = parsePersistentSample(b.obj());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsBelowMinSchemaVersion) {
    auto result =
        parsePersistentSample(buildPersistentSampleDoc(UUID::gen(),
                                                       SamplingTechniqueEnum::kRandom,
                                                       /*sampleSize=*/kStubSampleDocs.size(),
                                                       /*docs=*/kStubSampleDocs,
                                                       /*numChunks=*/boost::none,
                                                       /*schemaVersion=*/0));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNonNumericSchemaVersion) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*sampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kSchemaVersionFieldName << "one")));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsMissingCollectionUuid) {
    auto result = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(),
        SamplingTechniqueEnum::kRandom,
        /*sampleSize=*/kStubSampleDocs.size(),
        /*docs=*/kStubSampleDocs,
        /*numChunks=*/boost::none,
        /*schemaVersion=*/kPersistentSampleSchemaVersion,
        // Replace the built string UUID with an Undefined value.
        BSON(PersistentSampleDoc::kCollectionUuidFieldName << BSONUndefined)));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsUnknownSamplingMethod) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*sampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kSamplingMethodFieldName << "bogus")));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNonNumericSize) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*sampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kSampleSizeFieldName << "huge")));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNegativeSize) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*sampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kSampleSizeFieldName << -1LL)));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsChunkSampleWithoutNumChunks) {
    // Chunk technique without chunk_size — malformed.
    auto result =
        parsePersistentSample(buildPersistentSampleDoc(UUID::gen(),
                                                       SamplingTechniqueEnum::kChunk,
                                                       /*sampleSize=*/kStubSampleDocs.size(),
                                                       /*docs=*/kStubSampleDocs,
                                                       /*numChunks=*/boost::none));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ParsePersistentSample, RejectsNonPositiveNumChunks) {
    // chunk_size of zero is invalid — chunks must contain at least one doc.
    auto zeroResult =
        parsePersistentSample(buildPersistentSampleDoc(UUID::gen(),
                                                       SamplingTechniqueEnum::kChunk,
                                                       /*sampleSize=*/kStubSampleDocs.size(),
                                                       /*docs=*/kStubSampleDocs,
                                                       /*numChunks=*/0));
    ASSERT_NOT_OK(zeroResult.getStatus());

    auto negativeResult =
        parsePersistentSample(buildPersistentSampleDoc(UUID::gen(),
                                                       SamplingTechniqueEnum::kChunk,
                                                       /*sampleSize=*/kStubSampleDocs.size(),
                                                       /*docs=*/kStubSampleDocs,
                                                       /*numChunks=*/-1));
    ASSERT_NOT_OK(negativeResult.getStatus());
}

TEST(ParsePersistentSample, RejectsNegativePageNo) {
    // pageNo is defined in the idl to be >= 0
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*sampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/
                                 kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kPageNoFieldName << -1)));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNonDateCreatedAt) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*sampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kCreatedAtFieldName << "yesterday")));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsMissingDocsField) {
    const UUID uuid = UUID::gen();
    BSONObjBuilder b;
    b.append("_id",
             makePersistentSampleId(
                 uuid, SamplingTechniqueEnum::kRandom, kStubSampleDocs.size(), boost::none));
    b.append(PersistentSampleDoc::kPageNoFieldName, 0);
    b.append(PersistentSampleDoc::kCollectionUuidFieldName, uuid.toString());
    b.append(PersistentSampleDoc::kSchemaVersionFieldName, kPersistentSampleSchemaVersion);
    b.appendDate(PersistentSampleDoc::kCreatedAtFieldName, Date_t::now());
    b.append(PersistentSampleDoc::kSampleSizeFieldName,
             static_cast<long long>(kStubSampleDocs.size()));
    b.append(PersistentSampleDoc::kSamplingMethodFieldName,
             idlSerialize(SamplingTechniqueEnum::kRandom));
    auto result = parsePersistentSample(b.obj());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNonArrayDocsField) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*sampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kDocsFieldName << "not-an-array")));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNonObjectEntryInDocsArray) {
    BSONArrayBuilder arr;
    arr.append(BSON("_id" << 1));
    arr.append(42);  // scalar — must cause a miss.
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*sampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kDocsFieldName << arr.arr())));
    ASSERT_NOT_OK(result.getStatus());
}

// ── reassemblePersistentSample ────────────────────────────────────────────────────────────────

// Builds a single page document for `pageNo` of a logical sample. All pages of a sample share the
// same identity fields and differ only in `_id.pageNo` and their slice of `docs`.
BSONObj buildPage(const UUID& uuid,
                  SamplingTechniqueEnum method,
                  size_t sampleSize,
                  const std::vector<BSONObj>& docs,
                  int pageNo,
                  boost::optional<int> numChunks = boost::none) {
    return buildPersistentSampleDoc(uuid,
                                    method,
                                    sampleSize,
                                    docs,
                                    numChunks,
                                    /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                    /*overrides=*/BSONObj{},
                                    pageNo);
}

TEST(ReassemblePersistentSample, EmptyPagesReturnsNoSuchKey) {
    auto result = reassemblePersistentSample({});
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::NoSuchKey);
}

TEST(ReassemblePersistentSample, SinglePageRoundTrips) {
    const UUID uuid = UUID::gen();
    const std::vector<BSONObj> docs{BSON("_id" << 1), BSON("_id" << 2)};
    auto result = reassemblePersistentSample(
        {buildPage(uuid, SamplingTechniqueEnum::kRandom, docs.size(), docs, /*pageNo=*/0)});
    ASSERT_OK(result.getStatus());
    const auto& sample = result.getValue();
    ASSERT_EQUALS(sample.getDocs().size(), docs.size());
    ASSERT_BSONOBJ_EQ(sample.getDocs()[0], docs[0]);
    ASSERT_BSONOBJ_EQ(sample.getDocs()[1], docs[1]);
}

TEST(ReassemblePersistentSample, MultiplePagesConcatenatedInPageNoOrder) {
    const UUID uuid = UUID::gen();
    const BSONObj d0 = BSON("_id" << 0);
    const BSONObj d1 = BSON("_id" << 1);
    const BSONObj d2 = BSON("_id" << 2);
    const BSONObj d3 = BSON("_id" << 3);
    const size_t sampleSize = 4;

    // The clustered forward scan delivers pages already ordered by pageNo, so reassembly relies on
    // that ordering rather than sorting.
    auto result = reassemblePersistentSample({
        buildPage(uuid, SamplingTechniqueEnum::kRandom, sampleSize, {d0, d1}, /*pageNo=*/0),
        buildPage(uuid, SamplingTechniqueEnum::kRandom, sampleSize, {d2, d3}, /*pageNo=*/1),
    });
    ASSERT_OK(result.getStatus());
    const auto& sample = result.getValue();
    ASSERT_EQUALS(sample.getDocs().size(), 4u);
    ASSERT_BSONOBJ_EQ(sample.getDocs()[0], d0);
    ASSERT_BSONOBJ_EQ(sample.getDocs()[1], d1);
    ASSERT_BSONOBJ_EQ(sample.getDocs()[2], d2);
    ASSERT_BSONOBJ_EQ(sample.getDocs()[3], d3);
}

TEST(ReassemblePersistentSample, OutOfOrderPagesRejected) {
    const UUID uuid = UUID::gen();
    // reassembly requires pages in pageNo order (guaranteed by the clustered forward scan); an
    // out-of-order sequence is treated as an incorrectly written sample.
    auto result = reassemblePersistentSample({
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 2, {BSON("a" << 1)}, /*pageNo=*/1),
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 2, {BSON("a" << 0)}, /*pageNo=*/0),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, MissingMiddlePageRejected) {
    const UUID uuid = UUID::gen();
    // pages 0 and 2 present, 1 missing
    auto result = reassemblePersistentSample({
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 3, {BSON("a" << 0)}, /*pageNo=*/0),
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 3, {BSON("a" << 2)}, /*pageNo=*/2),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, FirstPageNotZeroRejected) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample({
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 2, {BSON("a" << 1)}, /*pageNo=*/1),
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 2, {BSON("a" << 2)}, /*pageNo=*/2),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, DuplicatePageNoRejected) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample({
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 2, {BSON("a" << 0)}, /*pageNo=*/0),
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 2, {BSON("a" << 1)}, /*pageNo=*/0),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, IdentityMismatchAcrossPagesRejected) {
    const UUID uuid = UUID::gen();
    // page 0 declares sampleSize 4, page 1 declares sampleSize 5 — inconsistent identity.
    auto result = reassemblePersistentSample({
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 4, {BSON("a" << 0)}, /*pageNo=*/0),
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 5, {BSON("a" << 1)}, /*pageNo=*/1),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, ReassembledDocsExceedingSampleSizeRejected) {
    const UUID uuid = UUID::gen();
    // Each page is individually within sampleSize=2, but combined they hold 3 docs.
    auto result = reassemblePersistentSample({
        buildPage(uuid,
                  SamplingTechniqueEnum::kRandom,
                  2,
                  {BSON("a" << 0), BSON("a" << 1)},
                  /*pageNo=*/0),
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 2, {BSON("a" << 2)}, /*pageNo=*/1),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, MalformedPagePropagatesError) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample({
        buildPage(uuid, SamplingTechniqueEnum::kRandom, 2, {BSON("a" << 0)}, /*pageNo=*/0),
        BSONObj{},  // empty/malformed page.
    });
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ReassemblePersistentSample, PreservesChunkNumChunks) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample({
        buildPage(uuid,
                  SamplingTechniqueEnum::kChunk,
                  2,
                  {BSON("a" << 0)},
                  /*pageNo=*/0,
                  /*numChunks=*/1),
        buildPage(uuid,
                  SamplingTechniqueEnum::kChunk,
                  2,
                  {BSON("a" << 1)},
                  /*pageNo=*/1,
                  /*numChunks=*/1),
    });
    ASSERT_OK(result.getStatus());
    const auto& sample = result.getValue();
    ASSERT_EQUALS(sample.getSamplingMethod(), SamplingTechniqueEnum::kChunk);
    ASSERT_TRUE(sample.getNumChunks().has_value());
    ASSERT_EQUALS(sample.getNumChunks().value(), 1);
    ASSERT_EQUALS(sample.getDocs().size(), 2u);
}

// ── makePersistentSamplePageDocs
// ───────────────────────────────────────────────────────────────

std::vector<BSONObj> makeRandomSamplePageDocs(const std::vector<BSONObj>& sample,
                                              size_t sampleSize) {
    return makePersistentSamplePageDocs(UUID::gen(),
                                        SamplingTechniqueEnum::kRandom,
                                        sampleSize,
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

TEST(MakePersistentSamplePageDocs, ScatteredDiscardsDoNotAddPages) {
    // Discards spread through the sample must not each force a page boundary: what remains is 27
    // tiny docs, which belong on a single page.
    // 3 out of the 30 docs (<= 10%) will be discarded.
    const auto sample = makeSampleWithOversizedDocs(/*numDocs=*/30, /*oversizedIdxs=*/{1, 14, 27});
    const auto pages = makeRandomSamplePageDocs(sample, sample.size());
    ASSERT_EQ(pages.size(), 1u);
    ASSERT_EQ(collectPagedDocIds(pages).size(), 27u);
    assertOversizedDocsAbsent(pages, {1, 14, 27});
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
    ASSERT_THROWS_CODE(makeRandomSamplePageDocs(sample, /*sampleSize=*/100), DBException, 13106000);
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

    assertOversizedDocsAbsent(pages, {11, 29});
}

TEST(MakePersistentSamplePageDocs, LargeDocJustUnderMaxFitsOnOwnPage) {
    // A doc just under the BSON size limit should still land on its own page.
    std::vector<BSONObj> sample{makeSizedDoc(0, BSONObjMaxUserSize - 4096)};
    const auto pages = makeRandomSamplePageDocs(sample, sample.size());
    ASSERT_EQ(pages.size(), 1u);
    ASSERT_LT(pages[0].objsize(), BSONObjMaxUserSize);
}

TEST(MakePersistentSamplePageDocs, EmptySampleProducesSingleEmptyPage) {
    const auto pages = makeRandomSamplePageDocs({}, /*sampleSize=*/10);
    ASSERT_EQ(pages.size(), 1u);
    auto swParsed = parsePersistentSample(pages[0]);
    ASSERT_OK(swParsed.getStatus());
    ASSERT_EQ(swParsed.getValue().getDocs().size(), 0u);
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
    ASSERT_EQ(parsed.getSampleSize(), 1000);
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

}  // namespace
}  // namespace mongo::ce
