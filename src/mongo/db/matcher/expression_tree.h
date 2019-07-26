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


/**
 * this contains all Expessions that define the structure of the tree
 * they do not look at the structure of the documents themselves, just combine other things
 */
namespace mongo {

class ListOfMatchExpression : public MatchExpression {
public:
    explicit ListOfMatchExpression(MatchType type) : MatchExpression(type) {}
    virtual ~ListOfMatchExpression();

    /**
     * @param e - I take ownership
     */
    void add(MatchExpression* e);

    /**
     * clears all the thingsd we own, and does NOT delete
     * someone else has taken ownership
     */
    void clearAndRelease() {
        _expressions.clear();
    }

    virtual size_t numChildren() const {
        return _expressions.size();
    }

    virtual MatchExpression* getChild(size_t i) const {
        return _expressions[i];
    }

    /*
     * Replaces the ith child with nullptr, and releases ownership of the child.
     */
    virtual std::unique_ptr<MatchExpression> releaseChild(size_t i) {
        auto child = std::unique_ptr<MatchExpression>(_expressions[i]);
        _expressions[i] = nullptr;
        return child;
    }

    /*
     * Removes the ith child, and releases ownership of the child.
     */
    virtual std::unique_ptr<MatchExpression> removeChild(size_t i) {
        auto child = std::unique_ptr<MatchExpression>(_expressions[i]);
        _expressions.erase(_expressions.begin() + i);
        return child;
    }

    virtual std::vector<MatchExpression*>* getChildVector() {
        return &_expressions;
    }

    bool equivalent(const MatchExpression* other) const;

    MatchCategory getCategory() const final {
        return MatchCategory::kLogical;
    }

protected:
    void _debugList(StringBuilder& debug, int indentationLevel) const;

    void _listToBSON(BSONArrayBuilder* out) const;

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    std::vector<MatchExpression*> _expressions;
};

class AndMatchExpression : public ListOfMatchExpression {
public:
    static constexpr StringData kName = "$and"_sd;

    AndMatchExpression() : ListOfMatchExpression(AND) {}
    virtual ~AndMatchExpression() {}

    virtual bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<AndMatchExpression> self = std::make_unique<AndMatchExpression>();
        for (size_t i = 0; i < numChildren(); ++i) {
            self->add(getChild(i)->shallowClone().release());
        }
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return std::move(self);
    }

    virtual void debugString(StringBuilder& debug, int indentationLevel = 0) const;

    virtual void serialize(BSONObjBuilder* out) const;

    bool isTriviallyTrue() const final;
};

class OrMatchExpression : public ListOfMatchExpression {
public:
    static constexpr StringData kName = "$or"_sd;

    OrMatchExpression() : ListOfMatchExpression(OR) {}
    virtual ~OrMatchExpression() {}

    virtual bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<OrMatchExpression> self = std::make_unique<OrMatchExpression>();
        for (size_t i = 0; i < numChildren(); ++i) {
            self->add(getChild(i)->shallowClone().release());
        }
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return std::move(self);
    }

    virtual void debugString(StringBuilder& debug, int indentationLevel = 0) const;

    virtual void serialize(BSONObjBuilder* out) const;

    bool isTriviallyFalse() const final;
};

class NorMatchExpression : public ListOfMatchExpression {
public:
    static constexpr StringData kName = "$nor"_sd;

    NorMatchExpression() : ListOfMatchExpression(NOR) {}
    virtual ~NorMatchExpression() {}

    virtual bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<NorMatchExpression> self = std::make_unique<NorMatchExpression>();
        for (size_t i = 0; i < numChildren(); ++i) {
            self->add(getChild(i)->shallowClone().release());
        }
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return std::move(self);
    }

    virtual void debugString(StringBuilder& debug, int indentationLevel = 0) const;

    virtual void serialize(BSONObjBuilder* out) const;
};

class NotMatchExpression final : public MatchExpression {
public:
    explicit NotMatchExpression(MatchExpression* e) : MatchExpression(NOT), _exp(e) {}

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<NotMatchExpression> self =
            std::make_unique<NotMatchExpression>(_exp->shallowClone().release());
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return std::move(self);
    }

    virtual bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const {
        return !_exp->matches(doc, nullptr);
    }

    bool matchesSingleElement(const BSONElement& elt, MatchDetails* details = nullptr) const final {
        return !_exp->matchesSingleElement(elt, details);
    }

    virtual void debugString(StringBuilder& debug, int indentationLevel = 0) const;

    virtual void serialize(BSONObjBuilder* out) const;

    bool equivalent(const MatchExpression* other) const;

    size_t numChildren() const final {
        return 1;
    }

    MatchExpression* getChild(size_t i) const final {
        return _exp.get();
    }

    std::vector<MatchExpression*>* getChildVector() final {
        return nullptr;
    }

    MatchExpression* releaseChild(void) {
        return _exp.release();
    }

    void resetChild(MatchExpression* newChild) {
        _exp.reset(newChild);
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kLogical;
    }

private:
    static boost::optional<StringData> getPathIfNotWithSinglePathMatchExpressionTree(
        MatchExpression* exp);
    static void serializeNotExpressionToNor(MatchExpression* exp, BSONObjBuilder* out);

    ExpressionOptimizerFunc getOptimizer() const final;

    std::unique_ptr<MatchExpression> _exp;
};
}  // namespace mongo
