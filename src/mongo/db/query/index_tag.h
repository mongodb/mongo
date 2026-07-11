// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/exact_cast.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_hasher.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {

// output from enumerator to query planner
class IndexTag final : public MatchExpression::TagData {
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

    ~IndexTag() override {}

    void debugString(StringBuilder* builder) const override {
        *builder << " || Selected Index #" << index << " pos " << pos << " combine "
                 << canCombineBounds << "\n";
    }

    MatchExpression::TagData* clone() const override {
        return new IndexTag(index, pos, canCombineBounds);
    }

    Type getType() const override {
        return Type::IndexTag;
    }

    void hash(absl::HashState& state, const MatchExpression::HashParam& param) const override {
        state = absl::HashState::combine(
            std::move(state), param.indexes->at(index).identifier, pos, canCombineBounds);
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
class RelevantTag final : public MatchExpression::TagData {
public:
    RelevantTag() : elemMatchExpr(nullptr), notExpr(nullptr), pathPrefix("") {}

    std::vector<size_t> first;
    std::vector<size_t> notFirst;

    // We don't know the full path from a node unless we keep notes as we traverse from the
    // root.  We do this once and store it.
    // TODO SERVER-122505: Do a FieldRef / std::string_view pass.
    // TODO SERVER-122506: We might want this inside of the MatchExpression.
    std::string path;

    // Points to the innermost containing $elemMatch. If this tag is
    // attached to an expression not contained in an $elemMatch, then
    // 'elemMatchExpr' is NULL. Not owned here.
    MatchExpression* elemMatchExpr;

    // Points to innermost containing $not within the innermost containing $elemMatch, provided
    // the tagged-node is bounds generating. Set to nullptr whenever there is no $not or there is no
    // $not within the innermost containing $elemMatch.
    MatchExpression* notExpr;

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

    void debugString(StringBuilder* builder) const override {
        *builder << " || First: ";
        for (size_t i = 0; i < first.size(); ++i) {
            *builder << first[i] << " ";
        }
        *builder << "notFirst: ";
        for (size_t i = 0; i < notFirst.size(); ++i) {
            *builder << notFirst[i] << " ";
        }
        *builder << "full path: " << path << "\n";
    }

    MatchExpression::TagData* clone() const override {
        RelevantTag* ret = new RelevantTag();
        ret->first = first;
        ret->notFirst = notFirst;
        return ret;
    }

    void hash(absl::HashState& state, const MatchExpression::HashParam& param) const override {
        MONGO_UNREACHABLE_TASSERT(9766200);
    }

    Type getType() const override {
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
        std::unique_ptr<OrPushdownTag> clone = std::make_unique<OrPushdownTag>();
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

    void hash(absl::HashState& state, const MatchExpression::HashParam& param) const override {
        if (_indexTag) {
            _indexTag->hash(state, param);
        }
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

/**
 * A tag to signal whether or not a predicate should be pruned from the MatchExpression tree.
 */
class PruneTag : public MatchExpression::TagData {
public:
    PruneTag(bool shouldBePruned) : _shouldBePruned(shouldBePruned) {}

    void debugString(StringBuilder* builder) const override {
        *builder << " prune: " << _shouldBePruned;
    }

    TagData* clone() const override {
        return new PruneTag(_shouldBePruned);
    }

    Type getType() const override {
        return Type::PruneTag;
    }

    void hash(absl::HashState& state, const MatchExpression::HashParam& param) const override {
        // Pruning tags are not relevant for the plan cache. They should actually never be left in
        // there.
        MONGO_UNREACHABLE_TASSERT(10360105);
    }

    bool shouldBePruned() const {
        return _shouldBePruned;
    }

private:
    bool _shouldBePruned;
};

/*
 * Reorders the nodes according to their tags as needed for access planning. 'tree' should be a
 * tagged MatchExpression tree in canonical order.
 */
void prepareForAccessPlanning(MatchExpression* tree);

/**
 * Downcasts from (possibly CV-qualified) TagData* to a derived type.
 * Asserts that the cast succeeded.
 */
template <std::derived_from<MatchExpression::TagData> Derived, typename Base>
requires std::is_same_v<MatchExpression::TagData, std::remove_cv_t<MatchExpression::TagData>>
Derived* indexTagCast(Base* td) {
    if (!td) {
        return nullptr;
    }

    auto* ptr = exact_pointer_cast<Derived*>(td);
    tassert(11408400,
            str::stream() << "Expected " << typeid(Derived).name() << " type, got "
                          << typeid(*td).name(),
            ptr);
    return ptr;
}

}  // namespace mongo
