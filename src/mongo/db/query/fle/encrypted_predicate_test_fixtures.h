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

#pragma once

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/equality_predicate.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/db/query/fle/range_predicate.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::fle {
using TagMap = std::map<std::pair<StringData, int>, std::vector<PrfBlock>>;

/*
 * The MockServerRewrite allows unit testing individual predicate rewrites without going through the
 * real server rewrite that traverses full expression trees.
 */
class MockServerRewrite : public QueryRewriterInterface {
public:
    MockServerRewrite() : _expCtx((new ExpressionContextForTest())) {}
    const FLEStateCollectionReader* getEscReader() const override {
        return nullptr;
    }
    const FLEStateCollectionReader* getEccReader() const override {
        return nullptr;
    }
    EncryptedCollScanMode getEncryptedCollScanMode() const override {
        return _mode;
    };
    ExpressionContext* getExpressionContext() const {
        return _expCtx.get();
    }

    void setForceEncryptedCollScanForTest() {
        _mode = EncryptedCollScanMode::kForceAlways;
    }

private:
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
    EncryptedCollScanMode _mode{EncryptedCollScanMode::kUseIfNeeded};
};

class EncryptedPredicateRewriteTest : public unittest::Test {
public:
    EncryptedPredicateRewriteTest() {}

    void setUp() override {}

    void tearDown() override {}

    static std::unique_ptr<MatchExpression> makeInExpr(StringData fieldname,
                                                       BSONArray disjunctions) {
        auto inExpr = std::make_unique<InMatchExpression>(fieldname);
        std::vector<BSONElement> elems;
        disjunctions.elems(elems);
        uassertStatusOK(inExpr->setEqualities(elems));
        inExpr->setBackingBSON(std::move(disjunctions));
        return inExpr;
    }

    /*
     * Assertion helper for tag disjunction rewrite.
     */
    void assertRewriteToTags(const EncryptedPredicate& pred,
                             MatchExpression* input,
                             BSONArray expectedTags) {

        auto actual = pred.rewrite(input);
        auto expected = makeInExpr(kSafeContent, expectedTags);
        ASSERT_BSONOBJ_EQ(actual->serialize(),
                          static_cast<MatchExpression*>(expected.get())->serialize());
    }

protected:
    MockServerRewrite _mock{};
};
}  // namespace mongo::fle
