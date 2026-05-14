/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/ce/sampling/persistent_sample_loader.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <string>

namespace mongo::ce {
namespace {

const BSONObj kStubSampleDoc = BSON("_id" << 1);
const std::vector<BSONObj> kStubSampleDocs{kStubSampleDoc};

// ── buildPersistentSampleId ───────────────────────────────────────────────────────────────────

TEST(BuildPersistentSampleId, RandomMethodFormat) {
    const UUID uuid = UUID::gen();
    const std::string id =
        buildPersistentSampleId(uuid, SamplingCEMethodEnum::kRandom, 1000, boost::none);
    ASSERT_EQUALS(id, uuid.toString() + "_random_1000_v1");
}

TEST(BuildPersistentSampleId, ChunkMethodFormat) {
    const UUID uuid = UUID::gen();
    const std::string id =
        buildPersistentSampleId(uuid, SamplingCEMethodEnum::kChunk, 384, /*numChunks=*/10);
    ASSERT_EQUALS(id, uuid.toString() + "_chunk10_384_v1");
}

TEST(BuildPersistentSampleId, SchemaVersionIsEmbedded) {
    // The suffix must be `_v<kPersistentSampleSchemaVersion>` so readers and writers always
    // agree on the key even when the schema version constant is bumped in the future.
    const UUID uuid = UUID::gen();
    const std::string id =
        buildPersistentSampleId(uuid, SamplingCEMethodEnum::kRandom, 384, boost::none);
    const std::string expectedSuffix = "_v" + std::to_string(kPersistentSampleSchemaVersion);
    ASSERT_TRUE(id.ends_with(expectedSuffix))
        << "id='" << id << "' expected suffix '" << expectedSuffix << "'";
}

TEST(BuildPersistentSampleId, DifferentUUIDsProduceDifferentKeys) {
    const std::string a =
        buildPersistentSampleId(UUID::gen(), SamplingCEMethodEnum::kRandom, 384, boost::none);
    const std::string b =
        buildPersistentSampleId(UUID::gen(), SamplingCEMethodEnum::kRandom, 384, boost::none);
    ASSERT_NOT_EQUALS(a, b);
}

TEST(BuildPersistentSampleId, DifferentConfigurationsProduceDifferentKeys) {
    const UUID uuid = UUID::gen();
    const std::string randomKey =
        buildPersistentSampleId(uuid, SamplingCEMethodEnum::kRandom, 384, boost::none);
    const std::string chunkKey =
        buildPersistentSampleId(uuid, SamplingCEMethodEnum::kChunk, 384, /*numChunks=*/10);
    const std::string differentSizeKey =
        buildPersistentSampleId(uuid, SamplingCEMethodEnum::kRandom, 1000, boost::none);

    ASSERT_NOT_EQUALS(randomKey, chunkKey);
    ASSERT_NOT_EQUALS(randomKey, differentSizeKey);
    ASSERT_NOT_EQUALS(chunkKey, differentSizeKey);
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
        buildPersistentSampleDoc(uuid, SamplingCEMethodEnum::kRandom, docs.size(), docs);
    auto result = parsePersistentSample(sampleDoc);
    ASSERT_OK(result.getStatus());
    const auto& sample = result.getValue();

    ASSERT_EQUALS(sample.getCollectionUuid(), uuid.toString());
    ASSERT_EQUALS(sample.getSamplingMethod(), SamplingCEMethodEnum::kRandom);
    ASSERT_EQUALS(static_cast<size_t>(sample.getSampleSize()), docs.size());
    ASSERT_FALSE(sample.getNumChunks().has_value());

    ASSERT_EQUALS(sample.getCreatedAt(),
                  sampleDoc[PersistentSampleDoc::kCreatedAtFieldName].date());
    ASSERT_EQUALS(sample.getDocs().size(), docs.size());
    ASSERT_BSONOBJ_EQ(sample.getDocs()[0], docs[0]);
    ASSERT_BSONOBJ_EQ(sample.getDocs()[1], docs[1]);
}

TEST(ParsePersistentSample, ChunkSamplePopulatesNumChunks) {
    const std::vector<BSONObj> docs{BSON("x" << 1), BSON("x" << 2)};
    auto result = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(), SamplingCEMethodEnum::kChunk, docs.size(), docs, /*numChunks=*/1));
    ASSERT_OK(result.getStatus());
    const auto& sample = result.getValue();
    ASSERT_EQUALS(sample.getSamplingMethod(), SamplingCEMethodEnum::kChunk);
    ASSERT_TRUE(sample.getNumChunks().has_value());
    ASSERT_EQUALS(sample.getNumChunks().value(), 1);
}

