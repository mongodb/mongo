
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/pipeline/resume_token.h"

#include <algorithm>
#include <boost/optional/optional_io.hpp>
#include <random>

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

using Format = ResumeToken::SerializationFormat;

TEST(ResumeToken, EncodesFullTokenFromData) {
    Timestamp ts(1000, 2);
    UUID testUuid = UUID::gen();
    Document documentKey{{"_id"_sd, "stuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 2}}}};

    ResumeTokenData resumeTokenDataIn(ts, 0, 0, testUuid, Value(documentKey));
    ResumeToken token(resumeTokenDataIn);
    ResumeTokenData tokenData = token.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);
}

TEST(ResumeToken, EncodesTimestampOnlyTokenFromData) {
    Timestamp ts(1001, 3);

    ResumeTokenData resumeTokenDataIn;
    resumeTokenDataIn.clusterTime = ts;
    ResumeToken token(resumeTokenDataIn);
    ResumeTokenData tokenData = token.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);
}

TEST(ResumeToken, ShouldRoundTripThroughHexStringEncoding) {
    Timestamp ts(1000, 2);
    UUID testUuid = UUID::gen();
    Document documentKey{{"_id"_sd, "stuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 2}}}};

    for (int version = 0; version <= 1; ++version) {
        ResumeTokenData resumeTokenDataIn(ts, version, 0, testUuid, Value(documentKey));

        // Test serialization/parsing through Document.
        auto rtToken =
            ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString));
        ResumeTokenData tokenData = rtToken.getData();
        ASSERT_EQ(resumeTokenDataIn, tokenData);

        // Test serialization/parsing through BSON.
        rtToken = ResumeToken::parse(
            ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());
        tokenData = rtToken.getData();
        ASSERT_EQ(resumeTokenDataIn, tokenData);
    }
}

TEST(ResumeToken, ShouldRoundTripThroughBinDataEncoding) {
    Timestamp ts(1000, 2);
    UUID testUuid = UUID::gen();
    Document documentKey{{"_id"_sd, "stuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 2}}}};

    // BinData format does not have a 'version' field, so we default to 1.
    ResumeTokenData resumeTokenDataIn(ts, 1, 0, testUuid, Value(documentKey));

    // Test serialization/parsing through Document.
    auto rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kBinData).toBson());
    ResumeTokenData tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);

    // Test serialization/parsing through BSON.
    rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kBinData).toBson());
    tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);
}

TEST(ResumeToken, TimestampOnlyTokenShouldRoundTripThroughHexStringEncoding) {
    Timestamp ts(1001, 3);

    ResumeTokenData resumeTokenDataIn;
    resumeTokenDataIn.clusterTime = ts;

    // Test serialization/parsing through Document.
    auto rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());
    ResumeTokenData tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);

    // Test serialization/parsing through BSON.
    rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());
    tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);
}

TEST(ResumeToken, TimestampOnlyTokenShouldRoundTripThroughBinDataEncoding) {
    Timestamp ts(1001, 3);

    ResumeTokenData resumeTokenDataIn;
    resumeTokenDataIn.clusterTime = ts;

    // Test serialization/parsing through Document.
    auto rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kBinData).toBson());
    ResumeTokenData tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);

    // Test serialization/parsing through BSON.
    rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kBinData).toBson());
    tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);
}

TEST(ResumeToken, TestMissingTypebitsOptimization) {
    Timestamp ts(1000, 1);
    UUID testUuid = UUID::gen();

    ResumeTokenData hasTypeBitsData(ts, 0, 0, testUuid, Value(Document{{"_id", 1.0}}));
    ResumeTokenData noTypeBitsData(
        ResumeTokenData(ts, 0, 0, testUuid, Value(Document{{"_id", 1}})));
    ResumeToken hasTypeBitsToken(hasTypeBitsData);
    ResumeToken noTypeBitsToken(noTypeBitsData);
    ASSERT_EQ(noTypeBitsToken, hasTypeBitsToken);
    auto hasTypeBitsDoc = hasTypeBitsToken.toDocument(Format::kHexString);
    auto noTypeBitsDoc = noTypeBitsToken.toDocument(Format::kHexString);
    ASSERT_FALSE(hasTypeBitsDoc["_typeBits"].missing());
    ASSERT_TRUE(noTypeBitsDoc["_typeBits"].missing()) << noTypeBitsDoc["_typeBits"];
    auto rtHasTypeBitsData = ResumeToken::parse(hasTypeBitsDoc).getData();
    auto rtNoTypeBitsData = ResumeToken::parse(noTypeBitsDoc).getData();
    ASSERT_EQ(hasTypeBitsData, rtHasTypeBitsData);
    ASSERT_EQ(noTypeBitsData, rtNoTypeBitsData);
    ASSERT_EQ(BSONType::NumberDouble, rtHasTypeBitsData.documentKey["_id"].getType());
    ASSERT_EQ(BSONType::NumberInt, rtNoTypeBitsData.documentKey["_id"].getType());
}

