// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo::ce {
namespace {

const BSONObj kStubSampleDoc = BSON("_id" << 1);
const std::vector<BSONObj> kStubSampleDocs{kStubSampleDoc};

// ── makePersistentSampleId ────────────────────────────────────────────────────────────────────

// A fixed UUID so the tests below can spell out the expected `_id` strings in full.
const UUID kUuid = uassertStatusOK(UUID::parse("01234567-89ab-cdef-0123-456789abcdef"));

// The layout the tests pin down. schemaVersion is part of the identity so a schema bump can never
// match a stale document, and it follows the UUID so that every page of one collection's sample is
// contiguous in the clustered collection.
std::string expectedIdPrefix() {
    return str::stream() << kUuid.toString() << "_" << kPersistentSampleSchemaVersion << "_";
}

std::string expectedId(uint64_t pageNo, boost::optional<uint64_t> genId = boost::none) {
    std::string id = expectedIdPrefix();
    if (genId) {
        id += fmt::format("{}_", *genId);
    }
    return id + fmt::format("{:010}", pageNo);
}

TEST(MakePersistentSampleId, Layout) {
    ASSERT_EQ(makePersistentSampleId(kUuid), expectedId(0));
    ASSERT_EQ(makePersistentSampleId(kUuid, /*pageNo=*/7), expectedId(7));
}

TEST(MakePersistentSampleId, GenIdPrecedesThePageNoUnpadded) {
    ASSERT_EQ(makePersistentSampleId(kUuid, /*pageNo=*/7, /*genId=*/0), expectedId(7, 0));
    ASSERT_EQ(makePersistentSampleId(kUuid, /*pageNo=*/7, /*genId=*/145), expectedId(7, 145));
    // 18446744073709551615 is 2^64 - 1, the max uint64_t.
    ASSERT_EQ(
        makePersistentSampleId(kUuid, /*pageNo=*/0, /*genId=*/std::numeric_limits<uint64_t>::max()),
        expectedId(0, std::numeric_limits<uint64_t>::max()));
}

TEST(MakePersistentSampleId, IsDeterministic) {
    ASSERT_EQ(makePersistentSampleId(kUuid, 7), makePersistentSampleId(kUuid, 7));
}

TEST(MakePersistentSampleId, PageNoDefaultsToZero) {
    ASSERT_EQ(makePersistentSampleId(kUuid), makePersistentSampleId(kUuid, 0));
}


TEST(MakePersistentSampleId, EveryIdOfASampleHasTheSameLength) {
    // Equal length is what makes lexicographic order coincide with numeric page order.
    const size_t length = makePersistentSampleId(kUuid).size();
    for (int pageNo : {0, 1, 9, 10, 99, 100, 999, std::numeric_limits<int>::max()}) {
        ASSERT_EQ(makePersistentSampleId(kUuid, pageNo).size(), length);
    }
}

TEST(MakePersistentSampleId, PagesOfSameSampleSortInPageNoOrder) {
    std::string previous;
    for (int pageNo = 0; pageNo < 1024; ++pageNo) {
        const auto id = makePersistentSampleId(kUuid, pageNo);
        ASSERT_LT(previous, id);
        previous = id;
    }
}

TEST(MakePersistentSampleId, EveryIdentityFieldChangesTheId) {
    const auto baseline = makePersistentSampleId(kUuid, 1);
    ASSERT_NE(baseline, makePersistentSampleId(UUID::gen(), 1));
    ASSERT_NE(baseline, makePersistentSampleId(kUuid, 2));
    ASSERT_NE(baseline, makePersistentSampleId(kUuid, 1, /*genId=*/0));
}

// ── makePersistentSampleIdRange ───────────────────────────────────────────────────────────────

TEST(MakePersistentSampleIdRange, BoundsAreThePageNumberExtremes) {
    const auto [minId, maxId] = makePersistentSampleIdRange(kUuid);
    ASSERT_EQ(minId, expectedId(0));
    ASSERT_EQ(maxId, expectedId(9999999999));
}

TEST(MakePersistentSampleIdRange, BracketsEveryPageOfTheSample) {
    const auto [minId, maxId] = makePersistentSampleIdRange(kUuid);
    ASSERT_LTE(minId, maxId);
    for (int pageNo : {0, 1, 42, 383, std::numeric_limits<int>::max()}) {
        const auto id = makePersistentSampleId(kUuid, pageNo);
        ASSERT_LTE(minId, id);
        ASSERT_LTE(id, maxId);
    }
}

TEST(MakePersistentSampleAllGenerationsIdRange, BoundsAreTheIdentityPrefixAndItsSuccessor) {
    const auto [minId, maxId] = makePersistentSampleAllGenerationsIdRange(kUuid);
    ASSERT_EQ(minId, expectedIdPrefix());
    ASSERT_EQ(maxId,
              std::string(str::stream()
                          << kUuid.toString() << "_" << kPersistentSampleSchemaVersion << "`"));
}

TEST(MakePersistentSampleAllGenerationsIdRange, BracketsEveryPageOfEveryGeneration) {
    const auto [minId, maxId] = makePersistentSampleAllGenerationsIdRange(kUuid);
    for (int pageNo : {0, 1, 42, 383}) {
        const auto idWithoutGenId = makePersistentSampleId(kUuid, pageNo);
        ASSERT_LTE(minId, idWithoutGenId);
        ASSERT_LT(idWithoutGenId, maxId);
        for (uint64_t genId : {uint64_t{0},
                               uint64_t{1},
                               uint64_t{9},
                               uint64_t{10},
                               uint64_t{99},
                               uint64_t{145},
                               uint64_t{2147483648},
                               std::numeric_limits<uint64_t>::max()}) {
            const auto id = makePersistentSampleId(kUuid, pageNo, genId);
            ASSERT_LTE(minId, id);
            ASSERT_LT(id, maxId);
        }
    }
}

TEST(MakePersistentSampleAllGenerationsIdRange, ExcludesOtherCollections) {
    const auto [minId, maxId] = makePersistentSampleAllGenerationsIdRange(kUuid);
    for (const auto& id :
         {makePersistentSampleId(UUID::gen(), 0), makePersistentSampleId(UUID::gen(), 0, 9)}) {
        ASSERT_TRUE(id < minId || id >= maxId) << "id unexpectedly inside range: " << id;
    }
}

// ── parsePersistentSampleGenId ────────────────────────────────────────────────────────────────

TEST(ParsePersistentSampleGenId, IdWithoutGenIdBelongsToTheDefaultGeneration) {
    for (int pageNo : {0, 7, 383}) {
        const auto genId = parsePersistentSampleGenId(makePersistentSampleId(kUuid, pageNo), kUuid);
        ASSERT_OK(genId.getStatus());
        ASSERT_EQ(genId.getValue(), kDefaultPersistentSampleGenId);
    }
}

TEST(ParsePersistentSampleGenId, ParsesTheGenIdBeforeThePageNo) {
    for (uint64_t genId : {uint64_t{0},
                           uint64_t{1},
                           uint64_t{9},
                           uint64_t{10},
                           uint64_t{145},
                           uint64_t{2147483648},
                           std::numeric_limits<uint64_t>::max()}) {
        for (int pageNo : {0, 7, 383}) {
            const auto parsed =
                parsePersistentSampleGenId(makePersistentSampleId(kUuid, pageNo, genId), kUuid);
            ASSERT_OK(parsed.getStatus());
            ASSERT_EQ(parsed.getValue(), genId);
        }
    }
    ASSERT_EQ(parsePersistentSampleGenId(expectedId(7, 145), kUuid).getValue(), 145u);
}

TEST(ParsePersistentSampleGenId, RejectsIdsOfOtherCollections) {
    const auto id = makePersistentSampleId(UUID::gen());
    ASSERT_EQ(parsePersistentSampleGenId(id, kUuid).getStatus().code(),
              ErrorCodes::UnsupportedFormat)
        << id;
}

TEST(ParsePersistentSampleGenId, RejectsMalformedGenIdsAndPageNos) {
    for (std::string_view genIdAndPageNo : {"",
                                            "000000007",
                                            "00000000007",
                                            "_0000000000",
                                            "01_0000000000",
                                            "-1_0000000000",
                                            "1a_0000000000",
                                            "1_000000000a",
                                            "1_2_0000000000",
                                            // 2^64: one past the max uint64_t, so it overflows.
                                            "18446744073709551616_0000000000"}) {
        const auto id = expectedIdPrefix() + std::string(genIdAndPageNo);
        ASSERT_EQ(parsePersistentSampleGenId(id, kUuid).getStatus().code(),
                  ErrorCodes::UnsupportedFormat)
            << id;
    }
}

// ── selectHighestCompleteGeneration ───────────────────────────────────────────────────────────

StatusWith<PersistentSampleGeneration> select(std::vector<BSONObj> pages) {
    return selectHighestCompleteGeneration(std::move(pages), kUuid);
}

void assertSelectedGeneration(const StatusWith<PersistentSampleGeneration>& selected,
                              uint64_t genId,
                              int numPages) {
    ASSERT_OK(selected.getStatus());
    const auto& generation = selected.getValue();
    ASSERT_EQ(generation.genId(), genId);
    ASSERT_EQ(generation.numPages(), numPages);
    for (int pageNo = 0; pageNo < numPages; ++pageNo) {
        const auto page = generation.page(pageNo);
        ASSERT_TRUE(page.has_value());
        ASSERT_EQ((page.value())[PersistentSampleDoc::kPageNoFieldName].Int(), pageNo);
        const auto parsedGenId = parsePersistentSampleGenId((page.value())["_id"].String(), kUuid);
        ASSERT_OK(parsedGenId.getStatus());
        ASSERT_EQ(parsedGenId.getValue(), genId);
    }
}

TEST(SelectHighestCompleteGeneration, NoPagesIsNoSuchKey) {
    ASSERT_EQ(select({}).getStatus().code(), ErrorCodes::NoSuchKey);
}

TEST(SelectHighestCompleteGeneration, SingleCompleteGenerationIsSelected) {
    assertSelectedGeneration(select({buildPage(kUuid, {.genId = 4, .pageNo = 0, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 4, .pageNo = 1, .numPages = 2})}),
                             4,
                             2);
}

TEST(SelectHighestCompleteGeneration, SingleIncompleteGenerationIsNoSuchKey) {
    ASSERT_EQ(select({buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 3}),
                      buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 3})})
                  .getStatus()
                  .code(),
              ErrorCodes::NoSuchKey);
    ASSERT_EQ(
        select({buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 2})}).getStatus().code(),
        ErrorCodes::NoSuchKey);
}

