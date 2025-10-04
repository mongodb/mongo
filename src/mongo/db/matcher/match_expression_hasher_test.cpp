/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_hasher.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
class MatchExpressionHasherTest : public mongo::unittest::Test {
public:
    /**
     * Asserts that the two match expressions are equivalent and also that they hash to the same
     * code.
     */
    void assertEquivalent(BSONObj filter1,
                          BSONObj filter2,
                          const CollatorInterface* collator1 = nullptr,
                          const CollatorInterface* collator2 = nullptr) {
        auto opCtx = serviceContext.makeOperationContext();
        auto expCtx1 = makeContext(collator1, opCtx.get());
        auto expCtx2 = makeContext(collator2, opCtx.get());

        auto expr1 = makeMatchExpression(expCtx1, filter1);
        auto expr2 = makeMatchExpression(expCtx2, filter2);

        str::stream stream;

        if (hash(expr1.get()) != hash(expr2.get())) {
            stream << "MatchExpressions' hashes are not equal.\n";
        }

        if (!expr1->equivalent(expr2.get())) {
            stream << "MatchExpressions are not equivalent.\n";
        }

        if (stream.ss.len() > 0) {
            stream << "First expression: " << expr1->debugString()
                   << "\nSecond expression: " << expr2->debugString();
            FAIL(std::string(stream));
        }
    }

    /**
     * Asserts that the two match expressions are not equivalent and also that they hash to
     * different codes.
     */
    void assertNotEquivalent(BSONObj filter1,
                             BSONObj filter2,
                             const CollatorInterface* collator1 = nullptr,
                             const CollatorInterface* collator2 = nullptr) {
        auto opCtx = serviceContext.makeOperationContext();
        auto expCtx1 = makeContext(collator1, opCtx.get());
        auto expCtx2 = makeContext(collator2, opCtx.get());

        auto expr1 = makeMatchExpression(expCtx1, filter1);
        auto expr2 = makeMatchExpression(expCtx2, filter2);

        str::stream stream;

        if (hash(expr1.get()) == hash(expr2.get())) {
            stream << "MatchExpressions' hashes are equal.\n";
        }

        if (expr1->equivalent(expr2.get())) {
            stream << "MatchExpressions are equivalent.\n";
        }

        if (stream.ss.len() > 0) {
            stream << "First expression: " << expr1->debugString()
                   << "\nSecond expression: " << expr2->debugString();
            FAIL(std::string(stream));
        }
    }


    static boost::intrusive_ptr<ExpressionContext> makeContext(const CollatorInterface* collator,
                                                               OperationContext* opCtx) {
        return make_intrusive<ExpressionContextForTest>(
            opCtx,
            NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd),
            CollatorInterface::cloneCollator(collator));
    }

    static std::unique_ptr<MatchExpression> makeMatchExpression(
        boost::intrusive_ptr<ExpressionContext> expCtx, BSONObj filter) {
        auto expr = MatchExpressionParser::parse(filter,
                                                 expCtx,
                                                 ExtensionsCallbackNoop(),
                                                 MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_OK(expr);
        return std::move(expr.getValue());
    }

    const MatchExpressionHasher hash{};
    QueryTestServiceContext serviceContext{};
};
}  // namespace

TEST_F(MatchExpressionHasherTest, Equality) {
    BSONObj filter1 = BSON("a" << 5);
    BSONObj filter2 = BSON("a" << 5);
    BSONObj filter3 = BSON("a" << 10);

    assertEquivalent(filter1, filter2);
    assertNotEquivalent(filter1, filter3);
}

TEST_F(MatchExpressionHasherTest, Collation) {
    BSONObj filter1 = BSON("a" << 5);
    BSONObj filter2 = BSON("A" << 5);

    CollatorInterfaceMock collator1{CollatorInterfaceMock::MockType::kReverseString};
    CollatorInterfaceMock collator2{CollatorInterfaceMock::MockType::kReverseString};
    CollatorInterfaceMock collator3{CollatorInterfaceMock::MockType::kToLowerString};

    assertEquivalent(filter1, filter1, &collator1, &collator2);
    assertNotEquivalent(filter1, filter1, &collator1, &collator3);
    // Once we fix ComparisonMatchExpession::equivalent() the test below should start failing.
    assertNotEquivalent(filter1, filter2, &collator1, &collator2);
}