TEST(ResumeToken, FailsToParseForInvalidTokenFormats) {
    // Missing document.
    ASSERT_THROWS(ResumeToken::parse(Document()), AssertionException);
    // Missing data field.
    ASSERT_THROWS(ResumeToken::parse(Document{{"somefield"_sd, "stuff"_sd}}), AssertionException);
    // Wrong type data field
    ASSERT_THROWS(ResumeToken::parse(Document{{"_data"_sd, BSONNULL}}), AssertionException);
    ASSERT_THROWS(ResumeToken::parse(Document{{"_data"_sd, 0}}), AssertionException);

    // Valid data field, but wrong type typeBits.
    Timestamp ts(1010, 4);
    ResumeTokenData tokenData;
    tokenData.clusterTime = ts;
    auto goodTokenDocBinData = ResumeToken(tokenData).toDocument(Format::kBinData);
    auto goodData = goodTokenDocBinData["_data"].getBinData();
    ASSERT_THROWS(ResumeToken::parse(Document{{"_data"_sd, goodData}, {"_typeBits", "string"_sd}}),
                  AssertionException);
    auto goodTokenDocString = ResumeToken(tokenData).toDocument(Format::kHexString);
    auto goodString = goodTokenDocString["_data"].getString();
    ASSERT_THROWS(
        ResumeToken::parse(Document{{"_data"_sd, goodString}, {"_typeBits", "string"_sd}}),
        AssertionException);

    // Valid data except wrong bindata type.
    ASSERT_THROWS(ResumeToken::parse(
                      Document{{"_data"_sd, BSONBinData(goodData.data, goodData.length, newUUID)}}),
                  AssertionException);
    // Valid data, wrong typeBits bindata type.
    ASSERT_THROWS(
        ResumeToken::parse(Document{{"_data"_sd, goodData},
                                    {"_typeBits", BSONBinData(goodData.data, 0, newUUID)}}),
        AssertionException);
}

TEST(ResumeToken, FailsToDecodeInvalidKeyStringBinData) {
    Timestamp ts(1010, 4);
    ResumeTokenData tokenData;
    tokenData.clusterTime = ts;
    auto goodTokenDocBinData = ResumeToken(tokenData).toDocument(Format::kBinData);
    auto goodData = goodTokenDocBinData["_data"].getBinData();
    const unsigned char zeroes[] = {0, 0, 0, 0, 0};
    const unsigned char nonsense[] = {165, 85, 77, 86, 255};

    // Data of correct type, but empty.  This won't fail until we try to decode the data.
    auto emptyToken =
        ResumeToken::parse(Document{{"_data"_sd, BSONBinData(zeroes, 0, BinDataGeneral)}});
    ASSERT_THROWS_CODE(emptyToken.getData(), AssertionException, 40649);

    auto incorrectType = ResumeToken::parse(Document{{"_data"_sd, "string"_sd}});
    ASSERT_THROWS_CODE(incorrectType.getData(), AssertionException, ErrorCodes::FailedToParse);

    // Data of correct type with a bunch of zeros.
    auto zeroesToken = ResumeToken::parse(
        Document{{"_data"_sd, BSONBinData(zeroes, sizeof(zeroes), BinDataGeneral)}});
    ASSERT_THROWS_CODE(zeroesToken.getData(), AssertionException, 50811);
    zeroesToken = ResumeToken::parse(Document{{"_data"_sd, "00000"_sd}});
    ASSERT_THROWS_CODE(zeroesToken.getData(), AssertionException, ErrorCodes::FailedToParse);

    // Data of correct type with a bunch of nonsense.
    auto nonsenseToken = ResumeToken::parse(
        Document{{"_data"_sd, BSONBinData(nonsense, sizeof(nonsense), BinDataGeneral)}});
    ASSERT_THROWS_CODE(nonsenseToken.getData(), AssertionException, 50811);
    nonsenseToken = ResumeToken::parse(Document{{"_data"_sd, "nonsense"_sd}});
    ASSERT_THROWS_CODE(nonsenseToken.getData(), AssertionException, ErrorCodes::FailedToParse);

    // Valid data, bad typeBits; note that an all-zeros typebits is valid so it is not tested here.
    auto badTypeBitsToken = ResumeToken::parse(
        Document{{"_data"_sd, goodData},
                 {"_typeBits", BSONBinData(nonsense, sizeof(nonsense), BinDataGeneral)}});
    ASSERT_THROWS_CODE(badTypeBitsToken.getData(), AssertionException, ErrorCodes::Overflow);

    const unsigned char invalidString[] = {
        60,  // CType::kStringLike
        55,  // Non-null terminated
    };
    auto invalidStringToken = ResumeToken::parse(
        Document{{"_data"_sd, BSONBinData(invalidString, sizeof(invalidString), BinDataGeneral)}});
    ASSERT_THROWS_WITH_CHECK(
        invalidStringToken.getData(), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), 50816);
            ASSERT_STRING_CONTAINS(exception.reason(), "Failed to find null terminator in string");
        });
}

