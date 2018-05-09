/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/pipeline/resume_token.h"

#include <boost/optional/optional_io.hpp>
#include <random>

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

TEST(ResumeToken, EncodesFullTokenFromData) {
    Timestamp ts(1000, 2);
    UUID testUuid = UUID::gen();
    Document documentKey{{"_id"_sd, "stuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 2}}}};

    ResumeTokenData resumeTokenDataIn(ts, Value(documentKey), testUuid);
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

TEST(ResumeToken, RoundTripThroughBsonFullToken) {
    Timestamp ts(1000, 2);
    UUID testUuid = UUID::gen();
    Document documentKey{{"_id"_sd, "stuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 2}}}};

    ResumeTokenData resumeTokenDataIn(ts, Value(documentKey), testUuid);
    auto rtToken = ResumeToken::parse(ResumeToken(resumeTokenDataIn).toBSON());
    ResumeTokenData tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);
}

TEST(ResumeToken, RoundTripThroughBsonTimestampOnlyToken) {
    Timestamp ts(1001, 3);

    ResumeTokenData resumeTokenDataIn;
    resumeTokenDataIn.clusterTime = ts;
    auto rtToken = ResumeToken::parse(ResumeToken(resumeTokenDataIn).toBSON());
    ResumeTokenData tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);
}

TEST(ResumeToken, RoundTripThroughDocumentFullToken) {
    Timestamp ts(1000, 2);
    UUID testUuid = UUID::gen();
    Document documentKey{{"_id"_sd, "stuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 2}}}};

    ResumeTokenData resumeTokenDataIn(ts, Value(documentKey), testUuid);
    auto rtToken = ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument());
    ResumeTokenData tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);
}

TEST(ResumeToken, RoundTripThroughDocumentTimestampOnlyToken) {
    Timestamp ts(1001, 3);

    ResumeTokenData resumeTokenDataIn;
    resumeTokenDataIn.clusterTime = ts;
    auto rtToken = ResumeToken::parse(ResumeToken(resumeTokenDataIn).toDocument());
    ResumeTokenData tokenData = rtToken.getData();
    ASSERT_EQ(resumeTokenDataIn, tokenData);
}

TEST(ResumeToken, TestMissingTypebitsOptimization) {
    Timestamp ts(1000, 1);
    UUID testUuid = UUID::gen();

    ResumeTokenData hasTypeBitsData(ts, Value(Document{{"_id", 1.0}}), testUuid);
    ResumeTokenData noTypeBitsData(ResumeTokenData(ts, Value(Document{{"_id", 1}}), testUuid));
    ResumeToken hasTypeBitsToken(hasTypeBitsData);
    ResumeToken noTypeBitsToken(noTypeBitsData);
    ASSERT_EQ(noTypeBitsToken, hasTypeBitsToken);
    auto hasTypeBitsDoc = hasTypeBitsToken.toDocument();
    auto noTypeBitsDoc = noTypeBitsToken.toDocument();
    ASSERT_FALSE(hasTypeBitsDoc["_typeBits"].missing());
    ASSERT_TRUE(noTypeBitsDoc["_typeBits"].missing()) << noTypeBitsDoc["_typeBits"];
    auto rtHasTypeBitsData = ResumeToken::parse(hasTypeBitsDoc).getData();
    auto rtNoTypeBitsData = ResumeToken::parse(noTypeBitsDoc).getData();
    ASSERT_EQ(hasTypeBitsData, rtHasTypeBitsData);
    ASSERT_EQ(noTypeBitsData, rtNoTypeBitsData);
    ASSERT_EQ(BSONType::NumberDouble, rtHasTypeBitsData.documentKey["_id"].getType());
    ASSERT_EQ(BSONType::NumberInt, rtNoTypeBitsData.documentKey["_id"].getType());
}

// Tests comparison functions for tokens constructed from oplog data.
TEST(ResumeToken, CompareFromData) {
    Timestamp ts1(1000, 1);
    Timestamp ts2(1000, 2);
    Timestamp ts3(1001, 1);
    UUID testUuid = UUID::gen();
    UUID testUuid2 = UUID::gen();
    Document documentKey1a{{"_id"_sd, "stuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 2}}}};
    Document documentKey1b{{"_id"_sd, "stuff"_sd},
                           {"otherkey"_sd, Document{{"otherstuff"_sd, 2.0}}}};
    Document documentKey2{{"_id"_sd, "stuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 3}}}};
    Document documentKey3{{"_id"_sd, "ztuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 0}}}};

    ResumeToken token1a(ResumeTokenData(ts1, Value(documentKey1a), testUuid));
    ResumeToken token1b(ResumeTokenData(ts1, Value(documentKey1b), testUuid));

    // Equivalent types don't matter.
    ASSERT_EQ(token1a, token1b);
    ASSERT_LTE(token1a, token1b);
    ASSERT_GTE(token1a, token1b);

    // UUIDs matter, but all that really matters is they compare unequal.
    ResumeToken tokenOtherCollection(ResumeTokenData(ts1, Value(documentKey1a), testUuid2));
    ASSERT_NE(token1a, tokenOtherCollection);

    ResumeToken token2(ResumeTokenData(ts1, Value(documentKey2), testUuid));

    // Document keys matter.
    ASSERT_LT(token1a, token2);
    ASSERT_LTE(token1a, token2);
    ASSERT_GT(token2, token1a);
    ASSERT_GTE(token2, token1a);

    ResumeToken token3(ResumeTokenData(ts1, Value(documentKey3), testUuid));

    // Order within document keys matters.
    ASSERT_LT(token1a, token3);
    ASSERT_LTE(token1a, token3);
    ASSERT_GT(token3, token1a);
    ASSERT_GTE(token3, token1a);

    ASSERT_LT(token2, token3);
    ASSERT_LTE(token2, token3);
    ASSERT_GT(token3, token2);
    ASSERT_GTE(token3, token2);

    ResumeToken token4(ResumeTokenData(ts2, Value(documentKey1a), testUuid));

    // Timestamps matter.
    ASSERT_LT(token1a, token4);
    ASSERT_LTE(token1a, token4);
    ASSERT_GT(token4, token1a);
    ASSERT_GTE(token4, token1a);

    // Timestamps matter more than document key.
    ASSERT_LT(token3, token4);
    ASSERT_LTE(token3, token4);
    ASSERT_GT(token4, token3);
    ASSERT_GTE(token4, token3);

    ResumeToken token5(ResumeTokenData(ts3, Value(documentKey1a), testUuid));

    // Time matters more than increment in timestamp
    ASSERT_LT(token4, token5);
    ASSERT_LTE(token4, token5);
    ASSERT_GT(token5, token4);
    ASSERT_GTE(token5, token4);
}

// Tests comparison functions for tokens constructed from the keystring-encoded form.
TEST(ResumeToken, CompareFromEncodedData) {
    Timestamp ts1(1000, 1);
    Timestamp ts2(1000, 2);
    Timestamp ts3(1001, 1);
    UUID testUuid = UUID::gen();
    UUID testUuid2 = UUID::gen();
    Document documentKey1a{{"_id"_sd, "stuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 2}}}};
    Document documentKey1b{{"_id"_sd, "stuff"_sd},
                           {"otherkey"_sd, Document{{"otherstuff"_sd, 2.0}}}};
    Document documentKey2{{"_id"_sd, "stuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 3}}}};
    Document documentKey3{{"_id"_sd, "ztuff"_sd}, {"otherkey"_sd, Document{{"otherstuff"_sd, 0}}}};

    auto token1a = ResumeToken::parse(
        ResumeToken(ResumeTokenData(ts1, Value(documentKey1a), testUuid)).toDocument());
    auto token1b = ResumeToken::parse(
        ResumeToken(ResumeTokenData(ts1, Value(documentKey1b), testUuid)).toDocument());

    // Equivalent types don't matter.
    ASSERT_EQ(token1a, token1b);
    ASSERT_LTE(token1a, token1b);
    ASSERT_GTE(token1a, token1b);

    // UUIDs matter, but all that really matters is they compare unequal.
    auto tokenOtherCollection = ResumeToken::parse(
        ResumeToken(ResumeTokenData(ts1, Value(documentKey1a), testUuid2)).toDocument());
    ASSERT_NE(token1a, tokenOtherCollection);

    auto token2 = ResumeToken::parse(
        ResumeToken(ResumeTokenData(ts1, Value(documentKey2), testUuid)).toDocument());

    // Document keys matter.
    ASSERT_LT(token1a, token2);
    ASSERT_LTE(token1a, token2);
    ASSERT_GT(token2, token1a);
    ASSERT_GTE(token2, token1a);

    auto token3 = ResumeToken::parse(
        ResumeToken(ResumeTokenData(ts1, Value(documentKey3), testUuid)).toDocument());

    // Order within document keys matters.
    ASSERT_LT(token1a, token3);
    ASSERT_LTE(token1a, token3);
    ASSERT_GT(token3, token1a);
    ASSERT_GTE(token3, token1a);

    ASSERT_LT(token2, token3);
    ASSERT_LTE(token2, token3);
    ASSERT_GT(token3, token2);
    ASSERT_GTE(token3, token2);

    auto token4 = ResumeToken::parse(
        ResumeToken(ResumeTokenData(ts2, Value(documentKey1a), testUuid)).toDocument());

    // Timestamps matter.
    ASSERT_LT(token1a, token4);
    ASSERT_LTE(token1a, token4);
    ASSERT_GT(token4, token1a);
    ASSERT_GTE(token4, token1a);

    // Timestamps matter more than document key.
    ASSERT_LT(token3, token4);
    ASSERT_LTE(token3, token4);
    ASSERT_GT(token4, token3);
    ASSERT_GTE(token4, token3);

    auto token5 = ResumeToken::parse(
        ResumeToken(ResumeTokenData(ts3, Value(documentKey1a), testUuid)).toDocument());

    // Time matters more than increment in timestamp
    ASSERT_LT(token4, token5);
    ASSERT_LTE(token4, token5);
    ASSERT_GT(token5, token4);
    ASSERT_GTE(token5, token4);
}

TEST(ResumeToken, FailsToParseForInvalidTokenFormats) {
    // Missing document.
    ASSERT_THROWS(ResumeToken::parse(Document()), AssertionException);
    // Missing data field.
    ASSERT_THROWS(ResumeToken::parse(Document{{"somefield"_sd, "stuff"_sd}}), AssertionException);
    // Wrong type data field
    ASSERT_THROWS(ResumeToken::parse(Document{{"_data"_sd, "string"_sd}}), AssertionException);
    ASSERT_THROWS(ResumeToken::parse(Document{{"_data"_sd, 0}}), AssertionException);

    // Valid data field, but wrong type typeBits.
    Timestamp ts(1010, 4);
    ResumeTokenData tokenData;
    tokenData.clusterTime = ts;
    auto goodTokenDoc = ResumeToken(tokenData).toDocument();
    auto goodData = goodTokenDoc["_data"].getBinData();
    ASSERT_THROWS(ResumeToken::parse(Document{{"_data"_sd, goodData}, {"_typeBits", "string"_sd}}),
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
    auto goodTokenDocBinData = ResumeToken(tokenData).toDocument();
    auto goodData = goodTokenDocBinData["_data"].getBinData();
    const unsigned char zeroes[] = {0, 0, 0, 0, 0};
    const unsigned char nonsense[] = {165, 85, 77, 86, 255};

    // Data of correct type, but empty.  This won't fail until we try to decode the data.
    auto emptyToken =
        ResumeToken::parse(Document{{"_data"_sd, BSONBinData(zeroes, 0, BinDataGeneral)}});
    ASSERT_THROWS_CODE(emptyToken.getData(), AssertionException, 40649);

    // Data of correct type with a bunch of zeros.
    auto zeroesToken = ResumeToken::parse(
        Document{{"_data"_sd, BSONBinData(zeroes, sizeof(zeroes), BinDataGeneral)}});
    ASSERT_THROWS_CODE(zeroesToken.getData(), AssertionException, 50811);

    // Data of correct type with a bunch of nonsense.
    auto nonsenseToken = ResumeToken::parse(
        Document{{"_data"_sd, BSONBinData(nonsense, sizeof(nonsense), BinDataGeneral)}});
    ASSERT_THROWS_CODE(nonsenseToken.getData(), AssertionException, 50811);

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

}  // namspace
}  // namspace mongo