TEST(SelectHighestCompleteGeneration, NewerGenerationBeingWrittenFallsBackToOlder) {
    assertSelectedGeneration(select({buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 2, .pageNo = 0, .numPages = 3}),
                                     buildPage(kUuid, {.genId = 2, .pageNo = 1, .numPages = 3})}),
                             1,
                             2);
}

TEST(SelectHighestCompleteGeneration, BothGenerationsCompleteSelectsNewer) {
    assertSelectedGeneration(select({buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 2, .pageNo = 0, .numPages = 1})}),
                             2,
                             1);
}

TEST(SelectHighestCompleteGeneration, OlderGenerationBeingDeletedSelectsNewer) {
    assertSelectedGeneration(select({buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 2, .pageNo = 0, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 2, .pageNo = 1, .numPages = 2})}),
                             2,
                             2);
}

TEST(SelectHighestCompleteGeneration, NoGenerationCompleteIsNoSuchKey) {
    ASSERT_EQ(select({buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 2}),
                      buildPage(kUuid, {.genId = 2, .pageNo = 1, .numPages = 2})})
                  .getStatus()
                  .code(),
              ErrorCodes::NoSuchKey);
}

TEST(SelectHighestCompleteGeneration, LeftoversOfFailedRefreshesSelectHighestComplete) {
    assertSelectedGeneration(select({buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 1}),
                                     buildPage(kUuid, {.genId = 2, .pageNo = 0, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 3, .pageNo = 0, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 3, .pageNo = 1, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 4, .pageNo = 1, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 5, .pageNo = 0, .numPages = 3})}),
                             3,
                             2);
}

