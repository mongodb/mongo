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

#include <cstddef>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/clonable_ptr.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/path.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"

#include <memory>
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
                                 boost::optional<StringData> path,
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
    ElemMatchObjectMatchExpression(boost::optional<StringData> path,
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
                                       const SerializationOptions& opts = {},
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
    ElemMatchValueMatchExpression(boost::optional<StringData> path,
                                  std::unique_ptr<MatchExpression> sub,
                                  clonable_ptr<ErrorAnnotation> annotation = nullptr);
    explicit ElemMatchValueMatchExpression(boost::optional<StringData> path,
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
                                       const SerializationOptions& opts = {},
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
    SizeMatchExpression(boost::optional<StringData> path,
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
                                       const SerializationOptions& opts = {},
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
