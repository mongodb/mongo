// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

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
