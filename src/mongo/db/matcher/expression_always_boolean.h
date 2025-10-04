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

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

class AlwaysBooleanMatchExpression : public MatchExpression {
public:
    AlwaysBooleanMatchExpression(MatchType type, clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(type, std::move(annotation)) {}

    ~AlwaysBooleanMatchExpression() override = default;

    /**
     * The name of this MatchExpression.
     */
    virtual StringData name() const = 0;

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final {
        _debugAddSpace(debug, indentationLevel);
        debug << name() << ": 1";
        _debugStringAttachTagInfo(&debug);
    }

    void serialize(BSONObjBuilder* out,
                   const SerializationOptions& opts = {},
                   bool includePath = true) const final {
        opts.appendLiteral(out, name(), 1);
    }

    bool equivalent(const MatchExpression* other) const final {
        return other->matchType() == matchType();
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    size_t numChildren() const override {
        return 0;
    }

    MatchExpression* getChild(size_t i) const override {
        MONGO_UNREACHABLE_TASSERT(6400202);
    }

    void resetChild(size_t, MatchExpression*) override {
        MONGO_UNREACHABLE;
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }
};

class AlwaysFalseMatchExpression final : public AlwaysBooleanMatchExpression {
public:
    static constexpr StringData kName = "$alwaysFalse"_sd;

    AlwaysFalseMatchExpression(clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : AlwaysBooleanMatchExpression(MatchType::ALWAYS_FALSE, std::move(annotation)) {}

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
        return std::make_unique<AlwaysFalseMatchExpression>(_errorAnnotation);
    }

    bool isTriviallyFalse() const final {
        return true;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class AlwaysTrueMatchExpression final : public AlwaysBooleanMatchExpression {
public:
    static constexpr StringData kName = "$alwaysTrue"_sd;

    AlwaysTrueMatchExpression(clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : AlwaysBooleanMatchExpression(MatchType::ALWAYS_TRUE, std::move(annotation)) {}

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
        return std::make_unique<AlwaysTrueMatchExpression>(_errorAnnotation);
    }

    bool isTriviallyTrue() const final {
        return true;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

}  // namespace mongo
