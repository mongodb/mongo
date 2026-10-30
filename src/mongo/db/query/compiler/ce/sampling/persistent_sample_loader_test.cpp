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

#include <limits>
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

TEST(MakePersistentSampleId, GenIdPrecedesThePageNoUnpadded) {
    ASSERT_EQ(
        makePersistentSampleId(
            kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none, /*pageNo=*/7, /*genId=*/0),
        expectedId("random", "384_0_007"));
    ASSERT_EQ(makePersistentSampleId(kUuid,
                                     SamplingTechniqueEnum::kRandom,
                                     384,
                                     boost::none,
                                     /*pageNo=*/7,
                                     /*genId=*/145),
              expectedId("random", "384_145_007"));
    ASSERT_EQ(makePersistentSampleId(kUuid,
                                     SamplingTechniqueEnum::kChunk,
                                     384,
                                     /*numChunks=*/10,
                                     /*pageNo=*/0,
                                     /*genId=*/3),
              expectedId("chunk", "384_10_3_000"));
    // 18446744073709551615 is 2^64 - 1, the max uint64_t.
    ASSERT_EQ(makePersistentSampleId(kUuid,
                                     SamplingTechniqueEnum::kRandom,
                                     384,
                                     boost::none,
                                     /*pageNo=*/0,
                                     /*genId=*/std::numeric_limits<uint64_t>::max()),
              expectedId("random", "384_18446744073709551615_000"));
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

TEST(MakePersistentSampleIdRange, DifferentNumChunksYieldDisjointRanges) {
    // The chunk count is part of the shared prefix, so a different one yields a disjoint range.
    const auto [minId, maxId] =
        makePersistentSampleIdRange(kUuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/10);
    const auto [otherMin, otherMax] =
        makePersistentSampleIdRange(kUuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/20);
    ASSERT_TRUE(otherMin > maxId || otherMax < minId);
}

TEST(MakePersistentSampleAllGenerationsIdRange, BoundsAreTheIdentityPrefixAndItsSuccessor) {
    const auto [minId, maxId] = makePersistentSampleAllGenerationsIdRange(
        kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none);
    ASSERT_EQ(minId, expectedId("random", "384_"));
    ASSERT_EQ(maxId, expectedId("random", "384`"));

    const auto [chunkMin, chunkMax] = makePersistentSampleAllGenerationsIdRange(
        kUuid, SamplingTechniqueEnum::kChunk, 384, /*numChunks=*/10);
    ASSERT_EQ(chunkMin, expectedId("chunk", "384_10_"));
    ASSERT_EQ(chunkMax, expectedId("chunk", "384_10`"));
}

TEST(MakePersistentSampleAllGenerationsIdRange, BracketsEveryPageOfEveryGeneration) {
    const auto [minId, maxId] = makePersistentSampleAllGenerationsIdRange(
        kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none);
    for (int pageNo : {0, 1, 42, 383}) {
        const auto idWithoutGenId =
            makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none, pageNo);
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
            const auto id = makePersistentSampleId(
                kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none, pageNo, genId);
            ASSERT_LTE(minId, id);
            ASSERT_LT(id, maxId);
        }
    }
}

TEST(MakePersistentSampleAllGenerationsIdRange, ExcludesOtherSamples) {
    const auto [minId, maxId] = makePersistentSampleAllGenerationsIdRange(
        kUuid, SamplingTechniqueEnum::kRandom, 1, boost::none);

    const std::vector<std::string> outsiders{
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 10, boost::none, 9),
        makePersistentSampleId(
            kUuid, SamplingTechniqueEnum::kRandom, 10, boost::none, /*pageNo=*/9, /*genId=*/9),
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 100, boost::none, 0),
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kChunk, 1, /*numChunks=*/1),
        makePersistentSampleId(kUuid, SamplingTechniqueEnum::kSeqScan, 1, boost::none),
        makePersistentSampleId(UUID::gen(), SamplingTechniqueEnum::kRandom, 1, boost::none),
    };
    for (const auto& id : outsiders) {
        ASSERT_TRUE(id < minId || id >= maxId) << "id unexpectedly inside range: " << id;
    }
}