TEST(SelectHighestCompleteGeneration, GenerationsAreComparedNumerically) {
    assertSelectedGeneration(select({buildPage(kUuid, {.genId = 9, .pageNo = 0, .numPages = 1}),
                                     buildPage(kUuid, {.genId = 10, .pageNo = 0, .numPages = 1})}),
                             10,
                             1);
}

TEST(SelectHighestCompleteGeneration, IdsWithoutGenIdBelongToGenerationZero) {
    assertSelectedGeneration(select({buildPage(kUuid, {.pageNo = 0, .numPages = 2}),
                                     buildPage(kUuid, {.pageNo = 1, .numPages = 2}),
                                     buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 2})}),
                             0,
                             2);
    assertSelectedGeneration(select({buildPage(kUuid, {.pageNo = 0, .numPages = 1}),
                                     buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 1})}),
                             1,
                             1);
}

TEST(SelectHighestCompleteGeneration, MissingOrNonPositiveNumPagesIsUnsupportedFormat) {
    for (auto&& page : {buildPage(kUuid, {.pageNo = 0}),
                        buildPage(kUuid, {.pageNo = 0, .numPages = 0}),
                        buildPage(kUuid, {.pageNo = 0, .numPages = -1})}) {
        const auto status = select({page}).getStatus();
        ASSERT_EQ(status.code(), ErrorCodes::UnsupportedFormat);
        ASSERT_STRING_CONTAINS(status.reason(), "missing or non-positive numPages");
    }
}

TEST(SelectHighestCompleteGeneration, HugeNumPagesMakesAGenerationIncomplete) {
    // Only the pages read are stored, so a huge numPages costs nothing and simply leaves the
    // generation incomplete.
    const int hugeNumPages = std::numeric_limits<int>::max();
    assertSelectedGeneration(
        select({buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 1}),
                buildPage(kUuid, {.genId = 2, .pageNo = 0, .numPages = hugeNumPages})}),
        1,
        1);
    ASSERT_EQ(select({buildPage(kUuid, {.genId = 2, .pageNo = 0, .numPages = hugeNumPages})})
                  .getStatus()
                  .code(),
              ErrorCodes::NoSuchKey);
}

TEST(SelectHighestCompleteGeneration, PagesOfAGenerationMayArriveInAnyOrder) {
    assertSelectedGeneration(select({buildPage(kUuid, {.genId = 1, .pageNo = 2, .numPages = 3}),
                                     buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 3}),
                                     buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 3})}),
                             1,
                             3);
}

TEST(SelectHighestCompleteGeneration, PageBeyondNumPagesIsUnsupportedFormat) {
    ASSERT_EQ(select({buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 1}),
                      buildPage(kUuid, {.genId = 2, .pageNo = 0, .numPages = 1}),
                      buildPage(kUuid, {.genId = 2, .pageNo = 1, .numPages = 1})})
                  .getStatus()
                  .code(),
              ErrorCodes::UnsupportedFormat);
}

TEST(SelectHighestCompleteGeneration, PagesDisagreeingOnNumPagesAreUnsupportedFormat) {
    // Whichever page is read first, a generation whose pages disagree on numPages is malformed.
    ASSERT_EQ(select({buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 1}),
                      buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 2})})
                  .getStatus()
                  .code(),
              ErrorCodes::UnsupportedFormat);
    ASSERT_EQ(select({buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 3}),
                      buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 2}),
                      buildPage(kUuid, {.genId = 1, .pageNo = 2, .numPages = 3})})
                  .getStatus()
                  .code(),
              ErrorCodes::UnsupportedFormat);
    ASSERT_EQ(select({buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 1}),
                      buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 3})})
                  .getStatus()
                  .code(),
              ErrorCodes::UnsupportedFormat);
}

TEST(SelectHighestCompleteGeneration, DuplicatePageNoIsUnsupportedFormat) {
    ASSERT_EQ(select({buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 1}),
                      buildPage(kUuid, {.genId = 2, .pageNo = 0, .numPages = 2}),
                      buildPage(kUuid, {.genId = 2, .pageNo = 0, .numPages = 2})})
                  .getStatus()
                  .code(),
              ErrorCodes::UnsupportedFormat);
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
    ASSERT_EQUALS(static_cast<size_t>(sample.getRequestedSampleSize()), docs.size());
    ASSERT_FALSE(sample.getNumChunks().has_value());
    ASSERT_EQUALS(sample.getPageNo(), 0);

    ASSERT_EQUALS(sample.getCreatedAt(),
                  sampleDoc[PersistentSampleDoc::kCreatedAtFieldName].date());
    ASSERT_EQUALS(sample.getDocs().size(), docs.size());
    ASSERT_BSONOBJ_EQ(sample.getDocs()[0], docs[0]);
    ASSERT_BSONOBJ_EQ(sample.getDocs()[1], docs[1]);
}