TEST(ParsePersistentSample, RejectsDocsArrayLargerThanSampleSize) {
    // 'docs' must not contain more entries than 'sampleSize'. Fewer is allowed: chunk sampling
    // may collect less than the requested size when a chunk starts near end-of-collection.
    const std::vector<BSONObj> docs{BSON("a" << 1), BSON("a" << 2)};

    // docs.size() > sampleSize — rejected.
    auto docsExceedDeclared = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(), SamplingCEMethodEnum::kRandom, /*sampleSize=*/docs.size() - 1, docs));
    ASSERT_NOT_OK(docsExceedDeclared.getStatus());
    ASSERT_EQUALS(docsExceedDeclared.getStatus().code(), ErrorCodes::UnsupportedFormat);

    // docs.size() < sampleSize — allowed (underfull chunk sample).
    auto underFull = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(), SamplingCEMethodEnum::kRandom, /*sampleSize=*/docs.size() + 1, docs));
    ASSERT_OK(underFull.getStatus());
}

TEST(ParsePersistentSample, RejectsEmptySample) {
    auto result = parsePersistentSample(buildPersistentSampleDoc(
        UUID::gen(), SamplingCEMethodEnum::kRandom, /*sampleSize=*/0, /*docs=*/{}));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsMissingSchemaVersion) {
    BSONObjBuilder b;
    b.append(PersistentSampleDoc::kCollectionUuidFieldName, UUID::gen().toString());
    b.append(PersistentSampleDoc::kSampleSizeFieldName,
             static_cast<long long>(kStubSampleDocs.size()));
    b.append(PersistentSampleDoc::kSamplingMethodFieldName,
             idlSerialize(SamplingCEMethodEnum::kRandom));
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
                                                       SamplingCEMethodEnum::kRandom,
                                                       /*sampleSize=*/kStubSampleDocs.size(),
                                                       /*docs=*/kStubSampleDocs,
                                                       /*numChunks=*/boost::none,
                                                       /*schemaVersion=*/0));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNonNumericSchemaVersion) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingCEMethodEnum::kRandom,
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
        SamplingCEMethodEnum::kRandom,
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
                                 SamplingCEMethodEnum::kRandom,
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
                                 SamplingCEMethodEnum::kRandom,
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
                                 SamplingCEMethodEnum::kRandom,
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
                                                       SamplingCEMethodEnum::kChunk,
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
                                                       SamplingCEMethodEnum::kChunk,
                                                       /*sampleSize=*/kStubSampleDocs.size(),
                                                       /*docs=*/kStubSampleDocs,
                                                       /*numChunks=*/0));
    ASSERT_NOT_OK(zeroResult.getStatus());

    auto negativeResult =
        parsePersistentSample(buildPersistentSampleDoc(UUID::gen(),
                                                       SamplingCEMethodEnum::kChunk,
                                                       /*sampleSize=*/kStubSampleDocs.size(),
                                                       /*docs=*/kStubSampleDocs,
                                                       /*numChunks=*/-1));
    ASSERT_NOT_OK(negativeResult.getStatus());
}

TEST(ParsePersistentSample, RejectsNonDateCreatedAt) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingCEMethodEnum::kRandom,
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
             buildPersistentSampleId(
                 uuid, SamplingCEMethodEnum::kRandom, kStubSampleDocs.size(), boost::none));
    b.append(PersistentSampleDoc::kCollectionUuidFieldName, uuid.toString());
    b.append(PersistentSampleDoc::kSchemaVersionFieldName, kPersistentSampleSchemaVersion);
    b.appendDate(PersistentSampleDoc::kCreatedAtFieldName, Date_t::now());
    b.append(PersistentSampleDoc::kSampleSizeFieldName,
             static_cast<long long>(kStubSampleDocs.size()));
    b.append(PersistentSampleDoc::kSamplingMethodFieldName,
             idlSerialize(SamplingCEMethodEnum::kRandom));
    auto result = parsePersistentSample(b.obj());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ParsePersistentSample, RejectsNonArrayDocsField) {
    auto result = parsePersistentSample(
        buildPersistentSampleDoc(UUID::gen(),
                                 SamplingCEMethodEnum::kRandom,
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
                                 SamplingCEMethodEnum::kRandom,
                                 /*sampleSize=*/kStubSampleDocs.size(),
                                 /*docs=*/kStubSampleDocs,
                                 /*numChunks=*/boost::none,
                                 /*schemaVersion=*/kPersistentSampleSchemaVersion,
                                 BSON(PersistentSampleDoc::kDocsFieldName << arr.arr())));
    ASSERT_NOT_OK(result.getStatus());
}

}  // namespace
}  // namespace mongo::ce
