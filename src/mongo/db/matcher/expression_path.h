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

#include "mongo/db/field_ref.h"
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
                        boost::optional<StringData> path,
                        ElementPath::LeafArrayBehavior leafArrBehavior,
                        ElementPath::NonLeafArrayBehavior nonLeafArrayBehavior,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(matchType, std::move(annotation)),
          _elementPath(path ? boost::optional<ElementPath>(
                                  ElementPath(*path, leafArrBehavior, nonLeafArrayBehavior))
                            : boost::none) {}

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const override {
        invariant(_elementPath);
        MatchableDocument::IteratorHolder cursor(doc, &*_elementPath);
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

    /**
     * Gets the path that the expression applies to. Note that this returns an empty string for
     * empty path as well as no path cases. optPath() should be preferred in order to
     * distinguish between the two.
     */
    StringData path() const override final {
        return _elementPath ? _elementPath->fieldRef().dottedField() : "";
    }

    const FieldRef* fieldRef() const override final {
        return _elementPath ? &(_elementPath->fieldRef()) : nullptr;
    }

    /**
     * Gets the path that the expression applies to. If the expression does not apply to a specific
     * path, returns boost::none.
     */
    boost::optional<StringData> optPath() const {
        return _elementPath ? boost::optional<StringData>(path()) : boost::none;
    }

    /**
     * Resets the path for this expression. Note that this method will make a copy of 'path' such
     * that there's no lifetime requirements for the string which 'path' points into.
     */
    void setPath(StringData path) {
        invariant(_elementPath);
        _elementPath->reset(path);
    }

    /**
     * Finds an applicable rename from 'renameList' (if one exists) and applies it to the expression
     * path. Each pair in 'renameList' specifies a path prefix that should be renamed (as the first
     * element) and the path components that should replace the renamed prefix (as the second
     * element).
     *
     * Returns whether renames will always succeed if any rename is applicable. See
     * wouldRenameSucceed() for more details.
     *
     * TODO SERVER-74298 As soon as we implement SERVER-74298, the return value might not be
     * necessary any more.
     */
    [[nodiscard]] bool applyRename(const StringMap<std::string>& renameList) {
        if (!_elementPath || renameList.size() == 0) {
            return true;
        }

        if (auto&& [isRenamable, optRewrittenPath] = wouldRenameSucceed(renameList); !isRenamable) {
            return false;
        } else if (optRewrittenPath) {
            setPath(*optRewrittenPath);
        }

        return true;
    }

    /**
     * Returns a pair of bool and boost::optional<StringData>.
     *
     * - The bool indicates whether renames will always succeed if any rename is applicable. No
     *   applicable renames is considered as a successful rename and returns true with the second
     *   element of the pair is boost::none. This function can return false when a renamed path
     *   component descends into an $elemMatch or an object literal. For examples,
     *
     *   expr = {x: {$eq: {y: 3}}} and renames = {{"x.y", "a.b"}}. We should be able to rename 'x'
     *   and 'y' to 'a' and 'b' respectively but due to the current limitation of the algorithm, we
     *   cannot rename such match expressions.
     *
     *   Another similar example is expr = {x: {$elemMatch: {$eq: {y: 3}}}} and renames = {{"x.y",
     *   "a.b"}}.

     * - The boost::optional<StringData> is the rewritten path iff one rename is applicable. The
     *   rewritten path is the path after applying the only applicable rename in 'renameList'. If no
     *   rename is applicable, the rewritten path is boost::none.
     *
     * TODO SERVER-74298 As soon as we implement SERVER-74298, this separate function may not be
     * necessary any more and can be combined into applyRenames().
     */
    std::pair<bool, boost::optional<std::string>> wouldRenameSucceed(
        const StringMap<std::string>& renameList) const {
        invariant(_elementPath);

        size_t renamesFound = 0u;
        std::string rewrittenPath;
        for (const auto& rename : renameList) {
            if (rename.first == path()) {
                rewrittenPath = rename.second;

                ++renamesFound;
            }

            FieldRef prefixToRename(rename.first);
            const auto& pathFieldRef = _elementPath->fieldRef();
            if (prefixToRename.isPrefixOf(pathFieldRef)) {
                // Get the 'pathTail' by chopping off the 'prefixToRename' path components from the
                // beginning of the 'pathFieldRef' path.
                auto pathTail = pathFieldRef.dottedSubstring(prefixToRename.numParts(),
                                                             pathFieldRef.numParts());
                // Replace the chopped off components with the component names resulting from the
                // rename.
                rewrittenPath = str::stream() << rename.second << "." << pathTail.toString();

                ++renamesFound;
            } else if (pathFieldRef.isPrefixOf(prefixToRename)) {
                // TODO SERVER-74298 Implement renaming by each path component instead of returning
                // the pair of 'false' and boost::none. We can traverse subexpressions with the
                // remaining path suffix of 'prefixToRename' to see if we can rename each path
                // component. Any subexpression would succeed with 'rewrittenPath' then this path
                // component can be renamed. For example, assuming that 'pathFieldRef' == "a.b" and
                // 'prefixToRename' == "a.b.c", we can recurse down to the subexpression with path
                // "c" to see if we can rename it. If we can, we can rename this path too.
                return {false, boost::none};
            }
        }

        // There should never be multiple applicable renames.
        invariant(renamesFound <= 1u);
        if (renamesFound == 1u) {
            // There is an applicable rename. Modify the path of this expression to use the new
            // name.
            return {true, rewrittenPath};
        }

        return {true, boost::none};
    }

    void serialize(BSONObjBuilder* out, const SerializationOptions& opts) const override {
        if (opts.includePath) {
            BSONObjBuilder subObj(out->subobjStart(opts.serializeFieldPathFromString(path())));
            appendSerializedRightHandSide(&subObj, opts);
            subObj.doneFast();
        } else {
            appendSerializedRightHandSide(out, opts);
        }
    }

    /**
     * Constructs a BSONObj that represents the right-hand-side of a PathMatchExpression. Used for
     * serialization of PathMatchExpression in cases where we do not want to serialize the path in
     * line with the expression. For example {x: {$not: {$eq: 1}}}, where $eq is the
     * PathMatchExpression.
     *
     * Serialization options should be respected for any descendent expressions. Eg, if the
     * 'literalPolicy' option is 'kToDebugTypeString', then any literal argument (like the number 1
     * in the example above), should be "shapified" (e.g. "?number"). 'literal' here is in contrast
     * to another expression, if that is possible syntactically.
     */
    virtual void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                               const SerializationOptions& opts = {}) const = 0;

    BSONObj getSerializedRightHandSide(const SerializationOptions& opts = {}) const {
        BSONObjBuilder bob;
        appendSerializedRightHandSide(&bob, opts);
        return bob.obj();
    }

private:
    // ElementPath holds a FieldRef, which owns the underlying path string.
    // May be boost::none if this MatchExpression does not apply to a specific path.
    boost::optional<ElementPath> _elementPath;
};
}  // namespace mongo
