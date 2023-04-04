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

#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/namespace_string.h"
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
    MockServerRewrite() : _expCtx((new ExpressionContextForTest())) {
        _mockOptionalNss = boost::none;
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

    FLETagQueryInterface* getTagQueryInterface() const override {
        return nullptr;
    };
    const NamespaceString& getESCNss() const override {
        return _mockNss;
    }


private:
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
    EncryptedCollScanMode _mode{EncryptedCollScanMode::kUseIfNeeded};
    NamespaceString _mockNss{"mock"_sd};
    boost::optional<NamespaceString> _mockOptionalNss;
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

    static std::unique_ptr<MatchExpression> makeElemMatchWithIn(StringData fieldname,
                                                                BSONArray disjunctions) {
        auto elemMatchExpr = std::make_unique<ElemMatchValueMatchExpression>(fieldname);
        elemMatchExpr->add(makeInExpr(fieldname, disjunctions));
        return elemMatchExpr;
    }
    /*
     * Assertion helper for tag disjunction rewrite.
     */
    void assertRewriteToTags(const EncryptedPredicate& pred,
                             MatchExpression* input,
                             BSONArray expectedTags) {

        auto actual = pred.rewrite(input);
        auto expected = makeElemMatchWithIn(kSafeContent, expectedTags);
        ASSERT_BSONOBJ_EQ(actual->serialize(),
                          static_cast<MatchExpression*>(expected.get())->serialize());
    }

    template <typename T>
    void assertRewriteForOp(const EncryptedPredicate& pred,
                            BSONElement rhs,
                            std::vector<PrfBlock> allTags) {
        auto inputExpr = T("age"_sd, rhs);
        assertRewriteToTags(pred, &inputExpr, toBSONArray(std::move(allTags)));
    }

protected:
    MockServerRewrite _mock{};
    ExpressionContextForTest _expCtx;
};

// Helper functions for creating encrypted BinData blobs.
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

// Sample encryption keys for creating mock encrypted payloads.
constexpr auto kIndexKeyId = "12345678-1234-9876-1234-123456789012"_sd;
constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID indexKeyId = uassertStatusOK(UUID::parse(kIndexKeyId.toString()));
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId.toString()));

inline const FLEIndexKey& getIndexKey() {
    static std::string indexVec = hexblob::decode(
        "7dbfebc619aa68a659f64b8e23ccd21644ac326cb74a26840c3d2420176c40ae088294d00ad6cae9684237b21b754cf503f085c25cd320bf035c3417416e1e6fe3d9219f79586582112740b2add88e1030d91926ae8afc13ee575cfb8bb965b7"_sd);
    static FLEIndexKey indexKey(KeyMaterial(indexVec.begin(), indexVec.end()));
    return indexKey;
}

inline const FLEUserKey& getUserKey() {
    static std::string userVec = hexblob::decode(
        "a7ddbc4c8be00d51f68d9d8e485f351c8edc8d2206b24d8e0e1816d005fbe520e489125047d647b0d8684bfbdbf09c304085ed086aba6c2b2b1677ccc91ced8847a733bf5e5682c84b3ee7969e4a5fe0e0c21e5e3ee190595a55f83147d8de2a"_sd);
    static FLEUserKey userKey(KeyMaterial(userVec.begin(), userVec.end()));
    return userKey;
}
}  // namespace mongo::fle
