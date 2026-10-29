// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/redact_processor.h"

#include "mongo/db/pipeline/expression.h"

namespace mongo {
using namespace std::literals::string_view_literals;

using boost::intrusive_ptr;

static const Value descendVal = Value("descend"sv);
static const Value pruneVal = Value("prune"sv);
static const Value keepVal = Value("keep"sv);

RedactProcessor::RedactProcessor(const intrusive_ptr<ExpressionContext>& expCtx,
                                 const intrusive_ptr<Expression>& expression,
                                 Variables::Id currentId)
    : _expCtx(expCtx), _expression(expression), _currentId(currentId) {}

boost::optional<Document> RedactProcessor::process(const Document& input,
                                                   const EvaluationContext& ctx) const {
    auto& variables = _expCtx->variables;
    variables.setValue(_currentId, Value(input));
    return redactObject(input, ctx);
}

Value RedactProcessor::redactValue(const Value& in,
                                   const Document& root,
                                   const EvaluationContext& ctx) const {
    const BSONType valueType = in.getType();
    if (valueType == BSONType::object) {
        _expCtx->variables.setValue(_currentId, in);
        const boost::optional<Document> result = redactObject(root, ctx);
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
                const Value toAdd = redactValue(arr[i], root, ctx);
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

boost::optional<Document> RedactProcessor::redactObject(const Document& root,
                                                        const EvaluationContext& ctx) const {
    auto& variables = _expCtx->variables;
    const Value expressionResult = _expression->evaluate(root, &variables, ctx);

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
            Value val = redactValue(field.second, root, ctx);
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
