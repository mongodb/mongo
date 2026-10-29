// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
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

/**
 * MatchExpression for $_internalSchemaRootDocEq, which matches the root document with the
 * following equality semantics:
 *
 * - comparisons between objects do not consider field order.
 * - null element values only match the literal null, and not missing or undefined values.
 * - always uses simple string comparison semantics, even if the query has a non-simple collation.
 */
class InternalSchemaRootDocEqMatchExpression final : public MatchExpression {
public:
    static constexpr std::string_view kName = "$_internalSchemaRootDocEq"sv;

    /**
     * Constructs a new match expression, taking ownership of 'rhs'.
     */
    explicit InternalSchemaRootDocEqMatchExpression(
        BSONObj rhs, clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(MatchExpression::INTERNAL_SCHEMA_ROOT_DOC_EQ, std::move(annotation)),
          _rhsObj(std::move(rhs)) {}

    std::unique_ptr<MatchExpression> clone() const final;

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final;

    void serialize(BSONObjBuilder* out,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const final;

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400218);
    }

    void resetChild(size_t i, MatchExpression*) final {
        MONGO_UNREACHABLE;
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
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

    BSONObj getRhsObj() const {
        return _rhsObj;
    }

    const BSONObj::ComparatorInterface* getObjCmp() const {
        return &_objCmp;
    }

private:
    UnorderedFieldsBSONObjComparator _objCmp;
    BSONObj _rhsObj;
};
}  // namespace mongo
