// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class AlwaysBooleanMatchExpression : public MatchExpression {
public:
    AlwaysBooleanMatchExpression(MatchType type, clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(type, std::move(annotation)) {}

    ~AlwaysBooleanMatchExpression() override = default;

    /**
     * The name of this MatchExpression.
     */
    virtual std::string_view name() const = 0;

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final {
        _debugAddSpace(debug, indentationLevel);
        debug << name() << ": 1";
        _debugStringAttachTagInfo(&debug);
    }

    void serialize(BSONObjBuilder* out,
                   const query_shape::SerializationOptions& opts = {},
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

class [[MONGO_MOD_NEEDS_REPLACEMENT]] AlwaysFalseMatchExpression final
    : public AlwaysBooleanMatchExpression {
public:
    static constexpr std::string_view kName = "$alwaysFalse"sv;

    AlwaysFalseMatchExpression(clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : AlwaysBooleanMatchExpression(MatchType::ALWAYS_FALSE, std::move(annotation)) {}

    std::string_view name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
        auto clone = std::make_unique<AlwaysFalseMatchExpression>(_errorAnnotation);
        if (getTag()) {
            clone->setTag(getTag()->clone());
        }
        return clone;
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

class [[MONGO_MOD_NEEDS_REPLACEMENT]] AlwaysTrueMatchExpression final
    : public AlwaysBooleanMatchExpression {
public:
    static constexpr std::string_view kName = "$alwaysTrue"sv;

    AlwaysTrueMatchExpression(clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : AlwaysBooleanMatchExpression(MatchType::ALWAYS_TRUE, std::move(annotation)) {}

    std::string_view name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
        auto clone = std::make_unique<AlwaysTrueMatchExpression>(_errorAnnotation);
        if (getTag()) {
            clone->setTag(getTag()->clone());
        }
        return clone;
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
