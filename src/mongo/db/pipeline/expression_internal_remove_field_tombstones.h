/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression.h"

namespace mongo {
/**
 * An internal expression used to remove 'tombstone' values, that is, missing values which are
 * used as placeholders to retain the position of removed fields.
 *
 * Note that this expression does not traverse arrays. For instance, given the document:
 *
 * {a: [{b: 1, c: <TOMBSTONE>}]}
 *
 * this expression will not remove the tombstone for 'c'.
 */
class ExpressionInternalRemoveFieldTombstones final : public Expression {
public:
    explicit ExpressionInternalRemoveFieldTombstones(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::intrusive_ptr<Expression> child)
        : Expression{expCtx, {std::move(child)}} {}

    Value evaluate(const Document& root, Variables* variables) const final {
        auto targetVal = _children[0]->evaluate(root, variables);
        uassert(4750600,
                str::stream() << "$_internalRemoveFieldTombstones requires a document "
                                 "input, found: "
                              << typeName(targetVal.getType()),
                targetVal.getType() == BSONType::Object);
        return _removeTombstones(targetVal.getDocument());
    }

    Value serialize(bool explain) const final {
        return Value();
    }

    boost::intrusive_ptr<Expression> optimize() final {
        invariant(_children.size() == 1);
        _children[0] = _children[0]->optimize();
        return this;
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final {
        invariant(_children.size() == 1);
        _children[0]->addDependencies(deps);
    }

private:
    Value _removeTombstones(const Document& document) const {
        MutableDocument output;
        FieldIterator iter = document.fieldIterator();
        while (iter.more()) {
            Document::FieldPair pair = iter.next();
            auto val = pair.second;
            if (val.getType() == BSONType::Object)
                val = _removeTombstones(val.getDocument());
            output.addField(pair.first, val);
        }
        return Value(output.freeze());
    }
};
}  // namespace mongo