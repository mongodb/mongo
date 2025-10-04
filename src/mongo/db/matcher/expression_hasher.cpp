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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
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
#include "mongo/stdx/utility.h"
#include "mongo/util/assert_util.h"

#include <absl/hash/hash.h>
#include <boost/optional.hpp>

namespace mongo {
namespace {

/**
 * MatchExpression's hasher implementation compatible with absl::Hash.
 */
template <typename H>
class MatchExpressionHashVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionHashVisitor(H hashState, const MatchExpression::HashParam& hashParam)
        : _hashState(std::move(hashState)),
          _params(hashParam),
          _hashValues(_params.hashValuesOrParams & HashValuesOrParams::kHashValues),
          _hashParamIds(_params.hashValuesOrParams & HashValuesOrParams::kHashParamIds),
          _hashTags(_params.hashValuesOrParams & HashValuesOrParams::kHashIndexTags) {
        tassert(10192200,
                "Index list needs to be provided to hash index tags",
                !_hashTags || _params.indexes != nullptr);
    }

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
        hashCombineCommonProperties(expr);
        combine(expr->hasNull());
        hashCombineCollator(expr->getCollator());

        if (_hashParamIds && expr->getInputParamId()) {
            // There cannot be regexes when InMatchExpression is parameterized.
            combine(expr->getInputParamId().get());
        } else if (_hashValues) {
            // Hash the size of equalities's list and up to a maximum of
            // 'maxNumberOfHashedElements' evenly chosen equalities.
            BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore,
                                         expr->getCollator());
            const auto& equalities = expr->getEqualities();
            const size_t eqStep = std::max(static_cast<size_t>(1),
                                           equalities.size() / _params.maxNumberOfInElementsToHash);
            combine(equalities.size());
            for (size_t i = 0; i < equalities.size(); i += eqStep) {
                combine(eltCmp.hash(equalities[i]));
            }

            // Hash the size of regexes's list and up to a maximum of
            // 'maxNumberOfHashedElements' evenly chosen regexes.
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
        hashCombineCommonProperties(expr);

        if (_hashParamIds && expr->getDivisorInputParamId()) {
            // Either both parameter IDs are set, or neither are.
            combine(expr->getDivisorInputParamId().get(), expr->getRemainderInputParamId().get());
        } else if (_hashValues) {
            // `equivalent()` function does not use DivisorInputParamId and
            // RemainderInputParamId.
            combine(expr->getDivisor(), expr->getRemainder());
        }
    }

    void visit(const RegexMatchExpression* expr) final {
        hashCombineCommonProperties(expr);

        if (_hashParamIds && expr->getSourceRegexInputParamId()) {
            // Either both parameter IDs are set, or neither are.
            combine(expr->getSourceRegexInputParamId().get(),
                    expr->getCompiledRegexInputParamId().get());
        } else if (_hashValues) {
            // `equivalent()` function does not use SourceRegexInputParamId or
            // CompiledRegexInputParamId. `getString()` function returns value of regex field.
            combine(expr->getString());
        }

        combine(expr->getFlags());
    }

    void visit(const SizeMatchExpression* expr) final {
        hashCombineCommonProperties(expr);

        if (_hashParamIds && expr->getInputParamId()) {
            combine(expr->getInputParamId().get());
        } else if (_hashValues) {
            // `equivalent()` function does not use InputParamId. `getData()` returns value of
            // size field.
            combine(expr->getData());
        }
    }

    void visit(const TypeMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        if (_hashParamIds && expr->getInputParamId()) {
            combine(expr->getInputParamId().get());
        } else if (_hashValues) {
            // `equivalent()` function does not use InputParamId.
            combine(expr->typeSet());
        }
    }

    void visit(const WhereMatchExpression* expr) final {
        visitWhere(expr);
    }

