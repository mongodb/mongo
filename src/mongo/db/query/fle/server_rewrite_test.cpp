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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/fle/server_rewrite.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"


namespace mongo {
namespace {

class MockFindRewriter : public fle::FLEFindRewriter {
public:
    MockFindRewriter() : fle::FLEFindRewriter(), _tags() {}

    bool isFleFindPayload(const BSONElement& fleFindPayload) override {
        return _encryptedFields.find(fleFindPayload.fieldNameStringData()) !=
            _encryptedFields.end();
    }

    void setEncryptedTags(std::pair<StringData, int> fieldvalue, BSONObj tags) {
        _encryptedFields.insert(fieldvalue.first);
        _tags[fieldvalue] = tags;
    }

private:
    BSONObj rewritePayloadAsTags(BSONElement fleFindPayload) override {
        ASSERT(fleFindPayload.isNumber());  // Only accept numbers as mock FFPs.
        ASSERT(_tags.find({fleFindPayload.fieldNameStringData(), fleFindPayload.Int()}) !=
               _tags.end());
        return _tags[{fleFindPayload.fieldNameStringData(), fleFindPayload.Int()}].copy();
    };

    std::map<std::pair<StringData, int>, BSONObj> _tags;
    std::set<StringData> _encryptedFields;
};
class FLEServerRewriteTest : public unittest::Test {
public:
    FLEServerRewriteTest() {}

    void setUp() override {}

    void tearDown() override {}

    std::unique_ptr<MatchExpression> parseMatchExpression(const BSONObj& query) {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        return uassertStatusOK(MatchExpressionParser::parse(query, expCtx));
    }

protected:
    MockFindRewriter _mock;
};

TEST_F(FLEServerRewriteTest, NoFFP_Equality) {
    auto match = fromjson("{ssn: '5'}");
    auto expected = fromjson("{ssn: {$eq: '5'}}");

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, NoFFP_In) {
    auto match = fromjson("{ssn: {$in: ['5', '6', '7']}}");
    auto expected = fromjson("{ssn: {$in: ['5', '6', '7']}}");

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Equality) {
    auto match = fromjson("{ssn: 5}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON(kSafeContent << BSON("$in" << tags));

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Equality_DottedPath) {
    auto match = fromjson("{'user.ssn': {$eq: 5}}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"user.ssn", 5}, tags);
    auto expected = BSON(kSafeContent << BSON("$in" << tags));

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
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

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
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

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
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

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Conjunction_PartlyEncrypted) {
    auto match = fromjson("{$and: [{ssn: 5}, {notSsn: 6}]}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$and" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))
                                              << BSON("notSsn" << BSON("$eq" << 6))));

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_CompoundEquality_PartlyEncrypted) {
    auto match = fromjson("{ssn: 5, notSsn: 6}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$and" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))
                                              << BSON("notSsn" << BSON("$eq" << 6))));

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Encrypted_Nested_Unencrypted) {
    auto match = fromjson("{ssn: 5, user: {region: 'US'}}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$and" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))
                                              << BSON("user" << BSON("$eq" << BSON("region"
                                                                                   << "US")))));

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Not_Equality) {
    auto match = fromjson("{ssn: {$not: {$eq: 5}}}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON(kSafeContent << BSON("$not" << BSON("$in" << tags)));

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Neq) {
    auto match = fromjson("{ssn: {$ne: 5}}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON(kSafeContent << BSON("$not" << BSON("$in" << tags)));

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
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

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
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

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Nor_Equality) {
    auto match = fromjson("{$nor: [{ssn: 5}]}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$nor" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))));

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Nor_Equality_WithUnencrypted) {
    auto match = fromjson("{$nor: [{ssn: 5}, {region: 'US'}]}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$nor" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))
                                              << BSON("region" << BSON("$eq"
                                                                       << "US"))));

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Or_Equality_WithUnencrypted) {
    auto match = fromjson("{$or: [{ssn: 5}, {region: 'US'}]}");
    auto tags = BSON_ARRAY(1 << 2 << 3);

    _mock.setEncryptedTags({"ssn", 5}, tags);
    auto expected = BSON("$or" << BSON_ARRAY(BSON(kSafeContent << BSON("$in" << tags))
                                             << BSON("region" << BSON("$eq"
                                                                      << "US"))));

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, TopLevel_Not_In) {
    auto match = fromjson("{ssn: {$not: {$in: [2, 4, 6]}}}");

    _mock.setEncryptedTags({"0", 2}, BSON_ARRAY(1 << 2));
    _mock.setEncryptedTags({"1", 4}, BSON_ARRAY(5 << 3));
    _mock.setEncryptedTags({"2", 6}, BSON_ARRAY(99 << 100));

    auto expected = BSON(
        kSafeContent << BSON("$not" << BSON("$in" << BSON_ARRAY(1 << 2 << 3 << 5 << 99 << 100))));

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
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

    auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(FLEServerRewriteTest, InMixOfEncryptedElementsIsDisallowed) {
    auto match = fromjson("{ssn: {$in: [2, 4, 6]}}");

    _mock.setEncryptedTags({"0", 2}, BSON_ARRAY(1 << 2));
    _mock.setEncryptedTags({"1", 4}, BSON_ARRAY(5 << 3));

    ASSERT_THROWS_CODE(
        _mock.rewriteMatchExpression(parseMatchExpression(match)), AssertionException, 6329400);
}

TEST_F(FLEServerRewriteTest, ComparisonToObjectIgnored) {
    // Although such a query should fail in query analysis, it's not realistic for us to catch all
    // the ways a FLEFindPayload could be improperly included in an explicitly encrypted query, so
    // this test demonstrates the server side behavior.
    {
        auto match = fromjson("{user: {$eq: {ssn: 5}}}");

        _mock.setEncryptedTags({"user.ssn", 5}, BSON_ARRAY(1 << 2));

        auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
        ASSERT_BSONOBJ_EQ(actual, match);
    }
    {
        auto match = fromjson("{user: {$in: [{ssn: 5}]}}");

        _mock.setEncryptedTags({"user.ssn", 5}, BSON_ARRAY(1 << 2));

        auto actual = _mock.rewriteMatchExpression(parseMatchExpression(match))->serialize();
        ASSERT_BSONOBJ_EQ(actual, match);
    }
}

}  // namespace
}  // namespace mongo
