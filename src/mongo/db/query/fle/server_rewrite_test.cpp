/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include <memory>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/fle/server_rewrite.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"


namespace mongo {
namespace {

class BasicMockFLEQueryRewriter : public fle::FLEQueryRewriter {
public:
    BasicMockFLEQueryRewriter() : fle::FLEQueryRewriter(new ExpressionContextForTest()) {}

    BSONObj rewriteMatchExpressionForTest(const BSONObj& obj) {
        auto res = rewriteMatchExpression(obj);
        return res ? res.value() : obj;
    }
};

class MockFLEQueryRewriter : public BasicMockFLEQueryRewriter {
public:
    MockFLEQueryRewriter() : _tags() {}

    bool isFleFindPayload(const BSONElement& fleFindPayload) const override {
        return _encryptedFields.find(fleFindPayload.fieldNameStringData()) !=
            _encryptedFields.end();
    }

    void setEncryptedTags(std::pair<StringData, int> fieldvalue, BSONObj tags) {
        _encryptedFields.insert(fieldvalue.first);
        _tags[fieldvalue] = tags;
    }

private:
    BSONObj rewritePayloadAsTags(BSONElement fleFindPayload) const override {
        ASSERT(fleFindPayload.isNumber());  // Only accept numbers as mock FFPs.
        ASSERT(_tags.find({fleFindPayload.fieldNameStringData(), fleFindPayload.Int()}) !=
               _tags.end());
        return _tags.find({fleFindPayload.fieldNameStringData(), fleFindPayload.Int()})->second;
    };

    std::map<std::pair<StringData, int>, BSONObj> _tags;
    std::set<StringData> _encryptedFields;
};

class FLEServerRewriteTest : public unittest::Test {
public:
    FLEServerRewriteTest() {}

    void setUp() override {}

