// expression_array.h

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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression_path.h"

namespace mongo {

/**
 * A path match expression which does not expand arrays at the end of the path, and which only
 * matches if the path contains an array.
 */
class ArrayMatchingMatchExpression : public PathMatchExpression {
public:
    ArrayMatchingMatchExpression(MatchType matchType)
        : PathMatchExpression(matchType,
                              ElementPath::LeafArrayBehavior::kNoTraversal,
                              ElementPath::NonLeafArrayBehavior::kTraverse) {}

    virtual ~ArrayMatchingMatchExpression() {}

    /**
     * Returns whether or not the nested array, represented as the object 'anArray', matches.
     *
     * 'anArray' must be the nested array at this expression's path.
     */
    virtual bool matchesArray(const BSONObj& anArray, MatchDetails* details) const = 0;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    bool equivalent(const MatchExpression* other) const override;

    MatchCategory getCategory() const final {
        return MatchCategory::kArrayMatching;
    }
};

class ElemMatchObjectMatchExpression : public ArrayMatchingMatchExpression {
public:
    ElemMatchObjectMatchExpression() : ArrayMatchingMatchExpression(ELEM_MATCH_OBJECT) {}
    Status init(StringData path, MatchExpression* sub);

    bool matchesArray(const BSONObj& anArray, MatchDetails* details) const;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ElemMatchObjectMatchExpression> e =
            stdx::make_unique<ElemMatchObjectMatchExpression>();
        e->init(path(), _sub->shallowClone().release()).transitional_ignore();
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    std::vector<MatchExpression*>* getChildVector() final {
        return nullptr;
    }

    virtual size_t numChildren() const {
        return 1;
    }

    virtual MatchExpression* getChild(size_t i) const {
        return _sub.get();
    }

    std::unique_ptr<MatchExpression> releaseChild() {
        return std::move(_sub);
    }

    void resetChild(std::unique_ptr<MatchExpression> newChild) {
        _sub = std::move(newChild);
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    std::unique_ptr<MatchExpression> _sub;
};

class ElemMatchValueMatchExpression : public ArrayMatchingMatchExpression {
public:
    ElemMatchValueMatchExpression() : ArrayMatchingMatchExpression(ELEM_MATCH_VALUE) {}
    virtual ~ElemMatchValueMatchExpression();

    Status init(StringData path);
    Status init(StringData path, MatchExpression* sub);
    void add(MatchExpression* sub);

    bool matchesArray(const BSONObj& anArray, MatchDetails* details) const;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ElemMatchValueMatchExpression> e =
            stdx::make_unique<ElemMatchValueMatchExpression>();
        e->init(path()).transitional_ignore();
        for (size_t i = 0; i < _subs.size(); ++i) {
            e->add(_subs[i]->shallowClone().release());
        }
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual std::vector<MatchExpression*>* getChildVector() {
        return &_subs;
    }

    virtual size_t numChildren() const {
        return _subs.size();
    }

    virtual MatchExpression* getChild(size_t i) const {
        return _subs[i];
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    bool _arrayElementMatchesAll(const BSONElement& e) const;

    std::vector<MatchExpression*> _subs;
};

class SizeMatchExpression : public ArrayMatchingMatchExpression {
public:
    SizeMatchExpression() : ArrayMatchingMatchExpression(SIZE) {}
    Status init(StringData path, int size);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<SizeMatchExpression> e = stdx::make_unique<SizeMatchExpression>();
        e->init(path(), _size).transitional_ignore();
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    size_t numChildren() const override {
        return 0;
    }

    MatchExpression* getChild(size_t i) const override {
        return nullptr;
    }

    std::vector<MatchExpression*>* getChildVector() final {
        return nullptr;
    }

    virtual bool matchesArray(const BSONObj& anArray, MatchDetails* details) const;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    int getData() const {
        return _size;
    }

private:
    virtual ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    int _size;  // >= 0 real, < 0, nothing will match
};
}
