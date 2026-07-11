// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <cstddef>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/path.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {

/**
 * A path match expression which does not expand arrays at the end of the path, and which only
 * matches if the path contains an array.
 */
class ArrayMatchingMatchExpression : public PathMatchExpression {
public:
    ArrayMatchingMatchExpression(MatchType matchType,
                                 boost::optional<std::string_view> path,
                                 clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : PathMatchExpression(matchType,
                              path,
                              ElementPath::LeafArrayBehavior::kNoTraversal,
                              ElementPath::NonLeafArrayBehavior::kTraverse,
                              std::move(annotation)) {}

    bool equivalent(const MatchExpression* other) const override;

    MatchCategory getCategory() const final {
        return MatchCategory::kArrayMatching;
    }
};

class ElemMatchObjectMatchExpression final : public ArrayMatchingMatchExpression {
public:
    ElemMatchObjectMatchExpression(boost::optional<std::string_view> path,
                                   std::unique_ptr<MatchExpression> sub,
                                   clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> clone() const override {
        std::unique_ptr<ElemMatchObjectMatchExpression> e =
            std::make_unique<ElemMatchObjectMatchExpression>(
                path(), _sub->clone(), _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return e;
    }

    void debugString(StringBuilder& debug, int indentationLevel) const override;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    size_t numChildren() const override {
        return 1;
    }

    MatchExpression* getChild(size_t i) const override {
        tassert(6400204, "Out-of-bounds access to child of MatchExpression.", i < numChildren());
        return _sub.get();
    }

    void resetChild(size_t i, MatchExpression* other) override {
        tassert(6329401, "Out-of-bounds access to child of MatchExpression.", i < numChildren());
        _sub.reset(other);
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
    std::unique_ptr<MatchExpression> _sub;
};

class ElemMatchValueMatchExpression final : public ArrayMatchingMatchExpression {
public:
    ElemMatchValueMatchExpression(boost::optional<std::string_view> path,
                                  std::unique_ptr<MatchExpression> sub,
                                  clonable_ptr<ErrorAnnotation> annotation = nullptr);
    explicit ElemMatchValueMatchExpression(boost::optional<std::string_view> path,
                                           clonable_ptr<ErrorAnnotation> annotation = nullptr);

    void add(std::unique_ptr<MatchExpression> sub);

    std::unique_ptr<MatchExpression> clone() const override {
        std::unique_ptr<ElemMatchValueMatchExpression> e =
            std::make_unique<ElemMatchValueMatchExpression>(path(), _errorAnnotation);
        for (size_t i = 0; i < _subs.size(); ++i) {
            e->add(_subs[i]->clone());
        }
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return e;
    }

    void debugString(StringBuilder& debug, int indentationLevel) const override;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return &_subs;
    }

    size_t numChildren() const override {
        return _subs.size();
    }

    MatchExpression* getChild(size_t i) const override {
        tassert(6400205, "Out-of-bounds access to child of MatchExpression.", i < numChildren());
        return _subs[i].get();
    }

    void resetChild(size_t i, MatchExpression* other) override {
        tassert(6329402, "Out-of-bounds access to child of MatchExpression.", i < numChildren());
        _subs[i].reset(other);
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    bool _arrayElementMatchesAll(const BSONElement& e) const;

    std::vector<std::unique_ptr<MatchExpression>> _subs;
};

class SizeMatchExpression : public ArrayMatchingMatchExpression {
public:
    SizeMatchExpression(boost::optional<std::string_view> path,
                        int size,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<SizeMatchExpression> e =
            std::make_unique<SizeMatchExpression>(path(), _size, _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        if (getInputParamId()) {
            e->setInputParamId(*getInputParamId());
        }
        return e;
    }

    size_t numChildren() const override {
        return 0;
    }

    MatchExpression* getChild(size_t i) const override {
        tassert(6400206, "SizeMatchExpression does not have any children.", i < numChildren());
        return nullptr;
    }

    void resetChild(size_t i, MatchExpression* other) override {
        tassert(6329403, "SizeMatchExpression does not have any children.", i < numChildren());
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    void debugString(StringBuilder& debug, int indentationLevel) const override;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const override;

    int getData() const {
        return _size;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void setInputParamId(boost::optional<InputParamId> paramId) {
        _inputParamId = paramId;
    }

    boost::optional<InputParamId> getInputParamId() const {
        return _inputParamId;
    }

private:
    int _size;  // >= 0 real, < 0, nothing will match

    boost::optional<InputParamId> _inputParamId;
};
}  // namespace mongo
