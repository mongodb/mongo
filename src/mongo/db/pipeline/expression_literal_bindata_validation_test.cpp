/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/data_view.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <array>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class ExpressionLiteralSubtypeTest : public AggregationContextFixture {
public:
    void assertLiteralRejects(const BSONBinData& binData) {
        auto expCtx = getExpCtx();
        BSONObj spec = BSON("$literal" << Value(binData));
        ASSERT_THROWS_CODE(
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
            AssertionException,
            ErrorCodes::FailedToParse);
    }

    void assertLiteralWorks(const BSONBinData& binData) {
        auto expCtx = getExpCtx();
        BSONObj spec = BSON("$literal" << Value(binData));
        ASSERT_DOES_NOT_THROW(
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState));
    }
};

TEST_F(ExpressionLiteralSubtypeTest, BsonColumnThrowsWhenUsedAsLiteral) {
    assertLiteralRejects({"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, Column});
}

TEST_F(ExpressionLiteralSubtypeTest, ByteArrayDeprecatedThrowsWithBadInnerPrefix) {
    std::array<char, 8> badBuf{};
    DataView(badBuf.data()).write<LittleEndian<int32_t>>(99);  // 99 != 8 - 4 = 4
    assertLiteralRejects({badBuf.data(), 8, ByteArrayDeprecated});
}

TEST_F(ExpressionLiteralSubtypeTest, ByteArrayDeprecatedWorksWithValidInnerPrefix) {
    std::array<char, 16> buf{};
    DataView(buf.data()).write<LittleEndian<int32_t>>(12);  // 12 == 16 - 4
    assertLiteralWorks({buf.data(), 16, ByteArrayDeprecated});
}

TEST_F(ExpressionLiteralSubtypeTest, BdtUUIDThrowsWithWrongSize) {
    assertLiteralRejects({"AAAA", 4, bdtUUID});
}

TEST_F(ExpressionLiteralSubtypeTest, MD5TypeThrowsWithWrongSize) {
    assertLiteralRejects({"AAAA", 4, MD5Type});
}

}  // namespace mongo