StatusWith<uint64_t> parseRandomGenId(std::string_view id) {
    return parsePersistentSampleGenId(id, kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none);
}

TEST(ParsePersistentSampleGenId, IdWithoutGenIdBelongsToTheDefaultGeneration) {
    for (int pageNo : {0, 7, 383}) {
        const auto genId = parseRandomGenId(makePersistentSampleId(
            kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none, pageNo));
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
            const auto parsed = parseRandomGenId(makePersistentSampleId(
                kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none, pageNo, genId));
            ASSERT_OK(parsed.getStatus());
            ASSERT_EQ(parsed.getValue(), genId);
        }
    }
    ASSERT_EQ(parseRandomGenId(expectedId("random", "384_145_007")).getValue(), 145u);
}

TEST(ParsePersistentSampleGenId, ParsesTheGenIdOfAChunkSample) {
    const auto parsed = parsePersistentSampleGenId(expectedId("chunk", "384_10_3_000"),
                                                   kUuid,
                                                   SamplingTechniqueEnum::kChunk,
                                                   384,
                                                   /*numChunks=*/10);
    ASSERT_OK(parsed.getStatus());
    ASSERT_EQ(parsed.getValue(), 3u);

    const auto withoutGenId = parsePersistentSampleGenId(expectedId("chunk", "384_10_000"),
                                                         kUuid,
                                                         SamplingTechniqueEnum::kChunk,
                                                         384,
                                                         /*numChunks=*/10);
    ASSERT_OK(withoutGenId.getStatus());
    ASSERT_EQ(withoutGenId.getValue(), kDefaultPersistentSampleGenId);
}

TEST(ParsePersistentSampleGenId, RejectsIdsOfOtherSamples) {
    for (const auto& id :
         {makePersistentSampleId(UUID::gen(), SamplingTechniqueEnum::kRandom, 384, boost::none),
          makePersistentSampleId(kUuid, SamplingTechniqueEnum::kSeqScan, 384, boost::none),
          makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 385, boost::none),
          makePersistentSampleId(kUuid, SamplingTechniqueEnum::kRandom, 38, boost::none)}) {
        ASSERT_EQ(parseRandomGenId(id).getStatus().code(), ErrorCodes::UnsupportedFormat) << id;
    }
}

TEST(ParsePersistentSampleGenId, RejectsMalformedGenIdsAndPageNos) {
    for (std::string_view genIdAndPageNo : {"",
                                            "07",
                                            "0007",
                                            "_000",
                                            "01_000",
                                            "-1_000",
                                            "1a_000",
                                            "1_00a",
                                            "1_2_000",
                                            // 2^64: one past the max uint64_t, so it overflows.
                                            "18446744073709551616_000"}) {
        const auto id = expectedId("random", std::string{"384_"} + std::string{genIdAndPageNo});
        ASSERT_EQ(parseRandomGenId(id).getStatus().code(), ErrorCodes::UnsupportedFormat) << id;
    }
}

// ── selectHighestCompleteGeneration ───────────────────────────────────────────────────────────

StatusWith<std::vector<BSONObj>> select(std::vector<BSONObj> pages) {
    return selectHighestCompleteGeneration(
        std::move(pages), kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none);
}

