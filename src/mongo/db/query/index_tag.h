/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <vector>

#include "mongo/bson/util/builder.h"
#include "mongo/db/matcher/expression.h"

namespace mongo {

// output from enumerator to query planner
class IndexTag : public MatchExpression::TagData {
public:
    static const size_t kNoIndex;

    /**
     * Assigns a leaf expression to the leading field of index 'i' where combining bounds with other
     * leaf expressions is known to be safe.
     */
    IndexTag(size_t i) : index(i) {}

    /**
     * Assigns a leaf expresssion to position 'p' of index 'i' where whether it is safe to combine
     * bounds with other leaf expressions is defined by 'canCombineBounds_'.
     */
    IndexTag(size_t i, size_t p, bool canCombineBounds_)
        : index(i), pos(p), canCombineBounds(canCombineBounds_) {}

    virtual ~IndexTag() {}

    virtual void debugString(StringBuilder* builder) const {
        *builder << " || Selected Index #" << index << " pos " << pos << " combine "
                 << canCombineBounds;
    }

    virtual MatchExpression::TagData* clone() const {
        return new IndexTag(index, pos, canCombineBounds);
    }

    // What index should we try to use for this leaf?
    size_t index = kNoIndex;

    // What position are we in the index?  (Compound.)
    size_t pos = 0U;

    // The plan enumerator can assign multiple predicates to the same position of a multikey index
    // when generating a self-intersection index assignment in enumerateAndIntersect().
    // 'canCombineBounds' gives the access planner enough information to know when it is safe to
    // intersect the bounds for multiple leaf expressions on the 'pos' field of 'index' and when it
    // isn't. The plan enumerator should never generate an index assignment where it isn't safe to
    // compound the bounds for multiple leaf expressions on the index.
    bool canCombineBounds = true;
};

// used internally
class RelevantTag : public MatchExpression::TagData {
public:
    RelevantTag() : elemMatchExpr(NULL), pathPrefix("") {}

    std::vector<size_t> first;
    std::vector<size_t> notFirst;

    // We don't know the full path from a node unless we keep notes as we traverse from the
    // root.  We do this once and store it.
    // TODO: Do a FieldRef / StringData pass.
    // TODO: We might want this inside of the MatchExpression.
    std::string path;

    // Points to the innermost containing $elemMatch. If this tag is
    // attached to an expression not contained in an $elemMatch, then
    // 'elemMatchExpr' is NULL. Not owned here.
    MatchExpression* elemMatchExpr;

    // If not contained inside an elemMatch, 'pathPrefix' contains the
    // part of 'path' prior to the first dot. For example, if 'path' is
    // "a.b.c", then 'pathPrefix' is "a". If 'path' is just "a", then
    // 'pathPrefix' is also "a".
    //
    // If tagging a predicate contained in an $elemMatch, 'pathPrefix'
    // holds the prefix of the path *inside* the $elemMatch. If this
    // tags predicate {a: {$elemMatch: {"b.c": {$gt: 1}}}}, then
    // 'pathPrefix' is "b".
    //
    // Used by the plan enumerator to make sure that we never
    // compound two predicates sharing a path prefix.
    std::string pathPrefix;

    virtual void debugString(StringBuilder* builder) const {
        *builder << " || First: ";
        for (size_t i = 0; i < first.size(); ++i) {
            *builder << first[i] << " ";
        }
        *builder << "notFirst: ";
        for (size_t i = 0; i < notFirst.size(); ++i) {
            *builder << notFirst[i] << " ";
        }
        *builder << "full path: " << path;
    }

    virtual MatchExpression::TagData* clone() const {
        RelevantTag* ret = new RelevantTag();
        ret->first = first;
        ret->notFirst = notFirst;
        return ret;
    }
};

/**
 * Tags each node of the tree with the lowest numbered index that the sub-tree rooted at that
 * node uses.
 *
 * Nodes that satisfy Indexability::nodeCanUseIndexOnOwnField are already tagged if there
 * exists an index that that node can use.
 */
void tagForSort(MatchExpression* tree);

/**
 * Sorts the tree using its IndexTag(s). Nodes that use the same index are adjacent to one
 * another.
 */
void sortUsingTags(MatchExpression* tree);

}  // namespace mongo
