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

#include "mongo/db/pipeline/redact_processor.h"

#include "mongo/db/pipeline/expression.h"

namespace mongo {

using boost::intrusive_ptr;

static const Value descendVal = Value("descend"_sd);
static const Value pruneVal = Value("prune"_sd);
static const Value keepVal = Value("keep"_sd);

RedactProcessor::RedactProcessor(const intrusive_ptr<ExpressionContext>& expCtx,
                                 const intrusive_ptr<Expression>& expression,
                                 Variables::Id currentId)
    : _expCtx(expCtx), _expression(expression), _currentId(currentId) {}

boost::optional<Document> RedactProcessor::process(const Document& input) const {
    auto& variables = _expCtx->variables;
    variables.setValue(_currentId, Value(input));
    return redactObject(input);
}

Value RedactProcessor::redactValue(const Value& in, const Document& root) const {
    const BSONType valueType = in.getType();
    if (valueType == BSONType::object) {
        _expCtx->variables.setValue(_currentId, in);
        const boost::optional<Document> result = redactObject(root);
        if (result) {
            return Value(*result);
        } else {
            return Value();
        }
    } else if (valueType == BSONType::array) {
        // TODO dont copy if possible
        std::vector<Value> newArr;
        const std::vector<Value>& arr = in.getArray();
        for (size_t i = 0; i < arr.size(); i++) {
            if (arr[i].getType() == BSONType::object || arr[i].getType() == BSONType::array) {
                const Value toAdd = redactValue(arr[i], root);
                if (!toAdd.missing()) {
                    newArr.push_back(toAdd);
                }
            } else {
                newArr.push_back(arr[i]);
            }
        }
        return Value(std::move(newArr));
    } else {
        return in;
    }
}

boost::optional<Document> RedactProcessor::redactObject(const Document& root) const {
    auto& variables = _expCtx->variables;
    const Value expressionResult = _expression->evaluate(root, &variables);

    ValueComparator simpleValueCmp;
    if (simpleValueCmp.evaluate(expressionResult == keepVal)) {
        return variables.getDocument(_currentId, root);
    } else if (simpleValueCmp.evaluate(expressionResult == pruneVal)) {
        return boost::optional<Document>();
    } else if (simpleValueCmp.evaluate(expressionResult == descendVal)) {
        const Document in = variables.getDocument(_currentId, root);
        in.loadIntoCache();

        MutableDocument out;
        out.copyMetaDataFrom(in);

        FieldIterator fields(in);
        while (fields.more()) {
            const Document::FieldPair field(fields.next());

            // This changes CURRENT so don't read from variables after this
            Value val = redactValue(field.second, root);
            if (!val.missing()) {
                out.addField(field.first, std::move(val));
            }
        }
        return out.freeze();
    } else {
        uasserted(17053,
                  str::stream() << "$redact's expression should not return anything "
                                << "aside from the variables $$KEEP, $$DESCEND, and "
                                << "$$PRUNE, but returned " << expressionResult.toString());
    }
}

}  // namespace mongo
