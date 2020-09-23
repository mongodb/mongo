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

#include "mongo/db/matcher/expression.h"

namespace mongo {

/**
 * A PathMatchExpression is an expression that acts on a field path with syntax
 * like "path.to.something": {$operator: ...}. Many such expressions are leaves in
 * the AST, such as $gt, $mod, $exists, and so on. But expressions that are not
 * leaves, such as $_internalSchemaObjectMatch, may also match against a field
 * path.
 */
class PathMatchExpression : public MatchExpression {
public:
    PathMatchExpression(MatchType matchType,
                        StringData path,
                        ElementPath::LeafArrayBehavior leafArrBehavior,
                        ElementPath::NonLeafArrayBehavior nonLeafArrayBehavior,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(matchType, std::move(annotation)),
          _elementPath(path, leafArrBehavior, nonLeafArrayBehavior) {}

    virtual ~PathMatchExpression() {}

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final {
        MatchableDocument::IteratorHolder cursor(doc, &_elementPath);
        while (cursor->more()) {
            ElementIterator::Context e = cursor->next();
            if (!matchesSingleElement(e.element(), details)) {
                continue;
            }
            if (details && details->needRecord() && !e.arrayOffset().eoo()) {
                details->setElemMatchKey(e.arrayOffset().fieldName());
            }
            return true;
        }
        return false;
    }

    const StringData path() const final {
        return _elementPath.fieldRef().dottedField();
    }

    /**
     * Resets the path for this expression. Note that this method will make a copy of 'path' such
     * that there's no lifetime requirements for the string which 'path' points into.
     */
    void setPath(StringData path) {
        _elementPath.reset(path);
    }

    /**
     * Finds an applicable rename from 'renameList' (if one exists) and applies it to the expression
     * path. Each pair in 'renameList' specifies a path prefix that should be renamed (as the first
     * element) and the path components that should replace the renamed prefix (as the second
     * element).
     */
    void applyRename(const StringMap<std::string>& renameList) {
        size_t renamesFound = 0u;
        std::string rewrittenPath;
        for (auto rename : renameList) {
            if (rename.first == path()) {
                rewrittenPath = rename.second;

                ++renamesFound;
            }

            FieldRef prefixToRename(rename.first);
            const auto& pathFieldRef = _elementPath.fieldRef();
            if (prefixToRename.isPrefixOf(pathFieldRef)) {
                // Get the 'pathTail' by chopping off the 'prefixToRename' path components from the
                // beginning of the 'pathFieldRef' path.
                auto pathTail = pathFieldRef.dottedSubstring(prefixToRename.numParts(),
                                                             pathFieldRef.numParts());
                // Replace the chopped off components with the component names resulting from the
                // rename.
                rewrittenPath = str::stream() << rename.second << "." << pathTail.toString();

                ++renamesFound;
            }
        }

        // There should never be multiple applicable renames.
        invariant(renamesFound <= 1u);
        if (renamesFound == 1u) {
            // There is an applicable rename. Modify the path of this expression to use the new
            // name.
            setPath(rewrittenPath);
        }
    }

    void serialize(BSONObjBuilder* out, bool includePath) const override {
        if (includePath) {
            out->append(path(), getSerializedRightHandSide());
        } else {
            out->appendElements(getSerializedRightHandSide());
        }
    }

    /**
     * Returns a BSONObj that represents the right-hand-side of a PathMatchExpression. Used for
     * serialization of PathMatchExpression in cases where we do not want to serialize the path in
     * line with the expression. For example {x: {$not: {$eq: 1}}}, where $eq is the
     * PathMatchExpression.
     */
    virtual BSONObj getSerializedRightHandSide() const = 0;

protected:
    void _doAddDependencies(DepsTracker* deps) const final {
        if (!path().empty()) {
            deps->fields.insert(path().toString());
        }
    }

private:
    // ElementPath holds a FieldRef, which owns the underlying path string.
    ElementPath _elementPath;
};
}  // namespace mongo