    void tearDown() override {}

protected:
    MockFLEQueryRewriter _mock;
};

TEST_F(FLEServerRewriteTest, NoFFP_Equality) {
    auto match = fromjson("{ssn: '5'}");
    auto expected = fromjson("{ssn: '5'}}");

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, NoFFP_In) {
    auto match = fromjson("{ssn: {$in: ['5', '6', '7']}}");
    auto expected = fromjson("{ssn: {$in: ['5', '6', '7']}}");

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Equality) {
    auto match = fromjson("{ssn: 5}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON(kSafeContent << BSON("$in" << tags));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Equality_DottedPath) {
    auto match = fromjson("{'user.ssn': {$eq: 5}}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"user.ssn", 5}, tags);
    auto expected = BSON(kSafeContent << BSON("$in" << tags));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_In) {
    auto match = fromjson("{ssn: {$in: [2, 4, 6]}}");

    // The key/value pairs that the mock functions use to determine the fake FFPs are inside an
    // array, and so the keys are the index values and the values are the actual array elements.
    _mock.setEncryptedTags({"0", 2}, BSON_ARRAY(1 << 2));
    _mock.setEncryptedTags({"1", 4}, BSON_ARRAY(5 << 3));
    _mock.setEncryptedTags({"2", 6}, BSON_ARRAY(99 << 100));

    // Order doesn't matter in a disjunction.
    auto expected = BSON(kSafeContent << BSON("$in" << BSON_ARRAY(1 << 2 << 3 << 5 << 99 << 100)));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_In_DottedPath) {
    auto match = fromjson("{'user.ssn': {$in: [2, 4, 6]}}");

    // The key/value pairs that the mock functions use to determine the fake FFPs are inside an
    // array, and so the keys are the index values and the values are the actual array elements.
    _mock.setEncryptedTags({"0", 2}, BSON_ARRAY(1 << 2));
    _mock.setEncryptedTags({"1", 4}, BSON_ARRAY(5 << 3));
    _mock.setEncryptedTags({"2", 6}, BSON_ARRAY(99 << 100));

    // Order doesn't matter in a disjunction.
    auto expected = BSON(kSafeContent << BSON("$in" << BSON_ARRAY(1 << 2 << 3 << 5 << 99 << 100)));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Conjunction_BothEncrypted) {
    auto match = fromjson("{$and: [{ssn: 5}, {age: 36}]}");
    auto ssnTags = BSON_ARRAY(1 << 2 << 3);
    auto ageTags = BSON_ARRAY(22 << 44 << 66);

    _mock.setEncryptedTags({"ssn", 5}, ssnTags);
    _mock.setEncryptedTags({"age", 36}, ageTags);
    auto expected = BSON("$and" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << ssnTags))
                                              << BSON(kSafeContent << BSON("$in" << ageTags))));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Conjunction_PartlyEncrypted) {
    auto match = fromjson("{$and: [{ssn: 5}, {notSsn: 6}]}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$and" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))
                                              << BSON("notSsn" << BSON("$eq" << 6))));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_CompoundEquality_PartlyEncrypted) {
    auto match = fromjson("{ssn: 5, notSsn: 6}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$and" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))
                                              << BSON("notSsn" << BSON("$eq" << 6))));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Encrypted_Nested_Unencrypted) {
    auto match = fromjson("{ssn: 5, user: {region: 'US'}}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$and" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))
                                              << BSON("user" << BSON("$eq" << BSON("region"
                                                                                   << "US")))));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Not_Equality) {
    auto match = fromjson("{ssn: {$not: {$eq: 5}}}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON(kSafeContent << BSON("$not" << BSON("$in" << tags)));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Neq) {
    auto match = fromjson("{ssn: {$ne: 5}}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON(kSafeContent << BSON("$not" << BSON("$in" << tags)));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}


TEST_F(FLEServerRewriteTest, TopLevel_And_In) {
    auto match = fromjson("{$and: [{ssn: {$in: [2, 4, 6]}}, {region: 'US'}]}");

    _mock.setEncryptedTags({"0", 2}, BSON_ARRAY(1 << 2));
    _mock.setEncryptedTags({"1", 4}, BSON_ARRAY(5 << 3));
    _mock.setEncryptedTags({"2", 6}, BSON_ARRAY(99 << 100));

    auto expected =
        BSON("$and" << BSON_ARRAY(
                 BSON(kSafeContent << BSON("$in" << BSON_ARRAY(1 << 2 << 3 << 5 << 99 << 100)))
                 << BSON("region" << BSON("$eq"
                                          << "US"))));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, NestedConjunction) {
    auto match = fromjson("{$and: [{$and: [{ssn: 2}, {other: 3}]}, {otherSsn: 5}]}");

    _mock.setEncryptedTags({"ssn", 2}, BSON_ARRAY(1 << 2));
    _mock.setEncryptedTags({"otherSsn", 5}, BSON_ARRAY(3 << 4));

    auto expected = fromjson(R"(
        { $and: [
            { $and: [
                { __safeContent__: { $in: [ 1, 2 ] } },
                { other: { $eq: 3 } }
            ] },
            { __safeContent__: { $in: [ 3, 4 ] } }
        ] })");

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Nor_Equality) {
    auto match = fromjson("{$nor: [{ssn: 5}]}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$nor" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Nor_Equality_WithUnencrypted) {
    auto match = fromjson("{$nor: [{ssn: 5}, {region: 'US'}]}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$nor" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))
                                              << BSON("region" << BSON("$eq"
                                                                       << "US"))));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Or_Equality_WithUnencrypted) {
    auto match = fromjson("{$or: [{ssn: 5}, {region: 'US'}]}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$or" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))
                                             << BSON("region" << BSON("$eq"
                                                                      << "US"))));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Not_In) {
    auto match = fromjson("{ssn: {$not: {$in: [2, 4, 6]}}}");

    _mock.setEncryptedTags({"0", 2}, BSON_ARRAY(1 << 2));
    _mock.setEncryptedTags({"1", 4}, BSON_ARRAY(5 << 3));
    _mock.setEncryptedTags({"2", 6}, BSON_ARRAY(99 << 100));

    auto expected = BSON(
        kSafeContent << BSON("$not" << BSON("$in" << BSON_ARRAY(1 << 2 << 3 << 5 << 99 << 100))));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Nin) {
    auto match = fromjson("{ssn: {$nin: [2, 4, 6]}}");

    _mock.setEncryptedTags({"0", 2}, BSON_ARRAY(1 << 2));
    _mock.setEncryptedTags({"1", 4}, BSON_ARRAY(5 << 3));
    _mock.setEncryptedTags({"2", 6}, BSON_ARRAY(99 << 100));

    // Order doesn't matter in a disjunction.
    auto expected = BSON(
        kSafeContent << BSON("$not" << BSON("$in" << BSON_ARRAY(1 << 2 << 3 << 5 << 99 << 100))));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, InMixOfEncryptedElementsIsDisallowed) {
    auto match = fromjson("{ssn: {$in: [2, 4, 6]}}");

    _mock.setEncryptedTags({"0", 2}, BSON_ARRAY(1 << 2));
    _mock.setEncryptedTags({"1", 4}, BSON_ARRAY(5 << 3));

    ASSERT_THROWS_CODE(_mock.rewriteMatchExpressionForTest(match), AssertionException, 6329400);
}

