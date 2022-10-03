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

#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/encrypted_predicate_test_fixtures.h"
#include "mongo/db/query/fle/range_predicate.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::fle {
namespace {
class MockRangePredicate : public RangePredicate {
public:
    MockRangePredicate(const QueryRewriterInterface* rewriter) : RangePredicate(rewriter) {}

    MockRangePredicate(const QueryRewriterInterface* rewriter,
                       TagMap tags,
                       std::set<StringData> encryptedFields)
        : RangePredicate(rewriter), _tags(tags), _encryptedFields(encryptedFields) {}

    void setEncryptedTags(std::pair<StringData, int> fieldvalue, std::vector<PrfBlock> tags) {
        _encryptedFields.insert(fieldvalue.first);
        _tags[fieldvalue] = tags;
    }


    bool payloadValid = true;

protected:
    bool isPayload(const BSONElement& elt) const override {
        return payloadValid;
    }

    bool isPayload(const Value& v) const override {
        return payloadValid;
    }

    std::vector<PrfBlock> generateTags(BSONValue payload) const {
        return stdx::visit(
            OverloadedVisitor{[&](BSONElement p) {
                                  auto parsedPayload = p.Obj().firstElement();
                                  auto fieldName = parsedPayload.fieldNameStringData();

                                  std::vector<BSONElement> range;
                                  auto payloadAsArray = parsedPayload.Array();
                                  for (auto&& elt : payloadAsArray) {
                                      range.push_back(elt);
                                  }

                                  std::vector<PrfBlock> allTags;
                                  for (auto i = range[0].Number(); i <= range[1].Number(); i++) {
                                      ASSERT(_tags.find({fieldName, i}) != _tags.end());
                                      auto temp = _tags.find({fieldName, i})->second;
                                      for (auto tag : temp) {
                                          allTags.push_back(tag);
                                      }
                                  }
                                  return allTags;
                              },
                              [&](std::reference_wrapper<Value> v) {
                                  if (v.get().isArray()) {
                                      auto arr = v.get().getArray();
                                      std::vector<PrfBlock> allTags;
                                      for (auto& val : arr) {
                                          allTags.push_back(PrfBlock(
                                              {static_cast<unsigned char>(val.coerceToInt())}));
                                      }
                                      return allTags;
                                  } else {
                                      return std::vector<PrfBlock>{};
                                  }
                              }},
            payload);
    }

private:
    TagMap _tags;
    std::set<StringData> _encryptedFields;
};
class RangePredicateRewriteTest : public EncryptedPredicateRewriteTest {
public:
    RangePredicateRewriteTest() : _predicate(&_mock) {}

protected:
    MockRangePredicate _predicate;
};

TEST_F(RangePredicateRewriteTest, MatchRangeRewrite) {
    RAIIServerParameterControllerForTest controller("featureFlagFLE2Range", true);

    int start = 1;
    int end = 3;
    StringData encField = "ssn";

    std::vector<PrfBlock> tags1 = {{1}, {2}, {3}};
    std::vector<PrfBlock> tags2 = {{4}, {5}, {6}};
    std::vector<PrfBlock> tags3 = {{7}, {8}, {9}};

    _predicate.setEncryptedTags({encField, 1}, tags1);
    _predicate.setEncryptedTags({encField, 2}, tags2);
    _predicate.setEncryptedTags({encField, 3}, tags3);

    std::vector<PrfBlock> allTags = {{1}, {2}, {3}, {4}, {5}, {6}, {7}, {8}, {9}};

    // The field redundancy is so that we can pull out the field
    // name in the mock version of rewriteRangePayloadAsTags.
    BSONObj query =
        BSON(encField << BSON("$between" << BSON(encField << BSON_ARRAY(start << end))));

    auto inputExpr = BetweenMatchExpression(encField, query[encField]["$between"], nullptr);

    assertRewriteToTags(_predicate, &inputExpr, toBSONArray(std::move(allTags)));
}


TEST_F(RangePredicateRewriteTest, AggRangeRewrite) {
    auto input = fromjson(R"({$between: ["$age", {$literal: [1, 2, 3]}]})");
    auto inputExpr =
        ExpressionBetween::parseExpression(&_expCtx, input, _expCtx.variablesParseState);

    auto expected = makeTagDisjunction(&_expCtx, toValues({{1}, {2}, {3}}));

    auto actual = _predicate.rewrite(inputExpr.get());

    ASSERT_BSONOBJ_EQ(actual->serialize(false).getDocument().toBson(),
                      expected->serialize(false).getDocument().toBson());
}

TEST_F(RangePredicateRewriteTest, AggRangeRewriteNoOp) {
    auto input = fromjson(R"({$between: ["$age", {$literal: [1, 2, 3]}]})");
    auto inputExpr =
        ExpressionBetween::parseExpression(&_expCtx, input, _expCtx.variablesParseState);

    auto expected = inputExpr;

    _predicate.payloadValid = false;
    auto actual = _predicate.rewrite(inputExpr.get());
    ASSERT(actual == nullptr);
}

};  // namespace
}  // namespace mongo::fle
