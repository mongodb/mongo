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
#include "encrypted_predicate_test_fixtures.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "range_validator.h"
#include <initializer_list>

namespace mongo::fle {
class RangeValidatorTest : public unittest::Test {
public:
    void setUp() override {}

    void tearDown() override {}

    RangeValidatorTest() : _expCtx(new ExpressionContextForTest()) {}

    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;

    std::vector<Fle2RangeOperator> operators = {Fle2RangeOperator::kGt,
                                                Fle2RangeOperator::kGte,
                                                Fle2RangeOperator::kLt,
                                                Fle2RangeOperator::kLte};

    std::vector<Fle2RangeOperator> lowerBounds = {Fle2RangeOperator::kGt, Fle2RangeOperator::kGte};
    std::vector<Fle2RangeOperator> upperBounds = {Fle2RangeOperator::kLt, Fle2RangeOperator::kLte};
};

#define ASSERT_VALID_RANGE_QUERY(json)                                             \
    {                                                                              \
        auto input = fromjson(json);                                               \
        auto expr = uassertStatusOK(MatchExpressionParser::parse(input, _expCtx)); \
        ASSERT_DOES_NOT_THROW(validateRanges(*expr.get()));                        \
    }

#define ASSERT_INVALID_RANGE_QUERY(code, json)                                     \
    {                                                                              \
        auto input = fromjson(json);                                               \
        std::cout << "DAVISDEBUG " << input << std::endl;                          \
        auto expr = uassertStatusOK(MatchExpressionParser::parse(input, _expCtx)); \
        ASSERT_THROWS_CODE(validateRanges(*expr.get()), DBException, code);        \
    }

namespace {
// These helper functions help construct encrypted queries in a succinct way. The function names are
// short in order to make the queries that we are testing more readable at the callsite.
// All of these functions generate strings with JSON which ultimately are parsed into
// MatchExpressions before being passed to the validator function for testing.

/**
 * Generate a range payload. Edges can be empty because we don't actually look at the payload's
 * contents during validation.
 */
std::string payload(StringData path,
                    int32_t payloadId,
                    Fle2RangeOperator firstOp,
                    boost::optional<Fle2RangeOperator> secondOp) {
    auto indexKey = getIndexKey();
    FLEIndexKeyAndId indexKeyAndId(indexKey.data, indexKeyId);
    auto userKey = getUserKey();
    FLEUserKeyAndId userKeyAndId(userKey.data, indexKeyId);

    FLE2RangeFindSpec spec(payloadId, firstOp);
    spec.setSecondOperator(secondOp);

    auto ffp = FLEClientCrypto::serializeFindRangePayload(
        indexKeyAndId, userKeyAndId, std::vector<std::string>(), 0, spec);

    BSONObjBuilder builder;
    toEncryptedBinData(path, EncryptedBinDataType::kFLE2FindRangePayload, ffp, &builder);
    return builder.obj().firstElement().jsonString(ExtendedCanonicalV2_0_0, false, false);
}

/**
 * Generate a range stub.
 */
std::string stub(StringData path,
                 int32_t payloadId,
                 Fle2RangeOperator firstOp,
                 boost::optional<Fle2RangeOperator> secondOp) {
    FLE2RangeFindSpec spec(payloadId, firstOp);
    spec.setSecondOperator(secondOp);
    auto ffp = FLEClientCrypto::serializeFindRangeStub(spec);

    BSONObjBuilder builder;
    toEncryptedBinData(path, EncryptedBinDataType::kFLE2FindRangePayload, ffp, &builder);
    return builder.obj().firstElement().jsonString(ExtendedCanonicalV2_0_0, false, false);
}


/**
 * Generate a one-sided range predicate with the given range comparison operator.
 */
std::string range(std::string field, Fle2RangeOperator op, std::string payload) {
    return str::stream() << "{" << field << ": {" << op << ": " << payload << "}}";
}

/**
 * Generate a one-sided range predicate under a $not with the given range comparison operator.
 */
std::string rangeNot(std::string field, Fle2RangeOperator op, std::string payload) {
    return str::stream() << "{" << field << ": {$not: {" << op << ": " << payload << "}}}";
}

/**
 * Generate a two-sided range predicate.
 */
std::string range(std::string field,
                  Fle2RangeOperator op1,
                  std::string p1,
                  Fle2RangeOperator op2,
                  std::string p2) {
    return str::stream() << "{" << field << ": {" << op1 << ": " << p1 << ", " << op2 << ": " << p2
                         << "}}";
}

std::string queryOp(std::string op, std::initializer_list<std::string> children) {
    auto ss = std::move(str::stream() << "{" << op << ": [");
    for (auto it = children.begin(); it != children.end(); ++it) {
        ss = std::move(ss << *it);
        if (it + 1 != children.end()) {
            ss = std::move(ss << ",");
        }
    }
    return ss << "]}";
}

/**
 * Build up a conjunction of other predicates that have been serialized to JSON.
 */
std::string dollarAnd(std::initializer_list<std::string> filters) {
    return queryOp("$and", std::move(filters));
}
}  // namespace

///////////////////////////////
//// HAPPY PATH TEST CASES ////
///////////////////////////////

TEST_F(RangeValidatorTest, TopLevel_OneSide) {
    for (auto op : operators) {
        ASSERT_VALID_RANGE_QUERY(range("age", op, payload("age", 0, op, boost::none)));
    }
}

TEST_F(RangeValidatorTest, TopLevel_TwoSided) {
    for (auto lb : lowerBounds) {
        for (auto ub : upperBounds) {
            ASSERT_VALID_RANGE_QUERY(
                range("age", lb, payload("age", 0, lb, ub), ub, stub("age", 0, lb, ub)));
        }
    }
}

TEST_F(RangeValidatorTest, Nested_OneSided) {
    for (auto lb : operators) {
        ASSERT_VALID_RANGE_QUERY(dollarAnd({BSON("name"
                                                 << "hello")
                                                .jsonString(),
                                            range("age", lb, payload("age", 0, lb, boost::none))}));
    }
}

TEST_F(RangeValidatorTest, Nested_TwoRanges) {
    for (auto ageLb : lowerBounds) {
        for (auto ageUb : upperBounds) {
            for (auto salaryLb : lowerBounds) {
                for (auto salaryUb : upperBounds) {
                    ASSERT_VALID_RANGE_QUERY(dollarAnd({
                        range("age",
                              ageLb,
                              payload("age", 0, ageLb, ageUb),
                              ageUb,
                              stub("age", 0, ageLb, ageUb)),
                        range("salary",
                              salaryLb,
                              payload("salary", 1, salaryLb, salaryUb),
                              salaryUb,
                              stub("salary", 1, salaryLb, salaryUb)),
                    }))
                }
            }
        }
    }
}

TEST_F(RangeValidatorTest, OneSidedWithTwoSided) {
    for (auto lbA : lowerBounds) {
        for (auto ubA : upperBounds) {
            for (auto opB : operators) {
                ASSERT_VALID_RANGE_QUERY(dollarAnd({
                    range("age", lbA, payload("age", 0, lbA, ubA), ubA, stub("age", 0, lbA, ubA)),
                    range("salary", opB, payload("salary", 1, opB, boost::none)),
                }))
            }
        }
    }
}

TEST_F(RangeValidatorTest, TwoOneSided) {
    for (auto opA : operators) {
        for (auto opB : operators) {
            ASSERT_VALID_RANGE_QUERY(dollarAnd({
                range("age", opA, payload("age", 0, opA, boost::none)),
                range("salary", opB, payload("salary", 1, opB, boost::none)),
            }))
        }
    }
}

//////////////////////////
//// ERROR TEST CASES ////
//////////////////////////


TEST_F(RangeValidatorTest, OneSided_Stub) {
    for (auto op : operators) {
        ASSERT_INVALID_RANGE_QUERY(7030709, range("age", op, stub("age", 0, op, boost::none)));
    }
}

TEST_F(RangeValidatorTest, LogicalOps_TopLevel) {
    auto logicalOps = {"$or", "$nor"};
    for (auto op : operators) {
        for (auto logicalOp : logicalOps) {
            ASSERT_INVALID_RANGE_QUERY(
                7030709,
                queryOp(logicalOp,
                        {
                            range("age", op, payload("age", 0, op, boost::none)),
                            range("age", op, stub("age", 0, op, boost::none)),
                        }));
        }

        ASSERT_INVALID_RANGE_QUERY(7030709, rangeNot("age", op, stub("age", 0, op, boost::none)));
    }
}

TEST_F(RangeValidatorTest, LogicalOps_UnderAnd) {
    auto logicalOps = {"$or", "$nor"};
    for (auto op : operators) {
        for (auto logicalOp : logicalOps) {
            ASSERT_INVALID_RANGE_QUERY(
                7030709,
                dollarAnd(
                    {
                        queryOp(logicalOp,
                                {
                                    range("age", op, payload("age", 0, op, boost::none)),
                                    range("age", op, stub("age", 0, op, boost::none)),
                                }),
                        BSON("hello"
                             << "world")
                            .jsonString(),
                    }));
        }
        ASSERT_INVALID_RANGE_QUERY(
            7030709,
            queryOp("$and",
                    {
                        rangeNot("age", op, payload("age", 0, op, boost::none)),
                        rangeNot("age", op, stub("age", 0, op, boost::none)),
                    }));
    }
}

TEST_F(RangeValidatorTest, OneSided_TwoOperators) {
    for (auto op : operators) {
        for (auto op2 : operators) {
            ASSERT_INVALID_RANGE_QUERY(7030710, range("age", op, payload("age", 0, op, op2)));
        }
    }
}

TEST_F(RangeValidatorTest, OneSided_WrongOp) {
    for (auto payloadOp : operators) {
        for (auto filterOp : operators) {
            if (payloadOp == filterOp) {
                continue;
            }
            ASSERT_INVALID_RANGE_QUERY(
                7030711, range("age", filterOp, payload("age", 0, payloadOp, boost::none)));
        }
    }
}


TEST_F(RangeValidatorTest, TwoSided_OperatorDoubled) {
    for (auto lb : lowerBounds) {
        for (auto ub : upperBounds) {
            {
                ASSERT_INVALID_RANGE_QUERY(
                    7030700,
                    range("age", lb, payload("age", 0, lb, lb), ub, stub("age", 0, lb, lb)));
            }
            {
                ASSERT_INVALID_RANGE_QUERY(
                    7030701,
                    range("age", ub, payload("age", 0, ub, ub), lb, stub("age", 0, ub, ub)));
            }
        }
    }
}

TEST_F(RangeValidatorTest, TwoSided_OperatorMismatch) {
    for (auto lb : lowerBounds) {
        for (auto ub : upperBounds) {
            for (auto lb2 : lowerBounds) {
                if (lb == lb2) {
                    continue;
                }
                ASSERT_INVALID_RANGE_QUERY(
                    7030702,
                    range("age", lb, payload("age", 0, lb, ub), ub, stub("age", 0, lb2, ub)))
            }
            for (auto ub2 : upperBounds) {
                if (ub == ub2) {
                    continue;
                }
                ASSERT_INVALID_RANGE_QUERY(
                    7030704,
                    range("age", lb, payload("age", 0, lb, ub), ub, stub("age", 0, lb, ub2)))
            }
        }
    }
}

TEST_F(RangeValidatorTest, TwoSided_SecondOpNotPresentInPayload) {
    for (auto lb : lowerBounds) {
        for (auto ub : upperBounds) {
            ASSERT_INVALID_RANGE_QUERY(
                7030709,
                range("age", lb, payload("age", 0, lb, ub), ub, stub("age", 0, lb, boost::none)));
            ASSERT_INVALID_RANGE_QUERY(
                7030715,
                range("age", lb, stub("age", 0, lb, ub), ub, payload("age", 0, ub, boost::none)));
        }
    }
}

TEST_F(RangeValidatorTest, TwoSided_OpUsedTwice) {
    for (auto lb : lowerBounds) {
        for (auto ub : upperBounds) {
            ASSERT_INVALID_RANGE_QUERY(
                7030705, range("age", lb, payload("age", 0, lb, ub), lb, stub("age", 0, lb, ub)));
            ASSERT_INVALID_RANGE_QUERY(
                7030706, range("age", ub, payload("age", 0, lb, ub), ub, stub("age", 0, lb, ub)));
        }
    }
}

TEST_F(RangeValidatorTest, TwoSided_WrongOp) {
    for (auto payloadLb : lowerBounds) {
        for (auto payloadUb : upperBounds) {
            for (auto filterLb : lowerBounds) {
                if (payloadLb == filterLb) {
                    continue;
                }
                for (auto filterUb : upperBounds) {
                    if (payloadUb == filterUb) {
                        continue;
                    }
                    ASSERT_INVALID_RANGE_QUERY(7030716,
                                               range("age",
                                                     filterLb,
                                                     payload("age", 0, payloadLb, payloadUb),
                                                     payloadLb,
                                                     stub("age", 0, payloadLb, payloadUb)));
                    ASSERT_INVALID_RANGE_QUERY(7030716,
                                               range("age",
                                                     payloadUb,
                                                     payload("age", 0, payloadLb, payloadUb),
                                                     filterUb,
                                                     stub("age", 0, payloadLb, payloadUb)));
                }
            }
        }
    }
}

TEST_F(RangeValidatorTest, TwoSided_SameBlobs) {
    for (auto lb : lowerBounds) {
        for (auto ub : upperBounds) {
            ASSERT_INVALID_RANGE_QUERY(
                7030707,
                range("age", lb, payload("age", 0, lb, ub), ub, payload("age", 0, lb, ub)));
            ASSERT_INVALID_RANGE_QUERY(
                7030708, range("age", lb, stub("age", 0, lb, ub), ub, stub("age", 0, lb, ub)));
        }
    }
}

TEST_F(RangeValidatorTest, TwoSided_MismatchedBlobs) {
    for (auto lb : lowerBounds) {
        for (auto ub : upperBounds) {
            ASSERT_INVALID_RANGE_QUERY(
                7030715,
                range("age", lb, payload("age", 1, lb, ub), ub, payload("age", 0, lb, ub)));
        }
    }
}

TEST_F(RangeValidatorTest, TwoSided_MatchingPayloadsAtDifferentLevels) {
    for (auto lb : lowerBounds) {
        for (auto ub : upperBounds) {
            ASSERT_INVALID_RANGE_QUERY(
                7030709,
                queryOp("$and",
                        {range("age", lb, payload("age", 0, lb, ub)),
                         queryOp("$or",
                                 {BSON("hello"
                                       << "world")
                                      .jsonString(),
                                  range("age", ub, stub("age", 0, lb, ub))})}));
            ASSERT_INVALID_RANGE_QUERY(
                7030710,
                queryOp("$and",
                        {range("age", lb, stub("age", 0, lb, ub)),
                         queryOp("$or",
                                 {BSON("hello"
                                       << "world")
                                      .jsonString(),
                                  range("age", ub, payload("age", 0, lb, ub))})}));
        }
    }
}
}  // namespace mongo::fle
