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

#include <deque>
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

    virtual Type getType() const {
        return Type::IndexTag;
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

    virtual Type getType() const {
        return Type::RelevantTag;
    }
};

/**
 * An OrPushdownTag indicates that this node is a predicate that can be used inside of a sibling
 * indexed OR.
 */
class OrPushdownTag final : public MatchExpression::TagData {
public:
    /**
     * A destination to which this predicate should be pushed down, consisting of a route through
     * the sibling indexed OR, and the tag the predicate should receive after it is pushed down.
     */
    struct Destination {

        Destination clone() const {
            Destination clone;
            clone.route = route;
            clone.tagData.reset(tagData->clone());
            return clone;
        }

        void debugString(StringBuilder* builder) const {
            *builder << " || Move to ";
            bool firstPosition = true;
            for (auto position : route) {
                if (!firstPosition) {
                    *builder << ",";
                }
                firstPosition = false;
                *builder << position;
            }
            tagData->debugString(builder);
        }

        /**
         * The route along which the predicate should be pushed down. This starts at the
         * indexed OR sibling of the predicate. Each value in 'route' is the index of a child in
         * an indexed OR.
         * For example, if the MatchExpression tree is:
         *         AND
         *        /    \
         *   {a: 5}    OR
         *           /    \
         *         AND    {e: 9}
         *       /     \
         *    {b: 6}   OR
         *           /    \
         *       {c: 7}  {d: 8}
         * and the predicate is {a: 5}, then the path {0, 1} means {a: 5} should be
         * AND-combined with {d: 8}.
         */
        std::deque<size_t> route;

        // The TagData that the predicate should be tagged with after it is pushed down.
        std::unique_ptr<MatchExpression::TagData> tagData;
    };

    void debugString(StringBuilder* builder) const override {
        if (_indexTag) {
            _indexTag->debugString(builder);
        }
        for (const auto& dest : _destinations) {
            dest.debugString(builder);
        }
    }

    MatchExpression::TagData* clone() const override {
        std::unique_ptr<OrPushdownTag> clone = stdx::make_unique<OrPushdownTag>();
        for (const auto& dest : _destinations) {
            clone->addDestination(dest.clone());
        }
        if (_indexTag) {
            clone->setIndexTag(_indexTag->clone());
        }
        return clone.release();
    }

    Type getType() const override {
        return Type::OrPushdownTag;
    }

    void addDestination(Destination dest) {
        _destinations.push_back(std::move(dest));
    }

    const std::vector<Destination>& getDestinations() const {
        return _destinations;
    }

    /**
     *  Releases ownership of the destinations.
     */
    std::vector<Destination> releaseDestinations() {
        std::vector<Destination> destinations;
        destinations.swap(_destinations);
        return destinations;
    }

    void setIndexTag(MatchExpression::TagData* indexTag) {
        _indexTag.reset(indexTag);
    }

    const MatchExpression::TagData* getIndexTag() const {
        return _indexTag.get();
    }

    std::unique_ptr<MatchExpression::TagData> releaseIndexTag() {
        return std::move(_indexTag);
    }

private:
    std::vector<Destination> _destinations;

    // The index tag the predicate should receive at its current position in the tree.
    std::unique_ptr<MatchExpression::TagData> _indexTag;
};

/*
 * Reorders the nodes according to their tags as needed for access planning. 'tree' should be a
 * tagged MatchExpression tree in canonical order.
 */
void prepareForAccessPlanning(MatchExpression* tree);

}  // namespace mongo
