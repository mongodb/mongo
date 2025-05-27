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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class ExpressionInternalOwningShard final
    : public ExpressionFixedArity<ExpressionInternalOwningShard, 1> {
public:
    static constexpr const char* const opName = "$_internalOwningShard";

    ExpressionInternalOwningShard(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionInternalOwningShard, 1>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionInternalOwningShard(ExpressionContext* const expCtx,
                                  Expression::ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionInternalOwningShard, 1>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    Value evaluate(const Document& root, Variables* variables) const final;

    const char* getOpName() const final {
        return opName;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

/**
 * The expression '$_internalIndexKey' is used to generate index keys documents for the provided
 * document 'doc' using the index specification 'spec'. The 'doc' field can be an arbitrary
 * expression, including a field path or variable like '$$ROOT'. The 'spec' field is aligned with
 * that of the 'createIndex' command and is treated as a constant expression, as if it had been
 * supplied by the client as '$literal'.
 *
 * The expression specification is a follows:
 * {
 *     $_internalIndexKey: {
 *         doc: <document | expression | field-path | variable>,
 *         spec: <document>
 *     }
 * }
 *
 * Returns: A 'Value' which is an array of 'BSONObj' document, where each document represents the
 * generated index keys object.
 *
 * Note that this expression does not inherit the collation from the collection. A collation must be
 * explicitly provided in the index spec 'spec'.
 *
 * Examples:
 * Case 1: The 'doc' field is a document.
 * Input1:
 * {
 *     $_internalIndexKey: {
 *         doc: {a: 4, b: 5},
 *         spec: {key: {a: 1}, name: "exampleIndex"}
 *     }
 * }
 * Output1: [{a: 4}]
 *
 * Case 2: The 'doc' field is '$$ROOT' and the current document been processed by the pipeline is
 * '{a: 4, b: 5}'.
 * Input2:
 * {
 *     $_internalIndexKey: {
 *         doc: '$$ROOT',
 *         spec: {key: {a: 1}, name: "exampleIndex"}
 *     }
 * }
 * Output2: [{a: 4}]
 *
 * Case 3: The 'doc' field is an expression.
 * Input3:
 * {
 *     $_internalIndexKey: {
 *         doc: {$literal: {a: 4, b: 5}},
 *         spec: {key: {a: 1}, name: "exampleIndex"}
 *     }
 * }
 * Output3: [{a: 4}]
 */
class ExpressionInternalIndexKey final : public Expression {
public:
    static constexpr const char* const opName = "$_internalIndexKey";
    static constexpr auto kIndexSpecKeyField = "key"_sd;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement bsonExpr,
                                                  const VariablesParseState& vps);

    ExpressionInternalIndexKey(ExpressionContext* expCtx,
                               boost::intrusive_ptr<Expression> doc,
                               boost::intrusive_ptr<Expression> spec);

    boost::intrusive_ptr<Expression> optimize() final;

    Value serialize(const SerializationOptions& options) const final;

    Value evaluate(const Document& root, Variables* variables) const final;

    const char* getOpName() const {
        return opName;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const Expression* getDoc() const {
        return _doc.get();
    }

    const Expression* getSpec() const {
        return _spec.get();
    }

private:
    constexpr static auto kDocField = "doc"_sd;
    constexpr static auto kSpecField = "spec"_sd;

    boost::intrusive_ptr<Expression> _doc;
    boost::intrusive_ptr<Expression> _spec;
};

}  // namespace mongo
