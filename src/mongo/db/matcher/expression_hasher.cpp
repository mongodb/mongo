/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression_hasher.h"

#include "mongo/db/fts/fts_query_hash.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_internal_eq_hashed_key.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_text_noop.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/expression_where_noop.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/pipeline/expression_hasher.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hash_utils.h"

namespace mongo {
/**
 * MatcherTypeSet's hash function compatible with absl::Hash.
 */
template <typename H>
H AbslHashValue(H h, const MatcherTypeSet& expr) {
    return H::combine(std::move(h), expr.getBSONTypeMask(), expr.allNumbers);
}

/**
 * Collation's hash function compatible with absl::Hash.
 */
template <typename H>
H AbslHashValue(H h, const Collation& collation) {
    return H::combine(std::move(h),
                      collation.getLocale(),
                      collation.getCaseLevel(),
                      collation.getCaseFirst(),
                      collation.getStrength(),
                      collation.getNumericOrdering(),
                      collation.getAlternate(),
                      collation.getMaxVariable(),
                      collation.getNormalization(),
                      collation.getBackwards().value_or(false),
                      collation.getVersion());
}

/**
 * MatchExpression's hasher implementation compatible with absl::Hash.
 */
template <typename H>
class MatchExpressionHashVisitor final : public MatchExpressionConstVisitor {
public:
    explicit MatchExpressionHashVisitor(H hashState) : _hashState(std::move(hashState)) {}

    void visit(const BitsAllClearMatchExpression* expr) final {
        visitBitTest(expr);
    }
    void visit(const BitsAllSetMatchExpression* expr) final {
        visitBitTest(expr);
    }
    void visit(const BitsAnyClearMatchExpression* expr) final {
        visitBitTest(expr);
    }
    void visit(const BitsAnySetMatchExpression* expr) final {
        visitBitTest(expr);
    }

    void visit(const EqualityMatchExpression* expr) final {
        visitComparison(expr);
    }
    void visit(const GTEMatchExpression* expr) final {
        visitComparison(expr);
    }
    void visit(const GTMatchExpression* expr) final {
        visitComparison(expr);
    }
    void visit(const LTEMatchExpression* expr) final {
        visitComparison(expr);
    }
    void visit(const LTMatchExpression* expr) final {
        visitComparison(expr);
    }

    void visit(const InMatchExpression* expr) final {
        // `equivalent()` function compares $in using values of type, path, hasNull, collator,
        // regexes, and equalities fields.

        hashCombineTypeAndPath(expr);
        _hashState = H::combine(std::move(_hashState), expr->hasNull());
        hashCombineCollator(expr->getCollator());

        BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore,
                                     expr->getCollator());
        for (const auto& eq : expr->getEqualities()) {
            _hashState = H::combine(std::move(_hashState), eltCmp.hash(eq));
        }

        for (const auto& reg : expr->getRegexes()) {
            _hashState = H::combine(std::move(_hashState), calculateHash(*reg.get()));
        }
    }

    void visit(const ModMatchExpression* expr) final {
        // `equivalent()` function does not use DivisorInputParamId and RemainderInputParamId.
        hashCombineTypeAndPath(expr);
        _hashState = H::combine(std::move(_hashState), expr->getDivisor(), expr->getRemainder());
    }

    void visit(const RegexMatchExpression* expr) final {
        // `equivalent()` function uses values of path, regex, and flags fields. `getString()`
        // function returns value of regex field.
        hashCombineTypeAndPath(expr);
        _hashState = H::combine(std::move(_hashState), expr->getString(), expr->getFlags());
    }

    void visit(const SizeMatchExpression* expr) final {
        // `getData()` returns value of size field.
        hashCombineTypeAndPath(expr);
        _hashState = H::combine(std::move(_hashState), expr->getData());
    }

    void visit(const TypeMatchExpression* expr) final {
        // Only values of path, and typeSet fields are used by `equivalent()` function.
        hashCombineTypeAndPath(expr);
        _hashState = H::combine(std::move(_hashState), expr->typeSet());
    }

    void visit(const WhereMatchExpression* expr) final {
        visitWhere(expr);
    }

