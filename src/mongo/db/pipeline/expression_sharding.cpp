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

#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/expression/evaluate_sharding.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_sharding.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

namespace mongo {

Value ExpressionInternalOwningShard::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

boost::intrusive_ptr<Expression> ExpressionInternalIndexKey::parse(ExpressionContext* expCtx,
                                                                   BSONElement bsonExpr,
                                                                   const VariablesParseState& vps) {
    uassert(6868506,
            str::stream() << opName << " supports an object as its argument",
            bsonExpr.type() == BSONType::object);

    BSONElement docElement;
    BSONElement specElement;

    for (auto&& bsonArgs : bsonExpr.embeddedObject()) {
        if (bsonArgs.fieldNameStringData() == kDocField) {
            docElement = bsonArgs;
        } else if (bsonArgs.fieldNameStringData() == kSpecField) {
            uassert(6868507,
                    str::stream() << opName << " requires 'spec' argument to be an object",
                    bsonArgs.type() == BSONType::object);
            specElement = bsonArgs;
        } else {
            uasserted(6868508,
                      str::stream() << "Unknown argument: " << bsonArgs.fieldNameStringData()
                                    << "found while parsing" << opName);
        }
    }

    uassert(6868509,
            str::stream() << opName << " requires both 'doc' and 'spec' arguments",
            !docElement.eoo() && !specElement.eoo());

    return new ExpressionInternalIndexKey(expCtx,
                                          parseOperand(expCtx, docElement, vps),
                                          ExpressionConstant::create(expCtx, Value{specElement}));
}

ExpressionInternalIndexKey::ExpressionInternalIndexKey(ExpressionContext* expCtx,
                                                       boost::intrusive_ptr<Expression> doc,
                                                       boost::intrusive_ptr<Expression> spec)
    : Expression(expCtx, {std::move(doc), std::move(spec)}),
      _doc(_children[0]),
      _spec(_children[1]) {
    expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
}

boost::intrusive_ptr<Expression> ExpressionInternalIndexKey::optimize() {
    invariant(_doc);
    invariant(_spec);

    _doc = _doc->optimize();
    _spec = _spec->optimize();
    return this;
}

Value ExpressionInternalIndexKey::serialize(const SerializationOptions& options) const {
    invariant(_doc);
    invariant(_spec);

    auto specExprConstant = dynamic_cast<ExpressionConstant*>(_spec.get());
    tassert(7250400, "Failed to dynamic cast the 'spec' to 'ExpressionConstant'", specExprConstant);

    // The 'spec' is always treated as a constant so do not call '_spec->serialize()' which would
    // wrap the value in an unnecessary '$const' object.
    return Value(DOC(opName << DOC(kDocField << _doc->serialize(options) << kSpecField
                                             << specExprConstant->getValue())));
}

Value ExpressionInternalIndexKey::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

REGISTER_STABLE_EXPRESSION(_internalOwningShard, ExpressionInternalOwningShard::parse);
REGISTER_STABLE_EXPRESSION(_internalIndexKey, ExpressionInternalIndexKey::parse);

}  // namespace mongo