TEST(ResumeToken, WrongVersionToken) {
    Timestamp ts(1001, 3);

    ResumeTokenData resumeTokenDataIn;
    resumeTokenDataIn.clusterTime = ts;
    resumeTokenDataIn.version = 0;
    resumeTokenDataIn.fromInvalidate = ResumeTokenData::FromInvalidate::kFromInvalidate;

    // This one with version 0 should succeed. Version 0 cannot encode the fromInvalidate bool, so
    // we expect it to be set to the default 'kNotFromInvalidate' after serialization.
    auto rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());
    ResumeTokenData tokenData = rtToken.getData();
    ASSERT_NE(resumeTokenDataIn, tokenData);
    tokenData.fromInvalidate = ResumeTokenData::FromInvalidate::kFromInvalidate;
    ASSERT_EQ(resumeTokenDataIn, tokenData);

    // Version 1 should include the 'fromInvalidate' bool through serialization.
    resumeTokenDataIn.version = 1;
    rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());
    tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);

    // With version 2 it should fail - the maximum supported version is 1.
    resumeTokenDataIn.version = 2;
    rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());
    ASSERT_THROWS(rtToken.getData(), AssertionException);

    // For version 0, the 'tokenType' field is not encoded. We expect it to default from the value
    // 'kHighWaterMarkToken' back to 'kEventToken' after serialization.
    resumeTokenDataIn = {};
    resumeTokenDataIn.version = 0;
    resumeTokenDataIn.tokenType = ResumeTokenData::kHighWaterMarkToken;

    rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());
    tokenData = rtToken.getData();
    ASSERT_NE(resumeTokenDataIn, tokenData);
    tokenData.tokenType = ResumeTokenData::kHighWaterMarkToken;
    ASSERT_EQ(resumeTokenDataIn, tokenData);

    // Version 1 should include the 'tokenType' field through serialization.
    resumeTokenDataIn.version = 1;
    rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());
    tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);

    // A non-TokenType value in the 'tokenType' field should fail to decode.
    resumeTokenDataIn.tokenType = static_cast<ResumeTokenData::TokenType>(5);
    rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());
    ASSERT_THROWS(rtToken.getData(), AssertionException);

    // With version 2 it should fail - the maximum supported version is 1.
    resumeTokenDataIn.version = 2;
    rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());
    ASSERT_THROWS(rtToken.getData(), AssertionException);
}

TEST(ResumeToken, InvalidApplyOpsIndex) {
    Timestamp ts(1001, 3);

    ResumeTokenData resumeTokenDataIn;
    resumeTokenDataIn.clusterTime = ts;
    resumeTokenDataIn.applyOpsIndex = 1234;

    // Should round trip with a non-negative applyOpsIndex.
    auto rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());
    ResumeTokenData tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);

    // Should fail with a negative applyOpsIndex.
    resumeTokenDataIn.applyOpsIndex = std::numeric_limits<size_t>::max();
    rtToken =
        ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument(Format::kHexString).toBson());

    ASSERT_THROWS(rtToken.getData(), AssertionException);
}