TEST(ParsePersistentSample, IgnoresUnknownTopLevelField) {
    // Samples written by a newer version may carry fields this version does not know about.
    const UUID uuid = UUID::gen();
    const std::vector<BSONObj> docs{BSON("_id" << 1 << "a" << 10), BSON("_id" << 2 << "a" << 20)};
    const auto sampleDoc =
        buildPersistentSampleDoc(uuid,
                                 SamplingTechniqueEnum::kRandom,
                                 docs.size(),
                                 docs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 /*overrides=*/BSON("unknownField" << "value"));
    ASSERT_TRUE(sampleDoc.hasField("unknownField"));
    auto result = parsePersistentSample(sampleDoc);
    ASSERT_OK(result.getStatus());
    const auto& sample = result.getValue();

    ASSERT_EQUALS(sample.getCollectionUuid(), uuid.toString());
    ASSERT_EQUALS(sample.getSamplingMethod(), SamplingTechniqueEnum::kRandom);
    ASSERT_EQUALS(static_cast<size_t>(sample.getRequestedSampleSize()), docs.size());
    ASSERT_EQUALS(sample.getPageNo(), 0);
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

TEST(ParsePersistentSample, RejectsDocsArrayLargerThanRequestedSampleSize) {
    // 'docs' must not contain more entries than 'requestedSampleSize'. Fewer is allowed: chunk
    // sampling may collect less than the requested size when a chunk starts near end-of-collection.
    const std::vector<BSONObj> docs{BSON("a" << 1), BSON("a" << 2)};

    // docs.size() > requestedSampleSize — rejected.
    auto docsExceedDeclared =
        parsePersistentSample(buildPersistentSampleDoc(UUID::gen(),
                                                       SamplingTechniqueEnum::kRandom,
                                                       /*requestedSampleSize=*/docs.size() - 1,
                                                       docs));
    ASSERT_NOT_OK(docsExceedDeclared.getStatus());
    ASSERT_EQUALS(docsExceedDeclared.getStatus().code(), ErrorCodes::UnsupportedFormat);

    // docs.size() < requestedSampleSize — allowed (underfull chunk sample).
    auto underFull =
        parsePersistentSample(buildPersistentSampleDoc(UUID::gen(),
                                                       SamplingTechniqueEnum::kRandom,
                                                       /*requestedSampleSize=*/docs.size() + 1,
                                                       docs));
    ASSERT_OK(underFull.getStatus());
}

TEST(ParsePersistentSample, RejectsEmptySample) {
    auto result = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(), SamplingTechniqueEnum::kRandom, /*requestedSampleSize=*/0, /*docs=*/{}));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsMissingSchemaVersion) {
    BSONObjBuilder b;
    b.append(PersistentSampleDoc::kCollectionUuidFieldName, UUID::gen().toString());
    b.append(PersistentSampleDoc::kRequestedSampleSizeFieldName,
             static_cast<long long>(kStubSampleDocs.size()));
    b.append(PersistentSampleDoc::kSamplingMethodFieldName,
             idlSerialize(SamplingTechniqueEnum::kRandom));
    b.append(PersistentSampleDoc::kRequestedSamplingMethodFieldName,
             idlSerialize(SamplingTechniqueEnum::kRandom));
    b.appendDate(PersistentSampleDoc::kCreatedAtFieldName, Date_t::now());
    b.append(PersistentSampleDoc::kNumDocsDiscardedFieldName, static_cast<long long>(0));
    BSONArrayBuilder arr(b.subarrayStart(PersistentSampleDoc::kDocsFieldName));
    for (const auto& d : kStubSampleDocs) {
        arr.append(d);
    }
    arr.done();
    auto result = parsePersistentSample(b.obj());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsBelowMinSchemaVersion) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/0));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNonNumericSchemaVersion) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
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
        /*requestedSampleSize=*/kStubSampleDocs.size(),
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
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kSamplingMethodFieldName << "bogus")));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNonNumericSize) {
    auto result = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(),
        SamplingTechniqueEnum::kRandom,
        /*requestedSampleSize=*/kStubSampleDocs.size(),
        /*docs=*/kStubSampleDocs,
        /*numChunks=*/boost::none,
        /*schemaVersion=*/kPersistentSampleSchemaVersion,
        BSON(PersistentSampleDoc::kRequestedSampleSizeFieldName << "huge")));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNegativeSize) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kRequestedSampleSizeFieldName << -1LL)));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsChunkSampleWithoutNumChunks) {
    // Chunk technique without chunk_size — malformed.
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kChunk,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ParsePersistentSample, RejectsFullCollScanSampleWithNumChunks) {
    // A full collection scan sample requested as a chunk sample was not generated in chunks.
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kChunk,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/2,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kSamplingMethodFieldName
                                      << idlSerialize(SamplingTechniqueEnum::kFullCollScan))));
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ParsePersistentSample, RejectsNonPositiveNumChunks) {
    // chunk_size of zero is invalid — chunks must contain at least one doc.
    auto zeroResult = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kChunk,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/0));
    ASSERT_NOT_OK(zeroResult.getStatus());

    auto negativeResult = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kChunk,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/-1));
    ASSERT_NOT_OK(negativeResult.getStatus());
}

TEST(ParsePersistentSample, RejectsNegativePageNo) {
    // pageNo is defined in the idl to be >= 0
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/
                                 kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kPageNoFieldName << -1)));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, PopulatesNumPages) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 /*overrides=*/BSONObj{},
                                 /*pageNo=*/0,
                                 /*numDocsDiscarded=*/0,
                                 /*genId=*/boost::none,
                                 /*numPages=*/3));
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().getNumPages(), 3);
}

TEST(ParsePersistentSample, AcceptsMissingNumPages) {
    auto result = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(), SamplingTechniqueEnum::kRandom, kStubSampleDocs.size(), kStubSampleDocs));
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().getNumPages().has_value());
}

TEST(ParsePersistentSample, RejectsNonPositiveNumPages) {
    for (int numPages : {0, -1}) {
        auto result = parsePersistentSample(
            buildPersistentSampleDoc(UUID::gen(),
                                     SamplingTechniqueEnum::kRandom,
                                     /*requestedSampleSize=*/kStubSampleDocs.size(),
                                     /*docs=*/kStubSampleDocs,
                                     /*numChunks=*/boost::none,
                                     /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                     /*overrides=*/BSONObj{},
                                     /*pageNo=*/0,
                                     /*numDocsDiscarded=*/0,
                                     /*genId=*/boost::none,
                                     numPages));
        ASSERT_NOT_OK(result.getStatus());
    }
}

