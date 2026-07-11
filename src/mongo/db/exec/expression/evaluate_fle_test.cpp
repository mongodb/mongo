// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace expression_evaluation_test {

// Test we return true if it matches
TEST(ExpressionFLETest, TestBinData) {

    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;

    {
        auto expr = fromjson(R"({$_internalFleEq: {
        field: {
            "$binary": {
                "base64":
                "DpmKcmnbZ0q1pl/PVwNUh2kCFxinumXuHn6hOSbp+cge6qsJsh7GhSCgRen8HT9JkOZSkZQlSn4IU1vqmTdKRtpk/xX2YJdG76qRYahnyLhl44xjm5Nw1TTMTxAYW3/F0ZTZeWRb2vsU8ICPlHh4xn7isVzmp/0G9k19x67xzboc57gvFXpmCJ3i2qcDAJwaN1fVL/4+S0jJYje8HwgS6qXXaJBCyiZzd31LDXZLWMYkiDvrJBZEMeAnu8gATM5Hg+9Hfte7/C37QED8jjxmAoVB",
                    "subType": "6"
            }
        },
        server: {
            "$binary": {
                "base64": "CPFLfo1iUCYtRSLiuB+Bt5d1tAe/BCfIfAoGmQLBqBhO",
                "subType": "6"
            }
        }    } })");

        auto exprFle = ExpressionInternalFLEEqual::parse(&expCtx, expr.firstElement(), vps);

        ASSERT_VALUE_EQ(exprFle->evaluate({}, &expCtx.variables), Value(true));
    }

    {
        auto expr = fromjson(R"({$_internalFleEq: {
        field: {
            "$binary": {
                "base64":
                "DpmKcmnbZ0q1pl/PVwNUh2kCFxinumXuHn6hOSbp+cge6qsJsh7GhSCgRen8HT9JkOZSkZQlSn4IU1vqmTdKRtpk/xX2YJdG76qRYahnyLhl44xjm5Nw1TTMTxAYW3/F0ZTZeWRb2vsU8ICPlHh4xn7isVzmp/0G9k19x67xzboc57gvFXpmCJ3i2qcDAJwaN1fVL/4+S0jJYje8HwgS6qXXaJBCyiZzd31LDXZLWMYkiDvrJBZEMeAnu8gATM5Hg+9Hfte7/C37QED8jjxmAoVB",
                    "subType": "6"
            }
        },
        server: {
            "$binary": {
                "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                "subType": "6"
            }
        }    } })");

        auto exprFle = ExpressionInternalFLEEqual::parse(&expCtx, expr.firstElement(), vps);

        ASSERT_VALUE_EQ(exprFle->evaluate({}, &expCtx.variables), Value(false));
    }
}

TEST(ExpressionFLETest, TestBinData_RoundTrip) {

    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;

    auto expr = fromjson(R"({$_internalFleEq: {
    field: {
        "$binary": {
            "base64":
            "DpmKcmnbZ0q1pl/PVwNUh2kCFxinumXuHn6hOSbp+cge6qsJsh7GhSCgRen8HT9JkOZSkZQlSn4IU1vqmTdKRtpk/xX2YJdG76qRYahnyLhl44xjm5Nw1TTMTxAYW3/F0ZTZeWRb2vsU8ICPlHh4xn7isVzmp/0G9k19x67xzboc57gvFXpmCJ3i2qcDAJwaN1fVL/4+S0jJYje8HwgS6qXXaJBCyiZzd31LDXZLWMYkiDvrJBZEMeAnu8gATM5Hg+9Hfte7/C37QED8jjxmAoVB",
                "subType": "6"
        }
    },
    server: {
        "$binary": {
            "base64": "CPFLfo1iUCYtRSLiuB+Bt5d1tAe/BCfIfAoGmQLBqBhO",
            "subType": "6"
        }
    }    } })");

    auto exprFle = ExpressionInternalFLEEqual::parse(&expCtx, expr.firstElement(), vps);

    ASSERT_VALUE_EQ(exprFle->evaluate({}, &expCtx.variables), Value(true));

    auto value = exprFle->serialize();

    auto roundTripExpr = fromjson(R"({$_internalFleEq: {
    field: {
        "$const" : { "$binary": {
            "base64":
            "DpmKcmnbZ0q1pl/PVwNUh2kCFxinumXuHn6hOSbp+cge6qsJsh7GhSCgRen8HT9JkOZSkZQlSn4IU1vqmTdKRtpk/xX2YJdG76qRYahnyLhl44xjm5Nw1TTMTxAYW3/F0ZTZeWRb2vsU8ICPlHh4xn7isVzmp/0G9k19x67xzboc57gvFXpmCJ3i2qcDAJwaN1fVL/4+S0jJYje8HwgS6qXXaJBCyiZzd31LDXZLWMYkiDvrJBZEMeAnu8gATM5Hg+9Hfte7/C37QED8jjxmAoVB",
                "subType": "6"
        }}
    },
    server: {
        "$binary": {
            "base64": "CPFLfo1iUCYtRSLiuB+Bt5d1tAe/BCfIfAoGmQLBqBhO",
            "subType": "6"
        }
    }    } })");

    ASSERT_BSONOBJ_EQ(value.getDocument().toBson(), roundTripExpr);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