TEST_F(MatchExpressionHasherTest, LT_GT) {
    BSONObj gt = BSON("a" << BSON("$gt" << 5));
    BSONObj lt = BSON("a" << BSON("$lt" << 5));

    assertNotEquivalent(gt, lt);
}

TEST_F(MatchExpressionHasherTest, DifferentPaths) {
    BSONObj lt1 = BSON("a" << BSON("$lt" << 5));
    BSONObj lt2 = BSON("b" << BSON("$lt" << 5));

    assertNotEquivalent(lt1, lt2);
}

TEST_F(MatchExpressionHasherTest, Regex) {
    auto regex1 = fromjson("{a: /$abcd/}");
    auto regex2 = fromjson("{a: /$abcd/}");
    auto regex3 = fromjson("{a: /$abc/}");
    auto regex4 = fromjson("{a: /$abcd/i}");
    auto regex5 = fromjson("{b: /$abcd/}");

    assertEquivalent(regex1, regex2);
    assertNotEquivalent(regex1, regex3);
    assertNotEquivalent(regex1, regex4);
    assertNotEquivalent(regex1, regex5);
}

TEST_F(MatchExpressionHasherTest, In_Equalities) {
    auto in1 = fromjson("{a: {$in: [1, 'string', true,  1, [5, 11, 17]]}}");
    auto in2 = fromjson("{a: {$in: [1, 'string', true,     [5, 11, 17]]}}");
    auto in3 = fromjson("{a: {$in: [1, 'string', false, 1, [5, 11, 17]]}}");
    auto in4 = fromjson("{b: {$in: [1, 'string', true,  1, [5, 11, 17]]}}");

    assertEquivalent(in1, in2);
    assertNotEquivalent(in1, in3);
    assertNotEquivalent(in1, in4);
}

TEST_F(MatchExpressionHasherTest, In_Collation) {
    auto in1 = fromjson("{a: {$in: [1, 'hello', 'world']}}");
    auto in2 = fromjson("{a: {$in: [1, 'Hello', 'World']}}");
    auto in3 = fromjson("{a: {$in: [1, 'Moby',  'Dick' ]}}");

    CollatorInterfaceMock collatorToLowerString{CollatorInterfaceMock::MockType::kToLowerString};
    CollatorInterfaceMock collatorAlwaysEqual{CollatorInterfaceMock::MockType::kAlwaysEqual};

    assertEquivalent(in1, in2, &collatorToLowerString, &collatorToLowerString);
    assertEquivalent(in1, in3, &collatorAlwaysEqual, &collatorAlwaysEqual);

    assertNotEquivalent(in1, in1, &collatorToLowerString, &collatorAlwaysEqual);
    assertNotEquivalent(in3, in3, &collatorToLowerString, &collatorAlwaysEqual);
    assertNotEquivalent(in1, in3, &collatorToLowerString, &collatorToLowerString);
}

TEST_F(MatchExpressionHasherTest, In_Regexes) {
    // Main.
    auto in1 = fromjson("{a: {$in: [/$a/,  /$c/i]}}");
    // The same.
    auto in2 = fromjson("{a: {$in: [/$a/,  /$c/i]}}");
    // Defferent regex.
    auto in3 = fromjson("{a: {$in: [/$ab/, /$c/i]}}");
    // Different path.
    auto in4 = fromjson("{b: {$in: [/$a/,  /$c/i]}}");

    assertEquivalent(in1, in2);
    assertNotEquivalent(in1, in3);
    assertNotEquivalent(in1, in4);
}

TEST_F(MatchExpressionHasherTest, BitsAllSet) {
    // 8728 has bits set at the positions: 3, 4, 9, 13.
    auto bits1 = fromjson("{a: {$bitsAllSet: [3, 4, 9, 13]}}");
    auto bits2 = fromjson("{a: {$bitsAllSet: 8728}}");
    auto bits3 = fromjson("{a: {$bitsAllSet: 8729}}");
    auto bits4 = fromjson("{b: {$bitsAllSet: [3, 4, 9, 13]}}");

    assertEquivalent(bits1, bits2);
    assertNotEquivalent(bits1, bits3);
    assertNotEquivalent(bits1, bits4);
}

