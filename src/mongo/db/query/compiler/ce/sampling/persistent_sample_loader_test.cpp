// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <string>
#include <vector>

namespace mongo::ce {
namespace {

const BSONObj kStubSampleDoc = BSON("_id" << 1);
const std::vector<BSONObj> kStubSampleDocs{kStubSampleDoc};

// ── makePersistentSampleIdObj ─────────────────────────────────────────────────────────────────

TEST(MakePersistentSampleIdObj, EqualIdentitiesProduceEqualIds) {
    const UUID uuid = UUID::gen();
    const auto a =
        makePersistentSampleIdObj(uuid, SamplingTechniqueEnum::kRandom, 1000, boost::none);
    const auto b =
        makePersistentSampleIdObj(uuid, SamplingTechniqueEnum::kRandom, 1000, boost::none);
    ASSERT_BSONOBJ_EQ(a, b);
}

TEST(MakePersistentSampleIdObj, PopulatesFieldsInPrefixOrder) {
    const UUID uuid = UUID::gen();
    const auto randomId =
        makePersistentSampleIdObj(uuid, SamplingTechniqueEnum::kRandom, 384, boost::none);
    // schemaVersion is part of the identity so a schema bump can never match a stale document.
    ASSERT_EQ(randomId[PersistentSampleId::kSchemaVersionFieldName].numberInt(),
              kPersistentSampleSchemaVersion);
    ASSERT_EQ(UUID::parse(randomId[PersistentSampleId::kCollectionUuidFieldName]), uuid);
    ASSERT_EQ(randomId[PersistentSampleId::kSamplingMethodFieldName].str(), "random");
    ASSERT_EQ(randomId[PersistentSampleId::kSampleSizeFieldName].numberLong(), 384);
    ASSERT_TRUE(randomId[PersistentSampleId::kNumChunksFieldName].eoo());
    ASSERT_EQ(randomId[PersistentSampleId::kPageNoFieldName].numberInt(), 0);

    // schemaVersion first, pageNo last, so the clustered key orders pages by pageNo.
    std::vector<std::string> fieldNames;
    for (auto&& e : randomId) {
        fieldNames.push_back(std::string{e.fieldNameStringData()});
    }
    ASSERT_EQ(fieldNames.front(), PersistentSampleId::kSchemaVersionFieldName);
    ASSERT_EQ(fieldNames.back(), PersistentSampleId::kPageNoFieldName);

    const auto chunkId =
        makePersistentSampleIdObj(uuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/10);
    ASSERT_EQ(chunkId[PersistentSampleId::kNumChunksFieldName].numberInt(), 10);
    ASSERT_EQ(chunkId[PersistentSampleId::kPageNoFieldName].numberInt(), 0);
}

TEST(MakePersistentSampleIdObj, PageNoDefaultsToZeroAndIsSettable) {
    const UUID uuid = UUID::gen();
    // Omitting the argument yields page 0.
    const auto defaulted =
        makePersistentSampleIdObj(uuid, SamplingTechniqueEnum::kRandom, 384, boost::none);
    ASSERT_EQ(defaulted[PersistentSampleId::kPageNoFieldName].numberInt(), 0);

    // An explicit page number is threaded through to the _id.
    const auto page7 =
        makePersistentSampleIdObj(uuid, SamplingTechniqueEnum::kRandom, 384, boost::none, 7);
    ASSERT_EQ(page7[PersistentSampleId::kPageNoFieldName].numberInt(), 7);
}

TEST(MakePersistentSampleIdObj, DifferentConfigurationsProduceDifferentIds) {
    const UUID uuid = UUID::gen();
    const auto randomId =
        makePersistentSampleIdObj(uuid, SamplingTechniqueEnum::kRandom, 384, boost::none);
    const auto otherUuidId =
        makePersistentSampleIdObj(UUID::gen(), SamplingTechniqueEnum::kRandom, 384, boost::none);
    const auto chunkId =
        makePersistentSampleIdObj(uuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/10);
    const auto differentSizeId =
        makePersistentSampleIdObj(uuid, SamplingTechniqueEnum::kRandom, 1000, boost::none);
    const auto differentChunksId =
        makePersistentSampleIdObj(uuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/20);
    const auto differentPageId = makePersistentSampleIdObj(
        uuid, SamplingTechniqueEnum::kRandom, 384, boost::none, /*pageNo=*/1);

    ASSERT_BSONOBJ_NE(randomId, otherUuidId);
    ASSERT_BSONOBJ_NE(randomId, chunkId);
    ASSERT_BSONOBJ_NE(randomId, differentSizeId);
    ASSERT_BSONOBJ_NE(chunkId, differentChunksId);
    ASSERT_BSONOBJ_NE(randomId, differentPageId);
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
    ASSERT_EQUALS(sample.get_id().getPageNo(), 0);

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
    const UUID uuid = UUID::gen();
    const BSONObj badId = BSON(
        PersistentSampleId::kSchemaVersionFieldName
        << kPersistentSampleSchemaVersion << PersistentSampleId::kCollectionUuidFieldName << uuid
        << PersistentSampleId::kSamplingMethodFieldName
        << idlSerialize(SamplingTechniqueEnum::kRandom) << PersistentSampleId::kSampleSizeFieldName
        << static_cast<long long>(kStubSampleDocs.size()) << PersistentSampleId::kPageNoFieldName
        << -1);
    auto result =
        parsePersistentSample(buildPersistentSampleDoc(uuid,
                                                       SamplingTechniqueEnum::kRandom,
                                                       /*sampleSize=*/kStubSampleDocs.size(),
                                                       /*docs=*/kStubSampleDocs,
                                                       /*numChunks=*/boost::none,
                                                       /*schemaVersion=*/
                                                       kPersistentSampleSchemaVersion,
                                                       BSON("_id" << badId)));
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
             makePersistentSampleIdObj(
                 uuid, SamplingTechniqueEnum::kRandom, kStubSampleDocs.size(), boost::none));
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
    return buildPersistentSampleDoc(
        uuid,
        method,
        sampleSize,
        docs,
        numChunks,
        /*schemaVersion=*/kPersistentSampleSchemaVersion,
        /*overrides=*/
        BSON("_id" << makePersistentSampleIdObj(uuid, method, sampleSize, numChunks, pageNo)));
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

}  // namespace
}  // namespace mongo::ce