TEST(ParsePersistentSample, RejectsNonNumericNumPages) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kNumPagesFieldName << "three")));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNonDateCreatedAt) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kCreatedAtFieldName << "yesterday")));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsMissingDocsField) {
    const UUID uuid = UUID::gen();
    BSONObjBuilder b;
    b.append("_id", makePersistentSampleId(uuid));
    b.append(PersistentSampleDoc::kPageNoFieldName, 0);
    b.append(PersistentSampleDoc::kCollectionUuidFieldName, uuid.toString());
    b.append(PersistentSampleDoc::kSchemaVersionFieldName, kPersistentSampleSchemaVersion);
    b.appendDate(PersistentSampleDoc::kCreatedAtFieldName, Date_t::now());
    b.append(PersistentSampleDoc::kRequestedSampleSizeFieldName,
             static_cast<long long>(kStubSampleDocs.size()));
    b.append(PersistentSampleDoc::kSamplingMethodFieldName,
             idlSerialize(SamplingTechniqueEnum::kRandom));
    b.append(PersistentSampleDoc::kRequestedSamplingMethodFieldName,
             idlSerialize(SamplingTechniqueEnum::kRandom));
    b.append(PersistentSampleDoc::kNumDocsDiscardedFieldName, static_cast<long long>(0));
    auto result = parsePersistentSample(b.obj());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNonArrayDocsField) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
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
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kDocsFieldName << arr.arr())));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNegativeNumDocsDiscarded) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingTechniqueEnum::kRandom,
                                 /*requestedSampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kNumDocsDiscardedFieldName << -1)));
    ASSERT_NOT_OK(result.getStatus());
}

// ── PersistentSampleGeneration ────────────────────────────────────────────────────────────────

TEST(PersistentSampleGeneration, PagesAreFoundByPageNoWhateverTheirArrivalOrder) {
    PersistentSampleGeneration generation(/*genId=*/1, /*numPages=*/3);
    for (int pageNo : {2, 0, 1}) {
        ASSERT_FALSE(generation.isComplete());
        ASSERT_OK(
            generation.addPage(buildPage(kUuid, {.genId = 1, .pageNo = pageNo, .numPages = 3})));
    }
    ASSERT_TRUE(generation.isComplete());
    for (int pageNo = 0; pageNo < 3; ++pageNo) {
        const auto page = generation.page(pageNo);
        ASSERT_TRUE(page.has_value());
        ASSERT_EQ((page.value())[PersistentSampleDoc::kPageNoFieldName].Int(), pageNo);
    }
    ASSERT_FALSE(generation.page(3).has_value());
}

TEST(PersistentSampleGeneration, IsCompleteOnlyOnceEveryPageIsAdded) {
    PersistentSampleGeneration generation(/*genId=*/1, /*numPages=*/2);
    ASSERT_FALSE(generation.isComplete());
    ASSERT_OK(generation.addPage(buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 2})));
    ASSERT_FALSE(generation.isComplete());
    ASSERT_OK(generation.addPage(buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 2})));
    ASSERT_TRUE(generation.isComplete());
}

DEATH_TEST_REGEX(PersistentSampleGenerationDeathTest, NonPositiveNumPagesIsRejected, "13541500") {
    PersistentSampleGeneration(/*genId=*/1, /*numPages=*/0);
}

TEST(PersistentSampleGeneration, RejectsPageDisagreeingOnNumPages) {
    PersistentSampleGeneration generation(/*genId=*/1, /*numPages=*/2);
    for (auto&& page : {buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 3}),
                        buildPage(kUuid, {.genId = 1, .pageNo = 1}),
                        buildPage(kUuid, {.genId = 1, .pageNo = 1, .numPages = 0}),
                        buildPage(kUuid, {.genId = 1, .pageNo = 2, .numPages = 2})}) {
        ASSERT_EQ(generation.addPage(page).code(), ErrorCodes::UnsupportedFormat);
    }
    ASSERT_EQ(generation.numPages(), 0u);
}

TEST(PersistentSampleGeneration, RejectsDuplicatePageNo) {
    PersistentSampleGeneration generation(/*genId=*/1, /*numPages=*/2);
    ASSERT_OK(generation.addPage(
        buildPage(kUuid, {.genId = 1, .pageNo = 0, .numPages = 2, .docs = {BSON("a" << 0)}})));
    ASSERT_EQ(generation
                  .addPage(buildPage(
                      kUuid, {.genId = 1, .pageNo = 0, .numPages = 2, .docs = {BSON("a" << 1)}}))
                  .code(),
              ErrorCodes::UnsupportedFormat);
    ASSERT_EQ(generation.numPages(), 1u);
    ASSERT_BSONOBJ_EQ((generation.page(0).value())["docs"].Array()[0].Obj(), BSON("a" << 0));
}

// ── reassemblePersistentSample ────────────────────────────────────────────────────────────────

DEATH_TEST_REGEX(ReassemblePersistentSampleDeathTest, EmptyGenerationIsRejected, "13541501") {
    reassemblePersistentSample(PersistentSampleGeneration(/*genId=*/1, /*numPages=*/1))
        .getStatus()
        .ignore();
}

DEATH_TEST_REGEX(ReassemblePersistentSampleDeathTest, IncompleteGenerationIsRejected, "13541501") {
    const UUID uuid = UUID::gen();
    const std::vector<BSONObj> pages({buildPage(uuid, {.pageNo = 0, .numPages = 3}),
                                      buildPage(uuid, {.pageNo = 2, .numPages = 3})});
    PersistentSampleGeneration generation(kDefaultPersistentSampleGenId, 3);
    for (auto& page : pages) {
        ASSERT_OK(generation.addPage(std::move(page)));
    }
    ASSERT_FALSE(generation.isComplete());
    reassemblePersistentSample(generation).getStatus().ignore();
}

TEST(ReassemblePersistentSample, SinglePageRoundTrips) {
    const UUID uuid = UUID::gen();
    const std::vector<BSONObj> docs{BSON("_id" << 1), BSON("_id" << 2)};
    auto result = reassemblePersistentSample(generationOf(
        {buildPage(uuid, {.requestedSampleSize = docs.size(), .numPages = 1, .docs = docs})}));
    ASSERT_OK(result.getStatus());
    const auto& sample = result.getValue();
    ASSERT_EQUALS(sample.getDocs().size(), docs.size());
    ASSERT_BSONOBJ_EQ(sample.getDocs()[0], docs[0]);
    ASSERT_BSONOBJ_EQ(sample.getDocs()[1], docs[1]);
}

