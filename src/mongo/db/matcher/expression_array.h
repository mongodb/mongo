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

#include <boost/optional.hpp>
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
    ArrayMatchingMatchExpression(MatchType matchType,
                                 StringData path,
                                 clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : PathMatchExpression(matchType,
                              path,
                              ElementPath::LeafArrayBehavior::kNoTraversal,
                              ElementPath::NonLeafArrayBehavior::kTraverse,
                              std::move(annotation)) {}

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
    ElemMatchObjectMatchExpression(StringData path,
                                   MatchExpression* sub,
                                   clonable_ptr<ErrorAnnotation> annotation = nullptr);

    bool matchesArray(const BSONObj& anArray, MatchDetails* details) const;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ElemMatchObjectMatchExpression> e =
            std::make_unique<ElemMatchObjectMatchExpression>(
                path(), _sub->shallowClone().release(), _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return e;
    }

    virtual void debugString(StringBuilder& debug, int indentationLevel) const;

    BSONObj getSerializedRightHandSide() const final;

    boost::optional<std::vector<MatchExpression*>&> getChildVector() final {
        return boost::none;
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

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    std::unique_ptr<MatchExpression> _sub;
};

class ElemMatchValueMatchExpression : public ArrayMatchingMatchExpression {
public:
    /**
     * This constructor takes ownership of 'sub.'
     */
    ElemMatchValueMatchExpression(StringData path,
                                  MatchExpression* sub,
                                  clonable_ptr<ErrorAnnotation> annotation = nullptr);
    explicit ElemMatchValueMatchExpression(StringData path,
                                           clonable_ptr<ErrorAnnotation> annotation = nullptr);
    virtual ~ElemMatchValueMatchExpression();

    void add(MatchExpression* sub);

    bool matchesArray(const BSONObj& anArray, MatchDetails* details) const;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ElemMatchValueMatchExpression> e =
            std::make_unique<ElemMatchValueMatchExpression>(path(), _errorAnnotation);
        for (size_t i = 0; i < _subs.size(); ++i) {
            e->add(_subs[i]->shallowClone().release());
        }
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return e;
    }

    virtual void debugString(StringBuilder& debug, int indentationLevel) const;

    BSONObj getSerializedRightHandSide() const final;

    boost::optional<std::vector<MatchExpression*>&> getChildVector() final {
        return _subs;
    }

    virtual size_t numChildren() const {
        return _subs.size();
    }

    virtual MatchExpression* getChild(size_t i) const {
        return _subs[i];
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    bool _arrayElementMatchesAll(const BSONElement& e) const;

    std::vector<MatchExpression*> _subs;
};

class SizeMatchExpression : public ArrayMatchingMatchExpression {
public:
    SizeMatchExpression(StringData path,
                        int size,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<SizeMatchExpression> e =
            std::make_unique<SizeMatchExpression>(path(), _size, _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return e;
    }

    size_t numChildren() const override {
        return 0;
    }

    MatchExpression* getChild(size_t i) const override {
        return nullptr;
    }

    boost::optional<std::vector<MatchExpression*>&> getChildVector() final {
        return boost::none;
    }

    virtual bool matchesArray(const BSONObj& anArray, MatchDetails* details) const;

    virtual void debugString(StringBuilder& debug, int indentationLevel) const;

    BSONObj getSerializedRightHandSide() const final;

    virtual bool equivalent(const MatchExpression* other) const;

    int getData() const {
        return _size;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    virtual ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    int _size;  // >= 0 real, < 0, nothing will match
};
}  // namespace mongo
