/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