TEST(ReassemblePersistentSample, MultiplePagesConcatenatedInPageNoOrder) {
    const UUID uuid = UUID::gen();
    const std::vector<BSONObj> docs{BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 2)};
    const auto pageOf = [&](int pageNo) {
        return buildPage(uuid,
                         {.requestedSampleSize = docs.size(),
                          .pageNo = pageNo,
                          .numPages = 3,
                          .docs = {docs[pageNo]}});
    };

    // Pages are added out of order; reassembly still concatenates them in pageNo order.
    auto result = reassemblePersistentSample(generationOf({pageOf(2), pageOf(0), pageOf(1)}));
    ASSERT_OK(result.getStatus());
    const auto& sample = result.getValue();
    ASSERT_EQUALS(sample.getDocs().size(), docs.size());
    for (size_t i = 0; i < docs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(sample.getDocs()[i], docs[i]);
    }
}

TEST(ReassemblePersistentSample, IdentityMismatchAcrossPagesRejected) {
    const UUID uuid = UUID::gen();
    // page 0 declares requestedSampleSize 4, page 1 declares 5 — inconsistent identity.
    auto result = reassemblePersistentSample(generationOf({
        buildPage(uuid,
                  {.requestedSampleSize = 4, .pageNo = 0, .numPages = 2, .docs = {BSON("a" << 0)}}),
        buildPage(uuid,
                  {.requestedSampleSize = 5, .pageNo = 1, .numPages = 2, .docs = {BSON("a" << 1)}}),
    }));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, ReassembledDocsExceedingRequestedSampleSizeRejected) {
    const UUID uuid = UUID::gen();
    // Each page is individually within requestedSampleSize=2, but combined they hold 3 docs.
    auto result = reassemblePersistentSample(generationOf({
        buildPage(uuid,
                  {.requestedSampleSize = 2,
                   .pageNo = 0,
                   .numPages = 2,
                   .docs = {BSON("a" << 0), BSON("a" << 1)}}),
        buildPage(uuid,
                  {.requestedSampleSize = 2, .pageNo = 1, .numPages = 2, .docs = {BSON("a" << 2)}}),
    }));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, MalformedPagePropagatesError) {
    const UUID uuid = UUID::gen();
    // Page 1 belongs to the generation but its `docs` is not an array, so it fails to parse.
    const BSONObj malformed =
        buildPersistentSampleDoc(uuid,
                                 SamplingTechniqueEnum::kRandom,
                                 /*requestedSampleSize=*/2,
                                 /*docs=*/{BSON("a" << 1)},
                                 /*numChunks=*/boost::none,
                                 kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kDocsFieldName << "not-an-array"),
                                 /*pageNo=*/1,
                                 /*numDocsDiscarded=*/0,
                                 /*genId=*/boost::none,
                                 /*numPages=*/2);
    auto result = reassemblePersistentSample(generationOf({
        buildPage(uuid,
                  {.requestedSampleSize = 2, .pageNo = 0, .numPages = 2, .docs = {BSON("a" << 0)}}),
        malformed,
    }));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ReassemblePersistentSample, PreservesChunkNumChunks) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample(generationOf({
        buildPage(uuid,
                  {.method = SamplingTechniqueEnum::kChunk,
                   .numChunks = 1,
                   .pageNo = 0,
                   .numPages = 2,
                   .docs = {BSON("a" << 0)}}),
        buildPage(uuid,
                  {.method = SamplingTechniqueEnum::kChunk,
                   .numChunks = 1,
                   .pageNo = 1,
                   .numPages = 2,
                   .docs = {BSON("a" << 1)}}),
    }));
    ASSERT_OK(result.getStatus());
    const auto& sample = result.getValue();
    ASSERT_EQUALS(sample.getSamplingMethod(), SamplingTechniqueEnum::kChunk);
    ASSERT_TRUE(sample.getNumChunks().has_value());
    ASSERT_EQUALS(sample.getNumChunks().value(), 1);
    ASSERT_EQUALS(sample.getDocs().size(), 2u);
}

TEST(ReassemblePersistentSample, AcceptsAllPagesDeclaredByNumPages) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample(
        generationOf({buildPage(uuid, {.pageNo = 0, .numPages = 2, .docs = {BSON("a" << 0)}}),
                      buildPage(uuid, {.pageNo = 1, .numPages = 2, .docs = {BSON("a" << 1)}})}));
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(result.getValue().getDocs().size(), 2u);
}

// ── PersistentSampleLoader ────────────────────────────────────────────────────────────────────

TEST_F(SamplingEstimatorTest, PersistentLoaderLoadsSampleRegardlessOfGenerationMethod) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    // A collection has at most one persisted sample, so it is found whatever method the spec asks
    // for.
    persistSampleForTest(operationContext(),
                         _kTestNss,
                         SamplingTechniqueEnum::kChunk,
                         /*requestedSampleSize=*/4,
                         /*numDocs=*/4,
                         /*numChunks=*/2);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    for (const auto& spec : {makeSpec(_kTestNss, 4),
                             makeSpec(_kTestNss,
                                      4,
                                      SamplingCEMethodEnum::kChunk,
                                      SamplingSourceEnum::kPersistentSample,
                                      /*numChunks=*/4)}) {
        auto loaded = loader.tryLoad(spec);
        ASSERT_OK(loaded.getStatus());
        assertIsPersistedPrefix(loaded.getValue(), 4);
        // The technique reported is the one the persisted sample was generated with.
        ASSERT_TRUE(loaded.getValue().technique() == SamplingTechniqueEnum::kChunk);
    }
}

TEST_F(SamplingEstimatorTest, PersistentLoaderIgnoresSampleSmallerThanRequested) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    persistSampleForTest(operationContext(),
                         _kTestNss,
                         SamplingTechniqueEnum::kRandom,
                         /*requestedSampleSize=*/4,
                         /*numDocs=*/4);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    auto loaded = loader.tryLoad(makeSpec(_kTestNss, 5));
    ASSERT_EQUALS(loaded.getStatus().code(), ErrorCodes::NoSuchKey);
}

