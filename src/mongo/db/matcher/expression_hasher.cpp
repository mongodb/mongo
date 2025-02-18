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

#include "mongo/bson/bsonelement.h"
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
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/pipeline/expression_hasher.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/platform/decimal128.h"
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

template <typename H>
H AbslHashValue(H h, const Decimal128& dec) {
    return H::combine(std::move(h), dec.getValue().low64, dec.getValue().high64);
}

template <typename H>
H AbslHashValue(H h, const ExpressionWithPlaceholder& dec) {
    return H::combine(std::move(h), MatchExpressionHasher{}(dec.getFilter()), dec.getPlaceholder());
}

template <typename H>
H AbslHashValue(H h, const BSONElement& el) {
    return H::combine_contiguous(std::move(h), el.rawdata(), el.size());
}

/**
 * MatchExpression's hasher implementation compatible with absl::Hash.
 */
template <typename H>
class MatchExpressionHashVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionHashVisitor(H hashState, const MatchExpressionHashParams& hashParams)
        : _hashState(std::move(hashState)),
          _params(hashParams),
          _hashParamIds(_params.hashValuesOrParams & HashValuesOrParams::kHashParamIds) {}

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
        combine(expr->hasNull());
        hashCombineCollator(expr->getCollator());

        if (_hashParamIds && expr->getInputParamId()) {
            // There cannot be regexes when InMatchExpression is parameterized.
            combine(expr->getInputParamId().get());
        } else {
            // Hash the size of equalities's list and up to a maximum of 'maxNumberOfHashedElements'
            // evenly chosen equalities.
            BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore,
                                         expr->getCollator());
            const auto& equalities = expr->getEqualities();
            const size_t eqStep = std::max(static_cast<size_t>(1),
                                           equalities.size() / _params.maxNumberOfInElementsToHash);
            combine(equalities.size());
            for (size_t i = 0; i < equalities.size(); i += eqStep) {
                combine(eltCmp.hash(equalities[i]));
            }

            // Hash the size of regexes's list and up to a maximum of 'maxNumberOfHashedElements'
            // evenly chosen regexes.
            const auto& regexes = expr->getRegexes();
            const size_t regStep = std::max(static_cast<size_t>(1),
                                            regexes.size() / _params.maxNumberOfInElementsToHash);
            combine(regexes.size());
            for (size_t i = 0; i < regexes.size(); i += regStep) {
                combine(calculateHash(*regexes[i].get(), _params));
            }
        }
    }

    void visit(const ModMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);

        if (_hashParamIds && expr->getDivisorInputParamId()) {
            // Either both parameter IDs are set, or neither are.
            combine(expr->getDivisorInputParamId().get(), expr->getRemainderInputParamId().get());
        } else {
            // `equivalent()` function does not use DivisorInputParamId and RemainderInputParamId.
            combine(expr->getDivisor(), expr->getRemainder());
        }
    }

    void visit(const RegexMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);

        if (_hashParamIds && expr->getSourceRegexInputParamId()) {
            // Either both parameter IDs are set, or neither are.
            combine(expr->getSourceRegexInputParamId().get(),
                    expr->getCompiledRegexInputParamId().get());
        } else {
            // `equivalent()` function does not use SourceRegexInputParamId or
            // CompiledRegexInputParamId. `getString()` function returns value of regex field.
            combine(expr->getString());
        }

        combine(expr->getFlags());
    }

    void visit(const SizeMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);

        if (_hashParamIds && expr->getInputParamId()) {
            combine(expr->getInputParamId().get());
        } else {
            // `equivalent()` function does not use InputParamId. `getData()` returns value of size
            // field.
            combine(expr->getData());
        }
    }

    void visit(const TypeMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        if (_hashParamIds && expr->getInputParamId()) {
            combine(expr->getInputParamId().get());
        } else {
            // `equivalent()` function does not use InputParamId.
            combine(expr->typeSet());
        }
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
        combine(*expr->getExpression());
    }
    void visit(const GeoMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(SimpleBSONObjComparator{}.hash(expr->_rawObj));
    }
    void visit(const GeoNearMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(SimpleBSONObjComparator{}.hash(expr->_rawObj));
    }
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(SimpleBSONObjComparator{}.hash(expr->getGeoContainer().getGeoElement().Obj()),
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
        hashCombineTypeAndPath(expr);
        combine(expr->startIndex(), *expr->getExpression());
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->getNamePlaceholder(), *expr->getOtherwise());
        for (const auto& prop : expr->getProperties()) {
            combine(prop);
        }
        for (const auto& pat : expr->getPatternProperties()) {
            combine(pat.first.rawRegex, *pat.second.get());
        }
    }
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->typeSet());
    }
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->getBinDataSubType());
    }
    void visit(const InternalSchemaCondMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const InternalSchemaEqMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->getRhsElem());
    }
    void visit(const InternalSchemaFmodMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->getDivisor(), expr->getRemainder());
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->arrayIndex(), *expr->getExpression());
    }
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->getName(), expr->numItems());
    }
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->getName(), expr->strLen());
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->numProperties(), expr->getName());
    }
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->getName(), expr->numItems());
    }
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->getName(), expr->strLen());
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(expr->numProperties(), expr->getName());
    }
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(MatchExpressionHasher{}(expr->getChild(0)));
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
        combine(simpleHash(expr->getRhsObj()));
    }
    void visit(const InternalSchemaTypeExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
    }
    void visit(const InternalSchemaXorMatchExpression* expr) final {
        hashCombineTypeAndPath(expr);
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
        hashCombineTypeAndPath(expr);
        auto annulus = expr->getAnnulus();
        combine(annulus.getInner(), annulus.getOuter(), annulus.center().x, annulus.center().y);
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
        combine(expr->matchType(), expr->path());
    }

    void hashCombineCollator(const CollatorInterface* ci) {
        if (ci) {
            combine(ci->getSpec());
        }
    }

    void visitBitTest(const BitTestMatchExpression* expr) {
        hashCombineTypeAndPath(expr);

        if (_hashParamIds && expr->getBitPositionsParamId()) {
            // Either both parameter IDs are set, or neither are.
            combine(expr->getBitPositionsParamId().get(), expr->getBitMaskParamId().get());
        } else {
            // `equivalent()` function does not use the parameter IDs or BitMask.
            // `equivalent()` function compares sorted bitPositions.
            auto bitPositions = expr->getBitPositions();
            std::sort(bitPositions.begin(), bitPositions.end());
            combine(bitPositions);
        }
    }

    void visitComparison(const ComparisonMatchExpressionBase* expr) {
        hashCombineTypeAndPath(expr);

        if (_hashParamIds && expr->getInputParamId()) {
            combine(expr->getInputParamId().get());
        } else {
            // Please, keep BSONElementComparator consistent with
            // `ComparisonMatchExpressionBase::equivalent()`.
            const StringDataComparator* stringComparator = expr->getCollator();
            BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore,
                                         stringComparator);
            combine(eltCmp.hash(expr->getData()));
        }

        hashCombineCollator(expr->getCollator());
    }

    void visitText(const TextMatchExpressionBase* expr) {
        hashCombineTypeAndPath(expr);
        combine(expr->getFTSQuery());
    }

    void visitWhere(const WhereMatchExpressionBase* expr) {
        hashCombineTypeAndPath(expr);
        if (_hashParamIds && expr->getInputParamId()) {
            combine(expr->getInputParamId().get());
        } else {
            combine(expr->getCode());
        }
    }

    template <typename... Ts>
    void combine(const Ts&... values) {
        _hashState = H::combine(std::move(_hashState), values...);
    }

    H _hashState;

    const MatchExpressionHashParams& _params;
    const bool _hashParamIds;
};

/**
 * A utility struct used to pass additional parameters to the MatchExpression's hasher.
 */
struct AbslHashValueParams {
    const MatchExpression& exprToHash;
    const MatchExpressionHashParams& params;
};

template <typename H>
H AbslHashValue(H h, const AbslHashValueParams& toHash) {
    MatchExpressionHashVisitor<H> visitor{std::move(h), toHash.params};
    MatchExpressionWalker walker{&visitor, nullptr, nullptr};
    tree_walker::walk<true, MatchExpression>(&toHash.exprToHash, &walker);
    return visitor.extractHashState();
}

size_t calculateHash(const MatchExpression& expr, const MatchExpressionHashParams& params) {
    return absl::Hash<AbslHashValueParams>{}({expr, params});
}
}  // namespace mongo