TEST_F(FLEServerRewriteTest, ComparisonToObjectIgnored) {
    // Although such a query should fail in query analysis, it's not realistic for us to catch all
    // the ways a FLEFindPayload could be improperly included in an explicitly encrypted query, so
    // this test demonstrates the server side behavior.
    {
        auto match = fromjson("{user: {$eq: {ssn: 5}}}");

        _mock.setEncryptedTags({"user.ssn", 5}, BSON_ARRAY(1 << 2));

        auto actual = _mock.rewriteMatchExpressionForTest(match);
        ASSERT_BSONOBJ_EQ(actual, match);
    }
    {
        auto match = fromjson("{user: {$in: [{ssn: 5}]}}");

        _mock.setEncryptedTags({"user.ssn", 5}, BSON_ARRAY(1 << 2));

        auto actual = _mock.rewriteMatchExpressionForTest(match);
        ASSERT_BSONOBJ_EQ(actual, match);
    }
}

template <typename T>
std::vector<uint8_t> toEncryptedVector(EncryptedBinDataType dt, T t) {
    BSONObj obj = t.toBSON();

    std::vector<uint8_t> buf(obj.objsize() + 1);
    buf[0] = static_cast<uint8_t>(dt);

    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), buf.data() + 1);

    return buf;
}

template <typename T>
void toEncryptedBinData(StringData field, EncryptedBinDataType dt, T t, BSONObjBuilder* builder) {
    auto buf = toEncryptedVector(dt, t);

    builder->appendBinData(field, buf.size(), BinDataType::Encrypt, buf.data());
}

constexpr auto kIndexKeyId = "12345678-1234-9876-1234-123456789012"_sd;
constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID indexKeyId = uassertStatusOK(UUID::parse(kIndexKeyId.toString()));
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId.toString()));

std::vector<char> testValue = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
std::vector<char> testValue2 = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29};

const FLEIndexKey& getIndexKey() {
    static std::string indexVec = hexblob::decode(
        "7dbfebc619aa68a659f64b8e23ccd21644ac326cb74a26840c3d2420176c40ae088294d00ad6cae9684237b21b754cf503f085c25cd320bf035c3417416e1e6fe3d9219f79586582112740b2add88e1030d91926ae8afc13ee575cfb8bb965b7"_sd);
    static FLEIndexKey indexKey(KeyMaterial(indexVec.begin(), indexVec.end()));
    return indexKey;
}

const FLEUserKey& getUserKey() {
    static std::string userVec = hexblob::decode(
        "a7ddbc4c8be00d51f68d9d8e485f351c8edc8d2206b24d8e0e1816d005fbe520e489125047d647b0d8684bfbdbf09c304085ed086aba6c2b2b1677ccc91ced8847a733bf5e5682c84b3ee7969e4a5fe0e0c21e5e3ee190595a55f83147d8de2a"_sd);
    static FLEUserKey userKey(KeyMaterial(userVec.begin(), userVec.end()));
    return userKey;
}


BSONObj generateFFP(StringData path, int value) {
    auto indexKey = getIndexKey();
    FLEIndexKeyAndId indexKeyAndId(indexKey.data, indexKeyId);
    auto userKey = getUserKey();
    FLEUserKeyAndId userKeyAndId(userKey.data, indexKeyId);

    BSONObj doc = BSON("value" << value);
    auto element = doc.firstElement();
    auto fpp = FLEClientCrypto::serializeFindPayload(indexKeyAndId, userKeyAndId, element, 0);

    BSONObjBuilder builder;
    toEncryptedBinData(path, EncryptedBinDataType::kFLE2FindEqualityPayload, fpp, &builder);
    return builder.obj();
}

class FLEServerHighCardRewriteTest : public unittest::Test {
public:
    FLEServerHighCardRewriteTest() {}

    void setUp() override {}

    void tearDown() override {}

protected:
    BasicMockFLEQueryRewriter _mock;
};


