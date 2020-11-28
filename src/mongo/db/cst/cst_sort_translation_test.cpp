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

#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_pipeline_translation.h"
#include "mongo/db/cst/cst_sort_translation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

auto getExpCtx() {
    auto nss = NamespaceString{"db", "coll"};
    return boost::intrusive_ptr<ExpressionContextForTest>{new ExpressionContextForTest(nss)};
}

void assertSortPatternsEQ(SortPattern correct, SortPattern fromTest) {
    for (size_t i = 0; i < correct.size(); ++i) {
        ASSERT_EQ(correct[i].isAscending, fromTest[i].isAscending);
        if (correct[i].fieldPath) {
            if (fromTest[i].fieldPath) {
                ASSERT_EQ(correct[i].fieldPath->fullPath(), fromTest[i].fieldPath->fullPath());
            } else {
                FAIL("Pattern missing fieldpath");
            }
        } else if (fromTest[i].fieldPath) {
            FAIL("Pattern incorrectly had fieldpath");
        }
        if (correct[i].expression) {
            if (fromTest[i].expression)
                ASSERT_EQ(correct[i].expression->serialize(false).toString(),
                          fromTest[i].expression->serialize(false).toString());
            else {
                FAIL("Pattern missing expression");
            }
        } else if (fromTest[i].expression) {
            FAIL("Pattern incorrectly had expression");
        }
    }
}

TEST(CstSortTranslationTest, BasicSortGeneratesCorrectSortPattern) {
    const auto cst = CNode{CNode::ObjectChildren{
        {SortPath{makeVector<std::string>("val")}, CNode{KeyValue::intOneKey}}}};
    auto expCtx = getExpCtx();
    auto pattern = cst_sort_translation::translateSortSpec(cst, expCtx);
    auto correctPattern = SortPattern(fromjson("{val: 1}"), expCtx);
    assertSortPatternsEQ(correctPattern, pattern);
}

TEST(CstSortTranslationTest, MultiplePartSortGeneratesCorrectSortPattern) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {SortPath{makeVector<std::string>("val")}, CNode{KeyValue::intOneKey}},
            {SortPath{makeVector<std::string>("test")}, CNode{KeyValue::intNegOneKey}}}};
        auto expCtx = getExpCtx();
        auto pattern = cst_sort_translation::translateSortSpec(cst, expCtx);
        auto correctPattern = SortPattern(fromjson("{val: 1, test: -1}"), expCtx);
        assertSortPatternsEQ(correctPattern, pattern);
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {SortPath{makeVector<std::string>("val")}, CNode{KeyValue::doubleOneKey}},
            {SortPath{makeVector<std::string>("test")}, CNode{KeyValue::intNegOneKey}},
            {SortPath{makeVector<std::string>("third")}, CNode{KeyValue::longNegOneKey}}}};
        auto expCtx = getExpCtx();
        auto pattern = cst_sort_translation::translateSortSpec(cst, expCtx);
        auto correctPattern = SortPattern(fromjson("{val: 1, test: -1, third: -1}"), expCtx);
        assertSortPatternsEQ(correctPattern, pattern);
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {SortPath{makeVector<std::string>("val")}, CNode{KeyValue::intOneKey}},
            {SortPath{makeVector<std::string>("test")},
             CNode{
                 CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}},
             }}}};
        auto expCtx = getExpCtx();
        auto pattern = cst_sort_translation::translateSortSpec(cst, expCtx);
        auto correctPattern = SortPattern(fromjson("{val: 1, test: {$meta: \"randVal\"}}"), expCtx);
        assertSortPatternsEQ(correctPattern, pattern);
    }
}

TEST(CstSortTranslationTest, SortWithMetaGeneratesCorrectSortPattern) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {SortPath{makeVector<std::string>("val")},
             CNode{
                 CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}},
             }}}};
        auto expCtx = getExpCtx();
        auto pattern = cst_sort_translation::translateSortSpec(cst, expCtx);
        auto correctPattern = SortPattern(fromjson("{val: {$meta: \"randVal\"}}"), expCtx);
        assertSortPatternsEQ(correctPattern, pattern);
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {SortPath{makeVector<std::string>("val")},
             CNode{
                 CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::textScore}}},
             }}}};
        auto expCtx = getExpCtx();
        auto pattern = cst_sort_translation::translateSortSpec(cst, expCtx);
        auto correctPattern = SortPattern(fromjson("{val: {$meta: \"textScore\"}}"), expCtx);
        assertSortPatternsEQ(correctPattern, pattern);
    }
}

TEST(CstSortTranslationTest, SortWithDottedPathTranslatesCorrectly) {
    const auto cst =
        CNode{CNode::ObjectChildren{{SortPath{{"a", "b", "c"}}, CNode{KeyValue::intOneKey}}}};
    auto expCtx = getExpCtx();
    auto pattern = cst_sort_translation::translateSortSpec(cst, expCtx);
    auto correctPattern = SortPattern(fromjson("{'a.b.c': 1}"), expCtx);
    assertSortPatternsEQ(correctPattern, pattern);
}

}  // namespace
}  // namespace mongo
