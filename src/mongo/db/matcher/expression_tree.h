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
#include <memory>

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/util/make_data_structure.h"

/**
 * this contains all Expessions that define the structure of the tree
 * they do not look at the structure of the documents themselves, just combine other things
 */
namespace mongo {

class ListOfMatchExpression : public MatchExpression {
public:
    ListOfMatchExpression(MatchType type,
                          clonable_ptr<ErrorAnnotation> annotation,
                          std::vector<std::unique_ptr<MatchExpression>> expressions)
        : MatchExpression(type, std::move(annotation)), _expressions(std::move(expressions)) {}

    void add(std::unique_ptr<MatchExpression> e) {
        _expressions.push_back(std::move(e));
    }

    void clear() {
        _expressions.clear();
    }

    virtual size_t numChildren() const {
        return _expressions.size();
    }

    MatchExpression* getChild(size_t i) const final {
        tassert(6400201, "Out-of-bounds access to child of MatchExpression.", i < numChildren());
        return _expressions[i].get();
    }

    void resetChild(size_t i, MatchExpression* other) override {
        tassert(6329404, "Out-of-bounds access to child of MatchExpression.", i < numChildren());
        _expressions[i].reset(other);
    }

    /*
     * Replaces the ith child with nullptr, and releases ownership of the child.
     */
    std::unique_ptr<MatchExpression> releaseChild(size_t i) {
        return std::move(_expressions[i]);
    }

    /*
     * Removes the ith child, and releases ownership of the child.
     */
    virtual std::unique_ptr<MatchExpression> removeChild(size_t i) {
        auto child = std::move(_expressions[i]);
        _expressions.erase(_expressions.begin() + i);
        return child;
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return &_expressions;
    }

    bool equivalent(const MatchExpression* other) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kLogical;
    }

protected:
    void _debugList(StringBuilder& debug, int indentationLevel) const;

    void _listToBSON(BSONArrayBuilder* out, bool includePath) const;

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    std::vector<std::unique_ptr<MatchExpression>> _expressions;
};

class AndMatchExpression : public ListOfMatchExpression {
public:
    static constexpr StringData kName = "$and"_sd;

    AndMatchExpression(clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(AND, std::move(annotation), {}) {}
    AndMatchExpression(std::vector<std::unique_ptr<MatchExpression>> expressions,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(AND, std::move(annotation), std::move(expressions)) {}
    AndMatchExpression(std::unique_ptr<MatchExpression> expression,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(AND, std::move(annotation), makeVector(std::move(expression))) {}

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<AndMatchExpression> self =
            std::make_unique<AndMatchExpression>(_errorAnnotation);
        for (size_t i = 0; i < numChildren(); ++i) {
            self->add(getChild(i)->shallowClone());
        }
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return self;
    }

    virtual void debugString(StringBuilder& debug, int indentationLevel = 0) const;

    virtual void serialize(BSONObjBuilder* out, bool includePath) const;

    bool isTriviallyTrue() const final;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class OrMatchExpression : public ListOfMatchExpression {
public:
    static constexpr StringData kName = "$or"_sd;

    OrMatchExpression(clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(OR, std::move(annotation), {}) {}
    OrMatchExpression(std::vector<std::unique_ptr<MatchExpression>> expressions,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(OR, std::move(annotation), std::move(expressions)) {}
    OrMatchExpression(std::unique_ptr<MatchExpression> expression,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(OR, std::move(annotation), makeVector(std::move(expression))) {}

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<OrMatchExpression> self =
            std::make_unique<OrMatchExpression>(_errorAnnotation);
        for (size_t i = 0; i < numChildren(); ++i) {
            self->add(getChild(i)->shallowClone());
        }
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return self;
    }

    virtual void debugString(StringBuilder& debug, int indentationLevel = 0) const;

    virtual void serialize(BSONObjBuilder* out, bool includePath) const;

    bool isTriviallyFalse() const final;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class NorMatchExpression : public ListOfMatchExpression {
public:
    static constexpr StringData kName = "$nor"_sd;

    NorMatchExpression(clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(NOR, std::move(annotation), {}) {}
    NorMatchExpression(std::vector<std::unique_ptr<MatchExpression>> expressions,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(NOR, std::move(annotation), std::move(expressions)) {}
    NorMatchExpression(std::unique_ptr<MatchExpression> expression,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(NOR, std::move(annotation), makeVector(std::move(expression))) {}

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<NorMatchExpression> self =
            std::make_unique<NorMatchExpression>(_errorAnnotation);
        for (size_t i = 0; i < numChildren(); ++i) {
            self->add(getChild(i)->shallowClone());
        }
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return self;
    }

    virtual void debugString(StringBuilder& debug, int indentationLevel = 0) const;

    virtual void serialize(BSONObjBuilder* out, bool includePath) const;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class NotMatchExpression final : public MatchExpression {
public:
    static constexpr int kNumChildren = 1;
    explicit NotMatchExpression(MatchExpression* e,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(NOT, std::move(annotation)), _exp(e) {}

    explicit NotMatchExpression(std::unique_ptr<MatchExpression> expr,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(NOT, std::move(annotation)), _exp(std::move(expr)) {}

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<NotMatchExpression> self =
            std::make_unique<NotMatchExpression>(_exp->shallowClone(), _errorAnnotation);
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return self;
    }

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final {
        return !_exp->matches(doc, nullptr);
    }

    bool matchesSingleElement(const BSONElement& elt, MatchDetails* details = nullptr) const final {
        return !_exp->matchesSingleElement(elt, details);
    }

    virtual void debugString(StringBuilder& debug, int indentationLevel = 0) const;

    virtual void serialize(BSONObjBuilder* out, bool includePath) const;

    bool equivalent(const MatchExpression* other) const final;

    size_t numChildren() const final {
        return kNumChildren;
    }

    MatchExpression* getChild(size_t i) const final {
        tassert(6400210, "Out-of-bounds access to child of MatchExpression.", i < kNumChildren);
        return _exp.get();
    }


    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    MatchExpression* releaseChild() {
        return _exp.release();
    }

    void resetChild(size_t i, MatchExpression* newChild) {
        tassert(6329405, "Out-of-bounds access to child of MatchExpression.", i < numChildren());
        _exp.reset(newChild);
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kLogical;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    static void serializeNotExpressionToNor(MatchExpression* exp,
                                            BSONObjBuilder* out,
                                            bool includePath);

    ExpressionOptimizerFunc getOptimizer() const final;

    std::unique_ptr<MatchExpression> _exp;
};
}  // namespace mongo