TEST_F(MatchExpressionHasherTest, Mod) {
    auto mod1 = fromjson("{a: {$mod: [7, 3]}}");
    auto mod2 = fromjson("{a: {$mod: [7, 3]}}");
    auto mod3 = fromjson("{b: {$mod: [7, 3]}}");
    auto mod4 = fromjson("{a: {$mod: [5, 3]}}");
    auto mod5 = fromjson("{a: {$mod: [7, 5]}}");

    assertEquivalent(mod1, mod2);
    assertNotEquivalent(mod1, mod3);
    assertNotEquivalent(mod1, mod4);
    assertNotEquivalent(mod1, mod5);
}

TEST_F(MatchExpressionHasherTest, Size) {
    auto size1 = fromjson("{a: {$size: 2}}");
    auto size2 = fromjson("{a: {$size: 2}}");
    auto size3 = fromjson("{a: {$size: 1}}");
    auto size4 = fromjson("{ac: {$size: 2}}");

    assertEquivalent(size1, size2);
    assertNotEquivalent(size1, size3);
    assertNotEquivalent(size1, size4);
}

TEST_F(MatchExpressionHasherTest, Type) {
    auto type1 = fromjson("{a: {$type: 'array'}}");
    auto type2 = fromjson("{a: {$type: 'array'}}");  // 4 is array
    auto type3 = fromjson("{a: {$type: 5}}");
    auto type4 = fromjson("{a: {$type: 'date'}}");

    assertEquivalent(type1, type2);
    assertNotEquivalent(type1, type3);
    assertNotEquivalent(type1, type4);
}

TEST_F(MatchExpressionHasherTest, Where) {
    auto where1 = fromjson("{ $where: 'function() { return 1 }' }");
    auto where2 = fromjson("{ $where: 'function() { return 1 }' }");
    auto where3 = fromjson("{ $where: 'function() { return 3 }' }");
    auto where4 = fromjson("{ $where: 'function() {return 1 }' }");

    assertEquivalent(where1, where2);
    assertNotEquivalent(where1, where3);
    // The functions defined in where1 and where2 are different by a space only, but they are still
    // considred as completely different functions.
    assertNotEquivalent(where1, where4);
}

TEST_F(MatchExpressionHasherTest, And) {
    auto and1 = fromjson("{$and: [{a: 5}, {a: 7}]}");
    auto and2 = fromjson("{$and: [{a: 5}, {a: {$eq: 7}}]}");
    auto and3 = fromjson("{$and: [{a: 5}, {a: 8}]}");
    auto and4 = fromjson("{$and: [{a: 5}, {a: 7, b: 2}]}");

    assertEquivalent(and1, and2);
    assertNotEquivalent(and1, and3);
    assertNotEquivalent(and1, and4);
}

TEST_F(MatchExpressionHasherTest, Or) {
    auto or1 = fromjson("{$or: [{a: 5}, {a: 7}]}");
    auto or2 = fromjson("{$or: [{a: 5}, {a: {$eq: 7}}]}");
    auto or3 = fromjson("{$or: [{a: 5}, {a: 8}]}");
    auto or4 = fromjson("{$or: [{a: 5}, {a: 7, b: 2}]}");

    assertEquivalent(or1, or2);
    assertNotEquivalent(or1, or3);
    assertNotEquivalent(or1, or4);
}

TEST_F(MatchExpressionHasherTest, Nor) {
    auto nor1 = fromjson("{$nor: [{a: 5}, {a: 7}]}");
    auto nor2 = fromjson("{$nor: [{a: 5}, {a: {$eq: 7}}]}");
    auto nor3 = fromjson("{$nor: [{a: 5}, {a: 8}]}");
    auto nor4 = fromjson("{$nor: [{a: 5}, {a: 7, b: 2}]}");

    assertEquivalent(nor1, nor2);
    assertNotEquivalent(nor1, nor3);
    assertNotEquivalent(nor1, nor4);
}

TEST_F(MatchExpressionHasherTest, Not) {
    auto not1 = fromjson("{a: {$not: {$eq: 5}}}");
    auto not2 = fromjson("{a: {$not: {$eq: 5}}}");
    auto not3 = fromjson("{a: {$not: {$eq: 1}}}");
    auto not4 = fromjson("{b: {$not: {$eq: 5}}}");

    assertEquivalent(not1, not2);
    assertNotEquivalent(not1, not3);
    assertNotEquivalent(not1, not4);
}

