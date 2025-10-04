/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>

namespace mongo {

// This class is used by the aggregation framework and streams enterprise module
// to perform the document processing needed for $redact.
class RedactProcessor final {
public:
    RedactProcessor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                    const boost::intrusive_ptr<Expression>& expression,
                    Variables::Id currentId);

    // Processes the given document and returns the redacted document.
    boost::optional<Document> process(const Document& input) const;

    boost::intrusive_ptr<Expression>& getExpression() {
        return _expression;
    }

    const boost::intrusive_ptr<Expression>& getExpression() const {
        return _expression;
    }

    void setExpression(boost::intrusive_ptr<Expression> expression) {
        _expression = std::move(expression);
    }

private:
    // These both work over pExpCtx->variables.
    boost::optional<Document> redactObject(const Document& root) const;  // redacts CURRENT
    Value redactValue(const Value& in, const Document& root) const;

    boost::intrusive_ptr<ExpressionContext> _expCtx;
    boost::intrusive_ptr<Expression> _expression;
    Variables::Id _currentId;
};

}  // namespace mongo