    void visit(const AlwaysFalseMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const AlwaysTrueMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const AndMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const ElemMatchObjectMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const ElemMatchValueMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const ExistsMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const ExprMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        hashCombineCollator(expr->getExpressionContext()->getCollator());
        combine(*expr->getExpression());
    }
    void visit(const GeoMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(SimpleBSONObjComparator{}.hash(expr->rawObjForHashing()));
    }
    void visit(const GeoNearMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(SimpleBSONObjComparator{}.hash(expr->rawObjForHashing()));
    }
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
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
        hashCombineCommonProperties(expr);
        combine(expr->startIndex(), *expr->getExpression());
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->getNamePlaceholder(), *expr->getOtherwise());
        for (const auto& prop : expr->getProperties()) {
            combine(prop);
        }
        for (const auto& pat : expr->getPatternProperties()) {
            combine(pat.first.rawRegex, *pat.second.get());
        }
    }
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->typeSet());
    }
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->getBinDataSubType());
    }
    void visit(const InternalSchemaCondMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const InternalSchemaEqMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->getRhsElem());
    }
    void visit(const InternalSchemaFmodMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->getDivisor(), expr->getRemainder());
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->arrayIndex(), *expr->getExpression());
    }
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->getName(), expr->numItems());
    }
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->getName(), expr->strLen());
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->numProperties(), expr->getName());
    }
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->getName(), expr->numItems());
    }
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->getName(), expr->strLen());
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(expr->numProperties(), expr->getName());
    }
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(MatchExpressionHasher{}(expr->getChild(0)));
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
        combine(simpleHash(expr->getRhsObj()));
    }
    void visit(const InternalSchemaTypeExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const InternalSchemaXorMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const NorMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const NotMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const OrMatchExpression* expr) final {
        hashCombineCommonProperties(expr);
    }
    void visit(const TextMatchExpression* expr) final {
        visitText(expr);
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        visitText(expr);
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {
        hashCombineCommonProperties(expr);
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
    void hashCombineCommonProperties(const MatchExpression* expr);

    void hashCombineCollator(const CollatorInterface* ci) {
        if (ci) {
            combine(ci->getSpec());
        }
    }

    void visitBitTest(const BitTestMatchExpression* expr) {
        hashCombineCommonProperties(expr);

        if (_hashParamIds && expr->getBitPositionsParamId()) {
            // Either both parameter IDs are set, or neither are.
            combine(expr->getBitPositionsParamId().get(), expr->getBitMaskParamId().get());
        } else if (_hashValues) {
            // `equivalent()` function does not use the parameter IDs or BitMask.
            // `equivalent()` function compares sorted bitPositions.
            auto bitPositions = expr->getBitPositions();
            std::sort(bitPositions.begin(), bitPositions.end());
            combine(bitPositions);
        }
    }

    void visitComparison(const ComparisonMatchExpressionBase* expr) {
        hashCombineCommonProperties(expr);

        if (_hashParamIds && expr->getInputParamId()) {
            combine(expr->getInputParamId().get());
        } else if (_hashValues) {
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
        hashCombineCommonProperties(expr);
        combine(expr->getFTSQuery());
    }

    void visitWhere(const WhereMatchExpressionBase* expr) {
        hashCombineCommonProperties(expr);
        if (_hashParamIds && expr->getInputParamId()) {
            combine(expr->getInputParamId().get());
        } else if (_hashValues) {
            combine(expr->getCode());
        }
    }

    template <typename... Ts>
    void combine(const Ts&... values);

    H _hashState;

    const MatchExpression::HashParam& _params;
    const bool _hashValues;
    const bool _hashParamIds;
    const bool _hashTags;
};


/** See `RefWithHashParam` for information about these `hash` functions. */
namespace matcher_expression_hash {

/** Fallback */
template <typename H, typename T>
H hash(H h, const MatchExpression::HashParam&, const T& x) {
    return H::combine(std::move(h), x);
}

/**
 * MatcherTypeSet's hash function compatible with absl::Hash.
 */
template <typename H>
H hash(H h, const MatchExpression::HashParam&, const MatcherTypeSet& expr) {
    return H::combine(std::move(h), expr.getBSONTypeMask(), expr.allNumbers);
}

/**
 * Collation's hash function compatible with absl::Hash.
 */
template <typename H>
H hash(H h, const MatchExpression::HashParam&, const Collation& collation) {
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
H hash(H h, const MatchExpression::HashParam&, const Decimal128& dec) {
    return H::combine(std::move(h), dec.getValue().low64, dec.getValue().high64);
}

template <typename H>
H hash(H h, const MatchExpression::HashParam&, const ExpressionWithPlaceholder& dec) {
    return H::combine(std::move(h), MatchExpressionHasher{}(dec.getFilter()), dec.getPlaceholder());
}

template <typename H>
H hash(H h, const MatchExpression::HashParam&, const BSONElement& el) {
    return H::combine_contiguous(std::move(h), el.rawdata(), el.size());
}

template <typename H>
H hash(H h, const MatchExpression::HashParam& hashParams, const MatchExpression::TagData& x) {
    auto state = absl::HashState::Create(&h);
    x.hash(state, hashParams);
    return h;
}

/** Absl-compatible boost::optional */
template <typename H, typename T>
H hash(H h, const MatchExpression::HashParam& params, const boost::optional<T>& value) {
    if (value)
        h = hash(std::move(h), params, *value);
    return H::combine(std::move(h), bool{value});
}

template <typename H>
H hash(H h, const MatchExpression::HashParam& hashParams, const MatchExpression& x) {
    MatchExpressionHashVisitor<H> visitor{std::move(h), hashParams};
    MatchExpressionWalker walker{&visitor, nullptr, nullptr};
    tree_walker::walk<true, MatchExpression>(&x, &walker);
    return visitor.extractHashState();
}
}  // namespace matcher_expression_hash

/**
 * `MatchExpression` uses `absl::Hash` but has a requirement to customize the hashing of some
 * types. So we always hash values by combining instances of this wrapper type rather than
 * combining them directly.
 *
 * All hash combining is performed by the `matcher_expression_hash::hash` overloads. These
 * define custom hash behavior for several types that are defined outside of this API. These
 * model a `hash(h, param, x)` function that takes an abstract absl hash state `H` and a
 * `MatchExpression::HashParam param` as well as the `x` to be hashed. The `param` can be used
 * to optionally augment the hashing of some types with auxiliary information.
 */
template <typename T>
struct RefWithHashParam {
    template <typename H>
    friend H AbslHashValue(H h, const RefWithHashParam& x) {
        // Deliberately qualified call to disable ADL.
        return matcher_expression_hash::hash(std::move(h), x.hashParam, x.value);
    }

    const MatchExpression::HashParam& hashParam;
    const T& value;
};

template <typename H>
template <typename... Ts>
void MatchExpressionHashVisitor<H>::combine(const Ts&... values) {
    _hashState = H::combine(std::move(_hashState), RefWithHashParam<Ts>{_params, values}...);
}

template <typename H>
void MatchExpressionHashVisitor<H>::hashCombineCommonProperties(const MatchExpression* expr) {
    combine(expr->matchType(), expr->path());

    if (_hashTags && expr->getTag()) {
        combine(RefWithHashParam<MatchExpression::TagData>{_params, *expr->getTag()});
    }
}

}  // namespace

size_t calculateHash(const MatchExpression& expr, const MatchExpression::HashParam& params) {
    return absl::Hash<RefWithHashParam<MatchExpression>>{}({params, expr});
}
}  // namespace mongo
