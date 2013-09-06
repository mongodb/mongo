/**
 * Copyright 2011 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/pch.h"

#include "mongo/db/pipeline/document_source.h"

#include <boost/optional.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

    const char DocumentSourceRedact::redactName[] = "$redact";

    DocumentSourceRedact::DocumentSourceRedact(const intrusive_ptr<ExpressionContext>& expCtx,
                                               const intrusive_ptr<Expression>& expression)
        : DocumentSource(expCtx)
        , _expression(expression)
    { }

    const char *DocumentSourceRedact::getSourceName() const {
        return redactName;
    }

    static const Value descendVal = Value("descend");
    static const Value pruneVal = Value("prune");
    static const Value keepVal = Value("keep");

    boost::optional<Document> DocumentSourceRedact::getNext() {
        while (boost::optional<Document> in = pSource->getNext()) {
            Variables vars = Variables(*in,
                                       Value(*in),
                                       DOC("DESCEND" << descendVal
                                        << "PRUNE" << pruneVal
                                        << "KEEP" << keepVal));

            if (boost::optional<Document> result = redactObject(vars)) {
                return result;
            }
        }

        return boost::none;
    }

    Value DocumentSourceRedact::redactValue(const Variables& vars, const Value& in) {
        const BSONType valueType = in.getType();
        if (valueType == Object) {
            Variables recurse = vars;
            recurse.current = in;
            const boost::optional<Document> result = redactObject(recurse);
            if (result) {
                return Value(*result);
            }
            else {
                return Value();
            }
        }
        else if (valueType == Array) {
            // TODO dont copy if possible
            vector<Value> newArr;
            const vector<Value>& arr = in.getArray();
            for (size_t i = 0; i < arr.size(); i++) {
                if (arr[i].getType() == Object || arr[i].getType() == Array) {
                    const Value toAdd = redactValue(vars, arr[i]) ;
                    if (!toAdd.missing()) {
                        newArr.push_back(toAdd);
                    }
                }
                else {
                    newArr.push_back(arr[i]);
                }
            }
            return Value::consume(newArr);
        }
        else {
            return in;
        }
    }

    boost::optional<Document> DocumentSourceRedact::redactObject(const Variables& in) {
        const Value expressionResult = _expression->evaluate(in);

        if (expressionResult == keepVal) {
            return in.current.getDocument();
        }
        else if (expressionResult == pruneVal) {
            return boost::optional<Document>();
        }
        else if (expressionResult == descendVal) {
            MutableDocument out;
            FieldIterator fields(in.current.getDocument());
            while (fields.more()) {
                const Document::FieldPair field(fields.next());
                const Value val = redactValue(in, field.second);
                if (!val.missing()) {
                    out.addField(field.first, val);
                }
            }
            return out.freeze();
        }
        else {
            uasserted(17053, str::stream() << "$redact's expression should not return anything "
                                           << "aside from the variables $$KEEP, $$DESCEND, and "
                                           << "$$PRUNE, but returned "
                                           << expressionResult.toString());
        }
    }

    void DocumentSourceRedact::optimize() {
        _expression = _expression->optimize();
    }

    Value DocumentSourceRedact::serialize(bool explain) const {
        return Value(DOC(getSourceName() << _expression.get()->serialize()));
    }

    intrusive_ptr<DocumentSource> DocumentSourceRedact::createFromBson(
            BSONElement* bsonElement,
            const intrusive_ptr<ExpressionContext>& expCtx) {
        uassert(17054, str::stream() << redactName << " specification must be an object",
                bsonElement->type() == Object);

        Expression::ObjectCtx oCtx(0);

        intrusive_ptr<Expression> expression = Expression::parseObject(bsonElement, &oCtx);

        return new DocumentSourceRedact(expCtx, expression);
    }
}