TEST_F(MatchExpressionHasherTest, ElemMatchObject) {
    auto elemMatch1 = fromjson("{a: {$elemMatch: {b: 'hello'}}}");
    auto elemMatch2 = fromjson("{a: {$elemMatch: {b: 'hello'}}}");
    auto elemMatch3 = fromjson("{a: {$elemMatch: {b: 'Hello'}}}");
    auto elemMatch4 = fromjson("{c: {$elemMatch: {b: 'hello'}}}");
    auto elemMatch5 = fromjson("{a: {$elemMatch: {d: 'hello'}}}");

    CollatorInterfaceMock collatorToLowerString{CollatorInterfaceMock::MockType::kToLowerString};

    assertEquivalent(elemMatch1, elemMatch2);
    assertNotEquivalent(elemMatch1, elemMatch3);
    assertNotEquivalent(elemMatch1, elemMatch4);
    assertNotEquivalent(elemMatch1, elemMatch5);

    assertNotEquivalent(elemMatch1, elemMatch2, nullptr, &collatorToLowerString);
    assertEquivalent(elemMatch1, elemMatch3, &collatorToLowerString, &collatorToLowerString);
}

TEST_F(MatchExpressionHasherTest, ElemMatchValue) {
    auto elemMatch1 = fromjson("{a: {$elemMatch: {$eq: 'hello'}}}");
    auto elemMatch2 = fromjson("{a: {$elemMatch: {$eq: 'hello'}}}");
    auto elemMatch3 = fromjson("{a: {$elemMatch: {$eq: 'Hello'}}}");
    auto elemMatch4 = fromjson("{c: {$elemMatch: {$eq: 'hello'}}}");

    CollatorInterfaceMock collatorToLowerString{CollatorInterfaceMock::MockType::kToLowerString};

    assertEquivalent(elemMatch1, elemMatch2);
    assertNotEquivalent(elemMatch1, elemMatch3);
    assertNotEquivalent(elemMatch1, elemMatch4);

    assertNotEquivalent(elemMatch1, elemMatch2, nullptr, &collatorToLowerString);
    assertEquivalent(elemMatch1, elemMatch3, &collatorToLowerString, &collatorToLowerString);
}

TEST_F(MatchExpressionHasherTest, Exists) {
    auto exists1 = fromjson("{a: {$exists: true}}");
    auto exists2 = fromjson("{a: {$exists: 1}}");
    auto exists3 = fromjson("{a: {$exists: false}}");
    auto exists4 = fromjson("{b: {$exists: true}}");

    assertEquivalent(exists1, exists2);
    assertNotEquivalent(exists1, exists3);
    assertNotEquivalent(exists1, exists4);
}

TEST_F(MatchExpressionHasherTest, Expr) {
    auto expr1 = fromjson("{$expr: {$eq: ['$a', '$b']}}");
    auto expr2 = fromjson("{$expr: {$eq: ['$a', '$b']}}");
    auto expr3 = fromjson("{$expr: {$eq: ['$a', '$c']}}");
    auto expr4 = fromjson("{$expr: {$eq: ['$c', '$b']}}");
    auto expr5 = fromjson("{$expr: {$gt: ['$a', '$b']}}");

    assertEquivalent(expr1, expr2);
    assertNotEquivalent(expr1, expr3);
    assertNotEquivalent(expr1, expr4);
    assertNotEquivalent(expr1, expr5);
}

TEST_F(MatchExpressionHasherTest, Geo) {
    auto geo1 = fromjson("{a: {$near: {$geometry: {type: 'Point', coordinates: [10, 20]}}}}");
    auto geo2 = fromjson("{a: {$near: {$geometry: {type: 'Point', coordinates: [10, 20]}}}}");
    auto geo3 = fromjson("{b: {$near: {$geometry: {type: 'Point', coordinates: [10, 20]}}}}");
    auto geo4 = fromjson("{a: {$near: {$geometry: {type: 'Point', coordinates: [10, 22]}}}}");

    assertEquivalent(geo1, geo2);
    assertNotEquivalent(geo1, geo3);
    assertNotEquivalent(geo1, geo4);
}

TEST_F(MatchExpressionHasherTest, Text) {
    auto text1 = fromjson("{$text: {$search: 'hello'}}");
    auto text2 = fromjson("{$text: {$search: 'hello'}}");
    auto text3 = fromjson("{$text: {$search: 'hi'}}");
    auto text4 = fromjson("{$text: {$search: 'hello', $caseSensitive: true}}");

    assertEquivalent(text1, text2);
    assertNotEquivalent(text1, text3);
    assertNotEquivalent(text1, text4);
}
}  // namespace mongo
