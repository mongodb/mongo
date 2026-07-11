// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

/**
 * this contains all Expressions that define the structure of the tree
 * they do not look at the structure of the documents themselves, just combine other things
 */
namespace mongo {
using namespace std::literals::string_view_literals;

class ListOfMatchExpression : public MatchExpression {
public:
    ListOfMatchExpression(MatchType type,
                          clonable_ptr<ErrorAnnotation> annotation,
                          std::vector<std::unique_ptr<MatchExpression>> expressions)
        : MatchExpression(type, std::move(annotation)), _expressions(std::move(expressions)) {}

    void add(std::unique_ptr<MatchExpression> e) {
        _expressions.push_back(std::move(e));
    }

    void reserve(size_t n) {
        _expressions.reserve(n);
    }

    void clear() {
        _expressions.clear();
    }

    size_t numChildren() const override {
        return _expressions.size();
    }

    /**
     * Returns the unmodifiable vector of the children of the current node.
     */
    const std::vector<std::unique_ptr<MatchExpression>>& getChildren() const {
        return _expressions;
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

    const std::vector<std::unique_ptr<MatchExpression>>& getChildVector() const {
        return _expressions;
    }

    bool equivalent(const MatchExpression* other) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kLogical;
    }

protected:
    void _debugList(StringBuilder& debug, int indentationLevel) const;

    void _listToBSON(BSONArrayBuilder* out,
                     const query_shape::SerializationOptions& opts = {},
                     bool includePath = true) const;

private:
    std::vector<std::unique_ptr<MatchExpression>> _expressions;
};

class [[MONGO_MOD_NEEDS_REPLACEMENT]] AndMatchExpression : public ListOfMatchExpression {
public:
    static constexpr std::string_view kName = "$and"sv;

    AndMatchExpression(clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(AND, std::move(annotation), {}) {}
    AndMatchExpression(std::vector<std::unique_ptr<MatchExpression>> expressions,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(AND, std::move(annotation), std::move(expressions)) {}
    AndMatchExpression(std::unique_ptr<MatchExpression> expression,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(AND, std::move(annotation), makeVector(std::move(expression))) {}

    std::unique_ptr<MatchExpression> clone() const override {
        std::unique_ptr<AndMatchExpression> self =
            std::make_unique<AndMatchExpression>(_errorAnnotation);
        self->reserve(numChildren());
        for (size_t i = 0; i < numChildren(); ++i) {
            self->add(getChild(i)->clone());
        }
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return self;
    }

    void debugString(StringBuilder& debug, int indentationLevel = 0) const override;

    void serialize(BSONObjBuilder* out,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const override;

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
    static constexpr std::string_view kName = "$or"sv;

    OrMatchExpression(clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(OR, std::move(annotation), {}) {}
    OrMatchExpression(std::vector<std::unique_ptr<MatchExpression>> expressions,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(OR, std::move(annotation), std::move(expressions)) {}
    OrMatchExpression(std::unique_ptr<MatchExpression> expression,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(OR, std::move(annotation), makeVector(std::move(expression))) {}

    std::unique_ptr<MatchExpression> clone() const override {
        std::unique_ptr<OrMatchExpression> self =
            std::make_unique<OrMatchExpression>(_errorAnnotation);
        self->reserve(numChildren());
        for (size_t i = 0; i < numChildren(); ++i) {
            self->add(getChild(i)->clone());
        }
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return self;
    }

    void debugString(StringBuilder& debug, int indentationLevel = 0) const override;

    void serialize(BSONObjBuilder* out,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const override;

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
    static constexpr std::string_view kName = "$nor"sv;

    NorMatchExpression(clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(NOR, std::move(annotation), {}) {}
    NorMatchExpression(std::vector<std::unique_ptr<MatchExpression>> expressions,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(NOR, std::move(annotation), std::move(expressions)) {}
    NorMatchExpression(std::unique_ptr<MatchExpression> expression,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(NOR, std::move(annotation), makeVector(std::move(expression))) {}

    std::unique_ptr<MatchExpression> clone() const override {
        std::unique_ptr<NorMatchExpression> self =
            std::make_unique<NorMatchExpression>(_errorAnnotation);
        self->reserve(numChildren());
        for (size_t i = 0; i < numChildren(); ++i) {
            self->add(getChild(i)->clone());
        }
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return self;
    }

    void debugString(StringBuilder& debug, int indentationLevel = 0) const override;

    void serialize(BSONObjBuilder* out,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const override;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class [[MONGO_MOD_NEEDS_REPLACEMENT]] NotMatchExpression final : public MatchExpression {
public:
    static constexpr int kNumChildren = 1;
    explicit NotMatchExpression(MatchExpression* e,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(NOT, std::move(annotation)), _exp(e) {}

    explicit NotMatchExpression(std::unique_ptr<MatchExpression> expr,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(NOT, std::move(annotation)), _exp(std::move(expr)) {}

    std::unique_ptr<MatchExpression> clone() const override {
        std::unique_ptr<NotMatchExpression> self =
            std::make_unique<NotMatchExpression>(_exp->clone(), _errorAnnotation);
        if (getTag()) {
            self->setTag(getTag()->clone());
        }
        return self;
    }

    void debugString(StringBuilder& debug, int indentationLevel = 0) const override;

    void serialize(BSONObjBuilder* out,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const override;

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

    void resetChild(size_t i, MatchExpression* newChild) override {
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
                                            const query_shape::SerializationOptions& opts = {},
                                            bool includePath = true);

    std::unique_ptr<MatchExpression> _exp;
};
}  // namespace mongo