TEST_F(SamplingEstimatorTest, PersistentLoaderUsesPrefixOfLargerSample) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    persistSampleForTest(operationContext(),
                         _kTestNss,
                         SamplingTechniqueEnum::kRandom,
                         /*requestedSampleSize=*/10,
                         /*numDocs=*/10);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    // Requests of different sizes share the sample, each using the prefix it needs.
    for (size_t requestedSize : {1, 4, 10}) {
        auto loaded = loader.tryLoad(makeSpec(_kTestNss, requestedSize));
        ASSERT_OK(loaded.getStatus());
        assertIsPersistedPrefix(loaded.getValue(), requestedSize);
        ASSERT_EQUALS(loaded.getValue().numPages().value_or(0), 1u);
    }
}

TEST_F(SamplingEstimatorTest, PersistentLoaderUsesPrefixOfLargerChunkSample) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    persistSampleForTest(operationContext(),
                         _kTestNss,
                         SamplingTechniqueEnum::kChunk,
                         /*requestedSampleSize=*/10,
                         /*numDocs=*/10,
                         /*numChunks=*/2);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    auto loaded = loader.tryLoad(makeSpec(_kTestNss, 3));
    ASSERT_OK(loaded.getStatus());
    assertIsPersistedPrefix(loaded.getValue(), 3);
    ASSERT_TRUE(loaded.getValue().technique() == SamplingTechniqueEnum::kChunk);
}

TEST_F(SamplingEstimatorTest, PersistentLoaderUsesPrefixOfFullCollScanSample) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    // A sample of a collection smaller than the requested sample size is a full collection scan,
    // so it holds fewer docs than requested.
    constexpr size_t kNumDocs = 5;
    persistSampleForTest(operationContext(),
                         _kTestNss,
                         SamplingTechniqueEnum::kRandom,
                         /*requestedSampleSize=*/10,
                         kNumDocs,
                         /*numChunks=*/boost::none,
                         /*overrides=*/
                         BSON(PersistentSampleDoc::kSamplingMethodFieldName
                              << idlSerialize(SamplingTechniqueEnum::kFullCollScan)));

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    // A smaller request uses the prefix it needs; a request at least as large as the doc count
    // uses every doc.
    for (size_t requestedSize : {2, 5, 10}) {
        auto loaded = loader.tryLoad(makeSpec(_kTestNss, requestedSize));
        ASSERT_OK(loaded.getStatus());
        assertIsPersistedPrefix(loaded.getValue(), std::min(requestedSize, kNumDocs));
        ASSERT_TRUE(loaded.getValue().technique() == SamplingTechniqueEnum::kFullCollScan);
    }
    // The sample's requested size still bounds which requests it can serve.
    ASSERT_EQUALS(loader.tryLoad(makeSpec(_kTestNss, 11)).getStatus().code(),
                  ErrorCodes::NoSuchKey);
}

TEST_F(SamplingEstimatorTest, PersistentLoaderReportsNumChunksOfChunkSample) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    persistSampleForTest(operationContext(),
                         _kTestNss,
                         SamplingTechniqueEnum::kChunk,
                         /*requestedSampleSize=*/4,
                         /*numDocs=*/4,
                         /*numChunks=*/2);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    // The sample reports the number of chunks it was generated with, not the requested one.
    auto loaded = loader.tryLoad(makeSpec(_kTestNss,
                                          4,
                                          SamplingCEMethodEnum::kChunk,
                                          SamplingSourceEnum::kPersistentSample,
                                          /*numChunks=*/7));
    ASSERT_OK(loaded.getStatus());
    assertIsPersistedPrefix(loaded.getValue(), 4);
    ASSERT_EQ(loaded.getValue().numChunks(), boost::optional<int>(2));
}

TEST_F(SamplingEstimatorTest, PersistentLoaderUsesSampleWithFewerDocsThanItsRequestedSize) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    // A sample may hold fewer docs than it was requested with (e.g. docs too large to persist were
    // discarded). Whether it can serve a request depends on its requested size, not its doc count.
    persistSampleForTest(operationContext(),
                         _kTestNss,
                         SamplingTechniqueEnum::kRandom,
                         /*requestedSampleSize=*/10,
                         /*numDocs=*/8);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    auto loaded = loader.tryLoad(makeSpec(_kTestNss, 10));
    ASSERT_OK(loaded.getStatus());
    assertIsPersistedPrefix(loaded.getValue(), 8);
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

TEST_F(SamplingEstimatorTest, PersistentLoaderLoadsSampleWithGenIdInItsIds) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    const UUID uuid = [&] {
        auto srcColl = acquireCollectionForSampling(operationContext(), _kTestNss);
        return srcColl.getCollectionPtr()->uuid();
    }();
    const std::vector<BSONObj> persistedDocs{BSON("_id" << 10 << "tag" << "persisted"),
                                             BSON("_id" << 11 << "tag" << "persisted")};
    createCollAndInsertDocuments(
        operationContext(),
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName),
        {buildPage(uuid,
                   {.requestedSampleSize = persistedDocs.size(),
                    .genId = 9,
                    .pageNo = 0,
                    .numPages = 2,
                    .docs = {persistedDocs[0]}}),
         buildPage(uuid,
                   {.requestedSampleSize = persistedDocs.size(),
                    .genId = 9,
                    .pageNo = 1,
                    .numPages = 2,
                    .docs = {persistedDocs[1]}})},
        /*clustered=*/true);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    auto loaded = loader.tryLoad(makeSpec(_kTestNss, persistedDocs.size()));
    ASSERT_OK(loaded.getStatus());
    ASSERT_EQUALS(loaded.getValue().numPages().value(), 2u);
    ASSERT_EQUALS(loaded.getValue().size(), persistedDocs.size());
    for (size_t i = 0; i < persistedDocs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(loaded.getValue().docs()[i], persistedDocs[i]);
    }
}

