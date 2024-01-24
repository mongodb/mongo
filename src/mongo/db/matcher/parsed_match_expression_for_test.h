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

#pragma once

#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

/**
 * A MatchExpression may store BSONElements as arguments for expressions, to avoid copying large
 * values. A BSONElement is essentially a pointer into a BSONObj, so use
 * ParsedMatchExpressionForTest to ensure that the BSONObj outlives the MatchExpression, and the
 * BSONElement arguments remain pointing to allocated memory.
 */
class ParsedMatchExpressionForTest {
public:
    ParsedMatchExpressionForTest(const std::string& str,
                                 const CollatorInterface* collator = nullptr)
        : _obj(fromjson(str)) {
        _expCtx = make_intrusive<ExpressionContextForTest>();
        _expCtx->setCollator(CollatorInterface::cloneCollator(collator));
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(_obj,
                                         _expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kDefaultSpecialFeatures |
                                             MatchExpressionParser::AllowedFeatures::kJavascript);
        ASSERT_OK(result.getStatus());
        _expr = std::move(result.getValue());
    }

    const MatchExpression* get() const {
        return _expr.get();
    }

    /**
     * Relinquishes ownership of the parsed expression and returns it as a unique_ptr to the caller.
     * This 'ParsedMatchExpressionForTest' object still must outlive the returned value so that the
     * BSONObj used to create it remains alive.
     */
    std::unique_ptr<MatchExpression> release() {
        return std::move(_expr);
    }


private:
    const BSONObj _obj;
    std::unique_ptr<MatchExpression> _expr;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};
}  // namespace mongo
