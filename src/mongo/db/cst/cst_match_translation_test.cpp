/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_match_translation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

auto getExpCtx() {
    auto nss = NamespaceString{"db", "coll"};
    return boost::intrusive_ptr<ExpressionContextForTest>{new ExpressionContextForTest(nss)};
}

TEST(CstMatchTranslationTest, TranslatesEmpty) {
    const auto cst = CNode{CNode::ObjectChildren{}};
    auto match = cst_match_translation::translateMatchExpression(cst, getExpCtx());
    auto andExpr = dynamic_cast<AndMatchExpression*>(match.get());
    ASSERT(andExpr);
    ASSERT_EQ(0, andExpr->numChildren());
}

TEST(CstMatchTranslationTest, TranslatesSinglePredicate) {
    const auto cst = CNode{CNode::ObjectChildren{{UserFieldname{"a"}, CNode{UserInt{1}}}}};
    auto match = cst_match_translation::translateMatchExpression(cst, getExpCtx());
    ASSERT_BSONOBJ_EQ(match->serialize(), fromjson("{$and: [{a: {$eq: 1}}]}"));
}

TEST(CstMatchTranslationTest, TranslatesMultipleEqualityPredicates) {
    const auto cst = CNode{CNode::ObjectChildren{
        {UserFieldname{"a"}, CNode{UserInt{1}}},
        {UserFieldname{"b"}, CNode{UserNull{}}},
    }};
    auto match = cst_match_translation::translateMatchExpression(cst, getExpCtx());
    ASSERT_BSONOBJ_EQ(match->serialize(), fromjson("{$and: [{a: {$eq: 1}}, {b: {$eq: null}}]}"));
}

TEST(CstMatchTranslationTest, TranslatesEqualityPredicatesWithId) {
    const auto cst = CNode{CNode::ObjectChildren{
        {UserFieldname{"_id"}, CNode{UserNull{}}},
    }};
    auto match = cst_match_translation::translateMatchExpression(cst, getExpCtx());
    auto andExpr = dynamic_cast<AndMatchExpression*>(match.get());
    ASSERT(andExpr);
    ASSERT_EQ(1, andExpr->numChildren());
    ASSERT_BSONOBJ_EQ(match->serialize(), fromjson("{$and: [{_id: {$eq: null}}]}"));
}

}  // namespace
}  // namespace mongo