TEST_F(FLEServerHighCardRewriteTest, HighCard_TopLevel_Equality) {
    _mock.setForceHighCardinalityForTest();

    auto match = generateFFP("ssn", 1);
    auto expected = fromjson(R"({
    "$expr": {
        "$_internalFleEq": {
            "field": "$ssn",
            "edc": {
                "$binary": {
                    "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                    "subType": "6"
                }
            },
            "counter": {
                "$numberLong": "0"
            },
            "server": {
                "$binary": {
                    "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                    "subType": "6"
                }
            }
        }
    }
})");

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}


TEST_F(FLEServerHighCardRewriteTest, HighCard_TopLevel_In) {
    _mock.setForceHighCardinalityForTest();

    auto ffp1 = generateFFP("ssn", 1);
    auto ffp2 = generateFFP("ssn", 2);
    auto ffp3 = generateFFP("ssn", 3);
    auto expected = fromjson(R"({
    "$or": [
        {
            "$expr": {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "edc": {
                        "$binary": {
                            "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                            "subType": "6"
                        }
                    },
                    "counter": {
                        "$numberLong": "0"
                    },
                    "server": {
                        "$binary": {
                            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                            "subType": "6"
                        }
                    }
                }
            }
        },
        {
            "$expr": {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "edc": {
                        "$binary": {
                            "base64": "CLpCo6rNuYMVT+6n1HCX15MNrVYDNqf6udO46ayo43Sw",
                            "subType": "6"
                        }
                    },
                    "counter": {
                        "$numberLong": "0"
                    },
                    "server": {
                        "$binary": {
                            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                            "subType": "6"
                        }
                    }
                }
            }
        },
        {
            "$expr": {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "edc": {
                        "$binary": {
                            "base64": "CPi44oCQHnNDeRqHsNLzbdCeHt2DK/wCly0g2dxU5fqN",
                            "subType": "6"
                        }
                    },
                    "counter": {
                        "$numberLong": "0"
                    },
                    "server": {
                        "$binary": {
                            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                            "subType": "6"
                        }
                    }
                }
            }
        }
    ]
})");

    auto match =
        BSON("ssn" << BSON("$in" << BSON_ARRAY(ffp1.firstElement()
                                               << ffp2.firstElement() << ffp3.firstElement())));

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}


TEST_F(FLEServerHighCardRewriteTest, HighCard_TopLevel_Expr) {

    _mock.setForceHighCardinalityForTest();

    auto ffp = generateFFP("$ssn", 1);
    int len;
    auto v = ffp.firstElement().binDataClean(len);
    auto match = BSON("$expr" << BSON("$eq" << BSON_ARRAY(ffp.firstElement().fieldName()
                                                          << BSONBinData(v, len, Encrypt))));

    auto expected = fromjson(R"({ "$expr": {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "edc": {
                        "$binary": {
                            "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                            "subType": "6"
                        }
                    },
                    "counter": {
                        "$numberLong": "0"
                    },
                    "server": {
                        "$binary": {
                            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                            "subType": "6"
                        }
                    }
                }
            }
    })");

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerHighCardRewriteTest, HighCard_TopLevel_Expr_In) {

    _mock.setForceHighCardinalityForTest();

    auto ffp = generateFFP("$ssn", 1);
    int len;
    auto v = ffp.firstElement().binDataClean(len);

    auto ffp2 = generateFFP("$ssn", 1);
    int len2;
    auto v2 = ffp2.firstElement().binDataClean(len2);

    auto match = BSON(
        "$expr" << BSON("$in" << BSON_ARRAY(ffp.firstElement().fieldName()
                                            << BSON_ARRAY(BSONBinData(v, len, Encrypt)
                                                          << BSONBinData(v2, len2, Encrypt)))));

    auto expected = fromjson(R"({ "$expr": { "$or" : [ {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "edc": {
                        "$binary": {
                            "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                            "subType": "6"
                        }
                    },
                    "counter": {
                        "$numberLong": "0"
                    },
                    "server": {
                        "$binary": {
                            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                            "subType": "6"
                        }
                    }
                }},
                {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "edc": {
                        "$binary": {
                            "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                            "subType": "6"
                        }
                    },
                    "counter": {
                        "$numberLong": "0"
                    },
                    "server": {
                        "$binary": {
                            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                            "subType": "6"
                        }
                    }
                }}
            ]}})");

    auto actual = _mock.rewriteMatchExpressionForTest(match);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

}  // namespace
}  // namespace mongo