void assertSelectedGeneration(const StatusWith<std::vector<BSONObj>>& selected,
                              uint64_t genId,
                              int numPages) {
    ASSERT_OK(selected.getStatus());
    ASSERT_EQ(selected.getValue().size(), static_cast<size_t>(numPages));
    for (int pageNo = 0; pageNo < numPages; ++pageNo) {
        const BSONObj& page = selected.getValue()[pageNo];
        ASSERT_EQ(page[PersistentSampleDoc::kPageNoFieldName].Int(), pageNo);
        const auto parsedGenId = parsePersistentSampleGenId(
            page["_id"].String(), kUuid, SamplingTechniqueEnum::kRandom, 384, boost::none);
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
    ASSERT_EQ(select({buildPage(kUuid, {.pageNo = 0})}).getStatus().code(),
              ErrorCodes::UnsupportedFormat);
    ASSERT_EQ(select({buildPage(kUuid, {.pageNo = 0, .numPages = 0})}).getStatus().code(),
              ErrorCodes::UnsupportedFormat);
    ASSERT_EQ(select({buildPage(kUuid, {.pageNo = 0, .numPages = -1})}).getStatus().code(),
              ErrorCodes::UnsupportedFormat);
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
    b.append("_id",
             makePersistentSampleId(
                 uuid, SamplingTechniqueEnum::kRandom, kStubSampleDocs.size(), boost::none));
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

// ── reassemblePersistentSample ────────────────────────────────────────────────────────────────

TEST(ReassemblePersistentSample, EmptyPagesReturnsNoSuchKey) {
    auto result = reassemblePersistentSample({});
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::NoSuchKey);
}

TEST(ReassemblePersistentSample, SinglePageRoundTrips) {
    const UUID uuid = UUID::gen();
    const std::vector<BSONObj> docs{BSON("_id" << 1), BSON("_id" << 2)};
    auto result = reassemblePersistentSample(
        {buildPage(uuid, {.requestedSampleSize = docs.size(), .docs = docs})});
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
    const size_t requestedSampleSize = 4;

    // selectHighestCompleteGeneration hands over pages already ordered by pageNo, so reassembly
    // relies on that ordering rather than sorting.
    auto result = reassemblePersistentSample({
        buildPage(uuid,
                  {.requestedSampleSize = requestedSampleSize, .pageNo = 0, .docs = {d0, d1}}),
        buildPage(uuid,
                  {.requestedSampleSize = requestedSampleSize, .pageNo = 1, .docs = {d2, d3}}),
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
    // reassembly requires pages in pageNo order (guaranteed by selectHighestCompleteGeneration); an
    // out-of-order sequence is treated as an incorrectly written sample.
    auto result = reassemblePersistentSample({
        buildPage(uuid, {.requestedSampleSize = 2, .pageNo = 1, .docs = {BSON("a" << 1)}}),
        buildPage(uuid, {.requestedSampleSize = 2, .pageNo = 0, .docs = {BSON("a" << 0)}}),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, MissingMiddlePageRejected) {
    const UUID uuid = UUID::gen();
    // pages 0 and 2 present, 1 missing
    auto result = reassemblePersistentSample({
        buildPage(uuid, {.requestedSampleSize = 3, .pageNo = 0, .docs = {BSON("a" << 0)}}),
        buildPage(uuid, {.requestedSampleSize = 3, .pageNo = 2, .docs = {BSON("a" << 2)}}),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, FirstPageNotZeroRejected) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample({
        buildPage(uuid, {.requestedSampleSize = 2, .pageNo = 1, .docs = {BSON("a" << 1)}}),
        buildPage(uuid, {.requestedSampleSize = 2, .pageNo = 2, .docs = {BSON("a" << 2)}}),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, DuplicatePageNoRejected) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample({
        buildPage(uuid, {.requestedSampleSize = 2, .pageNo = 0, .docs = {BSON("a" << 0)}}),
        buildPage(uuid, {.requestedSampleSize = 2, .pageNo = 0, .docs = {BSON("a" << 1)}}),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, IdentityMismatchAcrossPagesRejected) {
    const UUID uuid = UUID::gen();
    // page 0 declares requestedSampleSize 4, page 1 declares 5 — inconsistent identity.
    auto result = reassemblePersistentSample({
        buildPage(uuid, {.requestedSampleSize = 4, .pageNo = 0, .docs = {BSON("a" << 0)}}),
        buildPage(uuid, {.requestedSampleSize = 5, .pageNo = 1, .docs = {BSON("a" << 1)}}),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, ReassembledDocsExceedingRequestedSampleSizeRejected) {
    const UUID uuid = UUID::gen();
    // Each page is individually within requestedSampleSize=2, but combined they hold 3 docs.
    auto result = reassemblePersistentSample({
        buildPage(
            uuid,
            {.requestedSampleSize = 2, .pageNo = 0, .docs = {BSON("a" << 0), BSON("a" << 1)}}),
        buildPage(uuid, {.requestedSampleSize = 2, .pageNo = 1, .docs = {BSON("a" << 2)}}),
    });
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, MalformedPagePropagatesError) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample({
        buildPage(uuid, {.requestedSampleSize = 2, .docs = {BSON("a" << 0)}}),
        BSONObj{},  // empty/malformed page.
    });
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ReassemblePersistentSample, PreservesChunkNumChunks) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample({
        buildPage(uuid,
                  {.method = SamplingTechniqueEnum::kChunk,
                   .numChunks = 1,
                   .pageNo = 0,
                   .docs = {BSON("a" << 0)}}),
        buildPage(uuid,
                  {.method = SamplingTechniqueEnum::kChunk,
                   .numChunks = 1,
                   .pageNo = 1,
                   .docs = {BSON("a" << 1)}}),
    });
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
        {buildPage(uuid, {.pageNo = 0, .numPages = 2, .docs = {BSON("a" << 0)}}),
         buildPage(uuid, {.pageNo = 1, .numPages = 2, .docs = {BSON("a" << 1)}})});
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(result.getValue().getDocs().size(), 2u);
}

TEST(ReassemblePersistentSample, FewerPagesThanNumPagesRejected) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample({buildPage(uuid, {.pageNo = 0, .numPages = 3}),
                                              buildPage(uuid, {.pageNo = 1, .numPages = 3})});
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, MorePagesThanNumPagesRejected) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample({buildPage(uuid, {.pageNo = 0, .numPages = 1}),
                                              buildPage(uuid, {.pageNo = 1, .numPages = 1})});
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

TEST(ReassemblePersistentSample, MissingNumPagesIsNotChecked) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample(
        {buildPage(uuid, {.requestedSampleSize = 2, .pageNo = 0, .docs = {BSON("a" << 0)}}),
         buildPage(uuid, {.requestedSampleSize = 2, .pageNo = 1, .docs = {BSON("a" << 1)}})});
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(result.getValue().getDocs().size(), 2u);
}

TEST(ReassemblePersistentSample, PagesDisagreeingOnNumPagesRejected) {
    const UUID uuid = UUID::gen();
    auto result = reassemblePersistentSample({buildPage(uuid, {.pageNo = 0, .numPages = 2}),
                                              buildPage(uuid, {.pageNo = 1, .numPages = 3})});
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::UnsupportedFormat);
}

// ── PersistentSampleLoader ────────────────────────────────────────────────────────────────────

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

TEST_F(SamplingEstimatorTest, PersistentLoaderIgnoresSamplesOfOtherSizes) {
    unittest::ServerParameterGuard persistentStatsFlag{"featureFlagPersistentStats", true};
    insertDocuments(_kTestNss, {BSON("_id" << 1 << "tag" << "live")});
    const UUID uuid = [&] {
        auto srcColl = acquireCollectionForSampling(operationContext(), _kTestNss);
        return srcColl.getCollectionPtr()->uuid();
    }();
    createCollAndInsertDocuments(
        operationContext(),
        NamespaceStringUtil::deserialize(_kTestNss.dbName(), kSamplesCollectionName),
        {buildPage(uuid, {.requestedSampleSize = 10, .genId = 0, .docs = {BSON("_id" << 10)}})},
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
