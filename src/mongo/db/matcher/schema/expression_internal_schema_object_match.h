// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class InternalSchemaObjectMatchExpression final : public PathMatchExpression {
public:
    static constexpr std::string_view kName = "$_internalSchemaObjectMatch"sv;
    static constexpr int kNumChildren = 1;

    InternalSchemaObjectMatchExpression(boost::optional<std::string_view> path,
                                        std::unique_ptr<MatchExpression> expr,
                                        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> clone() const final;

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const final;

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    size_t numChildren() const final {
        invariant(_sub);
        return kNumChildren;
    }

    MatchExpression* getChild(size_t i) const final {
        // 'i' must be 0 since there's always exactly one child.
        tassert(6400217, "Out-of-bounds access to child of MatchExpression.", i < kNumChildren);
        return _sub.get();
    }

    void resetChild(size_t i, MatchExpression* other) final {
        tassert(6329410, "Out-of-bounds access to child of MatchExpression.", i < kNumChildren);
        _sub.reset(other);
    }

    MatchExpression* releaseChild() {
        return _sub.release();
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
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
}  // namespace mongo
