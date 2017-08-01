/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
    PathMatchExpression(MatchType matchType) : MatchExpression(matchType) {}

    virtual ~PathMatchExpression() {}

    /**
     * Returns whether or not this expression should match against each element of an array (in
     * addition to the array as a whole).
     *
     * For example, returns true if a path match expression on "f" should match against 1, 2, and
     * [1, 2] for document {f: [1, 2]}. Returns false if this expression should only match against
     * [1, 2].
     */
    virtual bool shouldExpandLeafArray() const = 0;

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
        return _path;
    }

    Status setPath(StringData path) {
        _path = path;
        auto status = _elementPath.init(_path);
        if (!status.isOK()) {
            return status;
        }

        _elementPath.setTraverseLeafArray(shouldExpandLeafArray());
        return Status::OK();
    }

private:
    StringData _path;
    ElementPath _elementPath;
};
}  // namespace mongo