// Count of the pagesRead histogram bucket whose lower bound is `lowerBound`.
long long pagesReadBucketCount(long long lowerBound) {
    BSONObjBuilder builder;
    globalMetricTreeSet()[ClusterRole::None].appendTo(builder);
    // Keep the root alive: getObjectField returns views into its buffer.
    const BSONObj root = builder.obj();
    const BSONObj histograms = root.getObjectField("metrics")
                                   .getObjectField("query")
                                   .getObjectField("sampling")
                                   .getObjectField("persistentSample")
                                   .getObjectField("histograms");
    for (const auto& bucket : histograms["pagesRead"].Array()) {
        if (bucket["lowerBound"].numberLong() == lowerBound) {
            return bucket["count"].numberLong();
        }
    }
    FAIL(std::string(str::stream() << "no pagesRead bucket with lowerBound " << lowerBound));
    return 0;
}

TEST_F(SamplingEstimatorTest, PersistentLoaderLoadsHighestCompleteGeneration) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss,
                    {BSON("_id" << 1 << "tag" << "live"),
                     BSON("_id" << 2 << "tag" << "live"),
                     BSON("_id" << 3 << "tag" << "live")});
    const UUID uuid = [&] {
        auto srcColl = acquireCollectionForSampling(operationContext(), _kTestNss);
        return srcColl.getCollectionPtr()->uuid();
    }();
    createCollAndInsertDocuments(
        operationContext(),
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName),
        {buildPage(uuid,
                   {.requestedSampleSize = 2,
                    .genId = 9,
                    .pageNo = 0,
                    .numPages = 2,
                    .docs = {BSON("genId" << 9)}}),
         buildPage(uuid,
                   {.requestedSampleSize = 2,
                    .genId = 9,
                    .pageNo = 1,
                    .numPages = 2,
                    .docs = {BSON("genId" << 9)}}),
         buildPage(uuid,
                   {.requestedSampleSize = 2,
                    .genId = 10,
                    .pageNo = 0,
                    .numPages = 2,
                    .docs = {BSON("genId" << 10)}}),
         buildPage(uuid,
                   {.requestedSampleSize = 2,
                    .genId = 10,
                    .pageNo = 1,
                    .numPages = 2,
                    .docs = {BSON("genId" << 10)}}),
         buildPage(uuid,
                   {.requestedSampleSize = 2,
                    .genId = 11,
                    .pageNo = 0,
                    .numPages = 2,
                    .docs = {BSON("genId" << 11)}})},
        /*clustered=*/true);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    // All 5 scanned pages count as read, not only the 2 of the selected generation. With bounds
    // 1, 2, 4, ... that is the [4, 8) bucket.
    const long long pagesReadBefore = pagesReadBucketCount(4);
    auto loaded = loader.tryLoad(makeSpec(_kTestNss, 2));
    ASSERT_OK(loaded.getStatus());
    ASSERT_EQUALS(loaded.getValue().numPages().value(), 2u);
    ASSERT_EQUALS(loaded.getValue().size(), 2u);
    for (const auto& doc : loaded.getValue().docs()) {
        ASSERT_EQUALS(doc["genId"].numberInt(), 10);
    }
    ASSERT_EQUALS(pagesReadBucketCount(4), pagesReadBefore + 1);
}

TEST_F(SamplingEstimatorTest, PersistentLoaderMissesWhenNoGenerationIsComplete) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss,
                    {BSON("_id" << 1 << "tag" << "live"),
                     BSON("_id" << 2 << "tag" << "live"),
                     BSON("_id" << 3 << "tag" << "live")});
    const UUID uuid = [&] {
        auto srcColl = acquireCollectionForSampling(operationContext(), _kTestNss);
        return srcColl.getCollectionPtr()->uuid();
    }();
    createCollAndInsertDocuments(
        operationContext(),
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName),
        {buildPage(uuid, {.requestedSampleSize = 2, .genId = 1, .pageNo = 1, .numPages = 2}),
         buildPage(uuid, {.requestedSampleSize = 2, .genId = 2, .pageNo = 0, .numPages = 2})},
        /*clustered=*/true);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    auto loaded = loader.tryLoad(makeSpec(_kTestNss, 2));
    ASSERT_EQUALS(loaded.getStatus().code(), ErrorCodes::NoSuchKey);
}

TEST_F(SamplingEstimatorTest, PersistentLoaderRejectsMalformedHighestCompleteGeneration) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss,
                    {BSON("_id" << 1 << "tag" << "live"),
                     BSON("_id" << 2 << "tag" << "live"),
                     BSON("_id" << 3 << "tag" << "live")});
    const UUID uuid = [&] {
        auto srcColl = acquireCollectionForSampling(operationContext(), _kTestNss);
        return srcColl.getCollectionPtr()->uuid();
    }();
    createCollAndInsertDocuments(
        operationContext(),
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName),
        {buildPage(uuid, {.requestedSampleSize = 2, .genId = 1, .pageNo = 0, .numPages = 1}),
         buildPage(uuid, {.requestedSampleSize = 2, .genId = 2, .pageNo = 0, .numPages = 2}),
         buildPage(uuid,
                   {.requestedSampleSize = 2,
                    .genId = 2,
                    .pageNo = 1,
                    .numPages = 2,
                    .numDocsDiscarded = 1})},
        /*clustered=*/true);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    auto loaded = loader.tryLoad(makeSpec(_kTestNss, 2));
    ASSERT_EQUALS(loaded.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST_F(SamplingEstimatorTest, PersistentLoaderIgnoresSamplesOfOtherCollections) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    createCollAndInsertDocuments(
        operationContext(),
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName),
        {buildPersistentSampleDoc(
            UUID::gen(), SamplingTechniqueEnum::kRandom, 1, {BSON("_id" << 10)})},
        /*clustered=*/true);

    auto coll = acquireCollectionForSampling(operationContext(), _kTestNss);
    MultipleCollectionAccessor colls(std::move(coll));
    PersistentSampleLoader loader(operationContext(),
                                  colls,
                                  PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                  nullptr /*customerQueryExpCtx*/);
    auto loaded = loader.tryLoad(makeSpec(_kTestNss, 1));
    ASSERT_EQUALS(loaded.getStatus().code(), ErrorCodes::NoSuchKey);
}

}  // namespace
}  // namespace mongo::ce