    void visit(const AlwaysFalseMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const AlwaysTrueMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const AndMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const ElemMatchObjectMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const ElemMatchValueMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const ExistsMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const ExprMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        hashCombineCollator(expr->getExpressionContext()->getCollator());
        _hashState = H::combine(std::move(_hashState), *expr->getExpression());
    }
    void visit(const GeoMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        _hashState =
            H::combine(std::move(_hashState), SimpleBSONObjComparator{}.hash(expr->_rawObj));
    }
    void visit(const GeoNearMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        _hashState =
            H::combine(std::move(_hashState), SimpleBSONObjComparator{}.hash(expr->_rawObj));
    }
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        _hashState = H::combine(
            std::move(_hashState),
            SimpleBSONObjComparator{}.hash(expr->getGeoContainer().getGeoElement().Obj()),
            expr->getField());
    }
    void visit(const InternalExprEqMatchExpression* expr) final {
        visitComparison(expr);
    }
    void visit(const InternalExprGTMatchExpression* expr) final {
        visitComparison(expr);
    }
    void visit(const InternalExprGTEMatchExpression* expr) final {
        visitComparison(expr);
    }
    void visit(const InternalExprLTMatchExpression* expr) final {
        visitComparison(expr);
    }
    void visit(const InternalExprLTEMatchExpression* expr) final {
        visitComparison(expr);
    }
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901800);
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901801);
    }
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901802);
    }
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901803);
    }
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901804);
    }
    void visit(const InternalSchemaCondMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901805);
    }
    void visit(const InternalSchemaEqMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901806);
    }
    void visit(const InternalSchemaFmodMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901807);
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901808);
    }
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901809);
    }
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901810);
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901811);
    }
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901812);
    }
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901813);
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901814);
    }
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901815);
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901816);
    }
    void visit(const InternalSchemaTypeExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901817);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901818);
    }
    void visit(const InternalSchemaXorMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901819);
    }
    void visit(const NorMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const NotMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const OrMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const TextMatchExpression* expr) final {
        visitText(expr);
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        visitText(expr);
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(7901820);
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        visitWhere(expr);
    }
    void visit(const InternalEqHashedKey* expr) final {
        visitComparison(expr);
    }

    H extractHashState() {
        return std::move(_hashState);
    }

private:
    void hashCombineTypeAndPath(const MatchExpression* expr) {
        _hashState = H::combine(std::move(_hashState), expr->matchType(), expr->path());
    }

    void hashCombineCollator(const CollatorInterface* ci) {
        if (ci) {
            _hashState = H::combine(std::move(_hashState), ci->getSpec());
        }
    }

    void visitBitTest(const BitTestMatchExpression* expr) {
        // BitPositionsParamId, BitMaskParamId, and BitMask are not hashed because they are not used
        // in `equivalent()` function.
        hashCombineTypeAndPath(expr);

        // `equivalent()` function compares sorted bitPositions.
        auto bitPositions = expr->getBitPositions();
        std::sort(bitPositions.begin(), bitPositions.end());
        _hashState = H::combine(std::move(_hashState), bitPositions);
    }

    void visitComparison(const ComparisonMatchExpressionBase* expr) {
        // InputParamId is not hashed because it is not used in `equivalent()` function.

        // Please, keep BSONElementComparator consistent with
        // `ComparisonMatchExpressionBase::equivalent()`.
        const StringDataComparator* stringComparator = nullptr;
        BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore,
                                     stringComparator);
        hashCombineTypeAndPath(expr);
        _hashState = H::combine(std::move(_hashState), eltCmp.hash(expr->getData()));
        hashCombineCollator(expr->getCollator());
    }

    void visitText(const TextMatchExpressionBase* expr) {
        hashCombineTypeAndPath(expr);
        _hashState = H::combine(std::move(_hashState), expr->getFTSQuery());
    }

    void visitWhere(const WhereMatchExpressionBase* expr) {
        hashCombineTypeAndPath(expr);
        _hashState = H::combine(std::move(_hashState), expr->getCode());
    }

    H _hashState;
};

template <typename H>
H AbslHashValue(H h, const MatchExpression& expr) {
    MatchExpressionHashVisitor<H> visitor{std::move(h)};
    MatchExpressionWalker walker{&visitor, nullptr, nullptr};
    tree_walker::walk<true, MatchExpression>(&expr, &walker);
    return std::move(visitor.extractHashState());
}

size_t calculateHash(const MatchExpression& expr) {
    return absl::Hash<MatchExpression>{}(expr);
}
}  // namespace mongo