TEST(ResumeToken, StringEncodingSortsCorrectly) {
    // Make sure that the string encoding of the resume tokens will compare in the correct order,
    // namely timestamp, version, applyOpsIndex, uuid, then documentKey.
    Timestamp ts2_2(2, 2);
    Timestamp ts10_4(10, 4);
    Timestamp ts10_5(10, 5);
    Timestamp ts11_3(11, 3);

    // Generate two different UUIDs, and figure out which one is smaller. Store the smaller one in
    // 'lower_uuid'.
    UUID lower_uuid = UUID::gen();
    UUID higher_uuid = UUID::gen();
    if (lower_uuid > higher_uuid) {
        std::swap(lower_uuid, higher_uuid);
    }

    auto assertLt = [](const ResumeTokenData& lower, const ResumeTokenData& higher) {
        auto lowerString = ResumeToken(lower).toDocument(Format::kHexString)["_data"].getString();
        auto higherString = ResumeToken(higher).toDocument(Format::kHexString)["_data"].getString();
        ASSERT_LT(lowerString, higherString);
    };

    // Test using only Timestamps.
    assertLt({ts2_2, 0, 0, boost::none, Value()}, {ts10_4, 0, 0, boost::none, Value()});
    assertLt({ts2_2, 0, 0, boost::none, Value()}, {ts10_5, 0, 0, boost::none, Value()});
    assertLt({ts2_2, 0, 0, boost::none, Value()}, {ts11_3, 0, 0, boost::none, Value()});
    assertLt({ts10_4, 0, 0, boost::none, Value()}, {ts10_5, 0, 0, boost::none, Value()});
    assertLt({ts10_4, 0, 0, boost::none, Value()}, {ts11_3, 0, 0, boost::none, Value()});
    assertLt({ts10_5, 0, 0, boost::none, Value()}, {ts11_3, 0, 0, boost::none, Value()});

    // Test using Timestamps and version.
    assertLt({ts2_2, 0, 0, boost::none, Value()}, {ts2_2, 1, 0, boost::none, Value()});
    assertLt({ts10_4, 5, 0, boost::none, Value()}, {ts10_4, 10, 0, boost::none, Value()});

    // Test that the Timestamp is more important than the version, applyOpsIndex, UUID and
    // documentKey.
    assertLt({ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", 0}})},
             {ts10_5, 0, 0, lower_uuid, Value(Document{{"_id", 0}})});
    assertLt({ts2_2, 0, 0, lower_uuid, Value(Document{{"_id", 0}})},
             {ts10_5, 0, 0, lower_uuid, Value(Document{{"_id", 0}})});
    assertLt({ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", 1}})},
             {ts10_5, 0, 0, lower_uuid, Value(Document{{"_id", 0}})});
    assertLt({ts10_4, 0, 0, higher_uuid, Value(Document{{"_id", 0}})},
             {ts10_5, 0, 0, lower_uuid, Value(Document{{"_id", 0}})});
    assertLt({ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", 0}})},
             {ts10_5, 0, 0, higher_uuid, Value(Document{{"_id", 0}})});

    // Test that when the Timestamp is the same, the version breaks the tie.
    assertLt({ts10_4, 1, 50, lower_uuid, Value(Document{{"_id", 0}})},
             {ts10_4, 5, 1, lower_uuid, Value(Document{{"_id", 0}})});
    assertLt({ts2_2, 1, 0, higher_uuid, Value(Document{{"_id", 0}})},
             {ts2_2, 2, 0, lower_uuid, Value(Document{{"_id", 0}})});
    assertLt({ts10_4, 1, 0, lower_uuid, Value(Document{{"_id", 1}})},
             {ts10_4, 2, 0, lower_uuid, Value(Document{{"_id", 0}})});

    // Test that when the Timestamp and version are the same, the applyOpsIndex breaks the tie.
    assertLt({ts10_4, 1, 6, lower_uuid, Value(Document{{"_id", 0}})},
             {ts10_4, 1, 50, lower_uuid, Value(Document{{"_id", 0}})});
    assertLt({ts2_2, 0, 0, higher_uuid, Value(Document{{"_id", 0}})},
             {ts2_2, 0, 4, lower_uuid, Value(Document{{"_id", 0}})});

    // Test that when the Timestamp, version, and applyOpsIndex are the same, the UUID breaks the
    // tie.
    assertLt({ts2_2, 0, 0, lower_uuid, Value(Document{{"_id", 0}})},
             {ts2_2, 0, 0, higher_uuid, Value(Document{{"_id", 0}})});
    assertLt({ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", 0}})},
             {ts10_4, 0, 0, higher_uuid, Value(Document{{"_id", 0}})});
    assertLt({ts10_4, 1, 2, lower_uuid, Value(Document{{"_id", 0}})},
             {ts10_4, 1, 2, higher_uuid, Value(Document{{"_id", 0}})});
    assertLt({ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", 1}})},
             {ts10_4, 0, 0, higher_uuid, Value(Document{{"_id", 0}})});
    assertLt({ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", 1}})},
             {ts10_4, 0, 0, higher_uuid, Value(Document{{"_id", 2}})});

    // Test that when the Timestamp, version, applyOpsIndex, and UUID are the same, the documentKey
    // breaks the tie.
    assertLt({ts2_2, 0, 0, lower_uuid, Value(Document{{"_id", 0}})},
             {ts2_2, 0, 0, lower_uuid, Value(Document{{"_id", 1}})});
    assertLt({ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", 0}})},
             {ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", 1}})});
    assertLt({ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", 1}})},
             {ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", "string"_sd}})});
    assertLt({ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", BSONNULL}})},
             {ts10_4, 0, 0, lower_uuid, Value(Document{{"_id", 0}})});
}

}  // namspace
}  // namspace mongo
