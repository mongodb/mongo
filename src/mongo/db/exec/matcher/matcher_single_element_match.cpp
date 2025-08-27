/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_internal_eq_hashed_key.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/match_expression_util.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
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
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"


namespace mongo {

namespace exec::matcher {

namespace {

template <typename T>
bool evaluateComparisonExpressionSingleElement(const T* expr, const BSONElement& e) {
    const auto& rhs = expr->getData();
    if (e.type() != rhs.type()) {
        const auto ect = e.canonicalType();
        const auto rct = rhs.canonicalType();
        if (ect != rct) {
            // We can't call 'compareElements' on elements of different canonical types.  Usually
            // elements with different canonical types should never match any comparison, but there
            // are a few exceptions, handled here.

            // jstNULL and missing are treated the same.
            if (ect + rct == 5) {
                // At this point we know null (RHS) is being compared to either EOO (missing) or
                // undefined.
                return e.eoo() &&
                    (expr->matchType() == MatchExpression::EQ ||
                     expr->matchType() == MatchExpression::LTE ||
                     expr->matchType() == MatchExpression::GTE);
            }
            if (rhs.type() == BSONType::maxKey || rhs.type() == BSONType::minKey) {
                switch (expr->matchType()) {
                    // LT and LTE need no distinction here because the two elements that we are
                    // comparing do not even have the same canonical type and are thus not equal
                    // (i.e.the case where we compare MinKey against MinKey would not reach this
                    // switch statement at all).  The same reasoning follows for the lack of
                    // distinction between GTE and GT.
                    case MatchExpression::LT:
                    case MatchExpression::LTE:
                        return rhs.type() == BSONType::maxKey;
                    case MatchExpression::EQ:
                        return false;
                    case MatchExpression::GT:
                    case MatchExpression::GTE:
                        return rhs.type() == BSONType::minKey;
                    default:
                        // This is a comparison match expression, so it must be either
                        // a $lt, $lte, $gt, $gte, or equality expression.
                        MONGO_UNREACHABLE;
                }
            }
            return false;
        }
    }

    if (expr->matchType() == MatchExpression::EQ) {
        if (!expr->getCollator() && e.type() == BSONType::string) {
            // We know from above that _rhs must also be a String (or Symbol which has the same
            // representation) so if they have different value sizes, they must be different
            // strings. We can only stop here with the default collator, since other collators may
            // consider different length strings as equal.
            if (e.valuesize() != rhs.valuesize())
                return false;
        }
    } else {
        // Special case handling for NaN. NaN is equal to NaN but otherwise always compares to
        // false. This follows the normal comparison rules (where NaN is less than all numbers) for
        // EQ, so we only need to do this for other comparison types.
        const bool lhsIsNan =
            (((e.type() == BSONType::numberDouble) && (std::isnan(e._numberDouble())))) ||
            ((e.type() == BSONType::numberDecimal) && (e._numberDecimal().isNaN()));
        const bool rhsIsNan =
            (((rhs.type() == BSONType::numberDouble) && (std::isnan(rhs._numberDouble()))) ||
             ((rhs.type() == BSONType::numberDecimal) && (rhs._numberDecimal().isNaN())));
        if (lhsIsNan || rhsIsNan) {
            bool bothNaN = lhsIsNan && rhsIsNan;
            switch (expr->matchType()) {
                case MatchExpression::LT:
                case MatchExpression::GT:
                    return false;
                case MatchExpression::LTE:
                case MatchExpression::GTE:
                    return bothNaN;
                default:
                    // This is a comparison match expression, so it must be either
                    // a $lt, $lte, $gt, $gte, or equality expression.
                    fassertFailed(17448);
            }
        }
    }

    int x = BSONElement::compareElements(
        e, rhs, BSONElement::ComparisonRules::kConsiderFieldName, expr->getCollator());
    switch (expr->matchType()) {
        case MatchExpression::LT:
            return x < 0;
        case MatchExpression::LTE:
            return x <= 0;
        case MatchExpression::EQ:
            return x == 0;
        case MatchExpression::GT:
            return x > 0;
        case MatchExpression::GTE:
            return x >= 0;
        default:
            // This is a comparison match expression, so it must be either
            // a $lt, $lte, $gt, $gte, or equality expression.
            fassertFailed(16828);
    }
}
}  // namespace

void MatchesSingleElementEvaluator::visit(const EqualityMatchExpression* expr) {
    _result = evaluateComparisonExpressionSingleElement(expr, _elem);
}

void MatchesSingleElementEvaluator::visit(const GTEMatchExpression* expr) {
    _result = evaluateComparisonExpressionSingleElement(expr, _elem);
}

void MatchesSingleElementEvaluator::visit(const GTMatchExpression* expr) {
    _result = evaluateComparisonExpressionSingleElement(expr, _elem);
}

void MatchesSingleElementEvaluator::visit(const LTEMatchExpression* expr) {
    _result = evaluateComparisonExpressionSingleElement(expr, _elem);
}

void MatchesSingleElementEvaluator::visit(const LTMatchExpression* expr) {
    _result = evaluateComparisonExpressionSingleElement(expr, _elem);
}

void MatchesSingleElementEvaluator::visit(const RegexMatchExpression* expr) {
    switch (_elem.type()) {
        case BSONType::string:
        case BSONType::symbol:
            _result = !!expr->getRegex()->matchView(_elem.valueStringData());
            return;
        case BSONType::regEx:
            _result = expr->getString() == _elem.regex() && expr->getFlags() == _elem.regexFlags();
            return;
        default:
            _result = false;
            return;
    }
}

void MatchesSingleElementEvaluator::visit(const ModMatchExpression* expr) {
    if (!_elem.isNumber()) {
        _result = false;
        return;
    }

    long long dividend;
    if (_elem.type() == BSONType::numberDouble) {
        auto dividendDouble = _elem.Double();

        // If dividend is NaN or Infinity, then there is no match.
        if (!std::isfinite(dividendDouble)) {
            _result = false;
            return;
        }
        auto dividendLong = representAs<long long>(std::trunc(dividendDouble));

        // If the dividend value cannot be represented as a 64-bit integer, then we return false.
        if (!dividendLong) {
            _result = false;
            return;
        }
        dividend = *dividendLong;
    } else if (_elem.type() == BSONType::numberDecimal) {
        auto dividendDecimal = _elem.Decimal();

        // If dividend is NaN or Infinity, then there is no match.
        if (!dividendDecimal.isFinite()) {
            _result = false;
            return;
        }
        auto dividendLong =
            representAs<long long>(dividendDecimal.round(Decimal128::kRoundTowardZero));

        // If the dividend value cannot be represented as a 64-bit integer, then we return false.
        if (!dividendLong) {
            _result = false;
            return;
        }
        dividend = *dividendLong;
    } else {
        dividend = _elem.numberLong();
    }
    _result = overflow::safeMod(dividend, expr->getDivisor()) == expr->getRemainder();
}

void MatchesSingleElementEvaluator::visit(const InMatchExpression* expr) {
    // When an $in has a null, it adopts the same semantics as {$eq:null}. Namely, in addition to
    // matching literal null values, the $in should match missing.
    if (expr->hasNull() && _elem.eoo()) {
        _result = true;
        return;
    }
    if (expr->contains(_elem)) {
        _result = true;
        return;
    }
    for (auto&& regex : expr->getRegexes()) {
        if (matchesSingleElement(regex.get(), _elem, _details)) {
            _result = true;
            return;
        }
    }
    _result = false;
}

namespace {

/**
 * Helper function for performBitTest(...).
 *
 * needFurtherBitTests() determines if the result of a bit-test ('isBitSet') is enough
 * information to skip the rest of the bit tests.
 **/
template <typename T>
bool needFurtherBitTests(const T* expr, bool isBitSet) {
    const MatchExpression::MatchType mt = expr->matchType();

    return (isBitSet &&
            (mt == MatchExpression::BITS_ALL_SET || mt == MatchExpression::BITS_ANY_CLEAR)) ||
        (!isBitSet &&
         (mt == MatchExpression::BITS_ALL_CLEAR || mt == MatchExpression::BITS_ANY_SET));
}

/**
 * Performs bit test using bit positions on 'eValue' and returns whether or not the bit test
 * passes.
 */
template <typename T>
bool performBitTest(const T* expr, long long eValue) {
    const MatchExpression::MatchType mt = expr->matchType();

    uint64_t bitMask = expr->getBitMask();
    switch (mt) {
        case MatchExpression::BITS_ALL_SET:
            return (eValue & bitMask) == bitMask;
        case MatchExpression::BITS_ALL_CLEAR:
            return (~eValue & bitMask) == bitMask;
        case MatchExpression::BITS_ANY_SET:
            return eValue & bitMask;
        case MatchExpression::BITS_ANY_CLEAR:
            return ~eValue & bitMask;
        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Performs bit test using bit positions on 'eBinary' with length (in bytes) 'eBinaryLen' and
 * returns whether or not the bit test passes.
 */
template <typename T>
bool performBitTest(const T* expr, const char* eBinary, uint32_t eBinaryLen) {
    const MatchExpression::MatchType mt = expr->matchType();

    // Test each bit position.
    for (auto bitPosition : expr->getBitPositions()) {
        bool isBitSet;
        if (bitPosition >= eBinaryLen * 8) {
            // If position to test is longer than the data to test against, zero-extend.
            isBitSet = false;
        } else {
            // Map to byte position and bit position within that byte. Note that byte positions
            // start at position 0 in the char array, and bit positions start at the least
            // significant bit.
            int bytePosition = bitPosition / 8;
            int bit = bitPosition % 8;
            char byte = eBinary[bytePosition];

            isBitSet = byte & (1 << bit);
        }

        if (!needFurtherBitTests(expr, isBitSet)) {
            // If we can skip the rest fo the tests, that means we succeeded with _ANY_ or failed
            // with _ALL_.
            return mt == MatchExpression::BITS_ANY_SET || mt == MatchExpression::BITS_ANY_CLEAR;
        }
    }

    // If we finished all the tests, that means we succeeded with _ALL_ or failed with _ANY_.
    return mt == MatchExpression::BITS_ALL_SET || mt == MatchExpression::BITS_ALL_CLEAR;
}

template <typename T>
bool evaluateBitTestExpressionSingleElement(const T* expr,
                                            const BSONElement& e,
                                            MatchDetails* details) {
    // Validate 'e' is a number or a BinData.
    if (!e.isNumber() && e.type() != BSONType::binData) {
        return false;
    }

    if (e.type() == BSONType::binData) {
        int eBinaryLen;  // Length of eBinary (in bytes).
        const char* eBinary = e.binData(eBinaryLen);
        return performBitTest(expr, eBinary, eBinaryLen);
    }

    invariant(e.isNumber());

    if (e.type() == BSONType::numberDouble) {
        double eDouble = e.numberDouble();

        // NaN doubles are rejected.
        if (std::isnan(eDouble)) {
            return false;
        }

        // Integral doubles that are too large or small to be represented as a 64-bit signed
        // integer are treated as 0. We use 'kLongLongMaxAsDouble' because if we just did
        // eDouble > 2^63-1, it would be compared against 2^63. eDouble=2^63 would not get caught
        // that way.
        if (eDouble >= BSONElement::kLongLongMaxPlusOneAsDouble ||
            eDouble < std::numeric_limits<long long>::min()) {
            return false;
        }

        // This checks if e is an integral double.
        if (eDouble != static_cast<double>(static_cast<long long>(eDouble))) {
            return false;
        }
    } else if (e.type() == BSONType::numberDecimal) {
        Decimal128 eDecimal = e.numberDecimal();

        // NaN NumberDecimals are rejected.
        if (eDecimal.isNaN()) {
            return false;
        }

        // NumberDecimals that are too large or small to be represented as a 64-bit signed
        // integer are treated as 0.
        if (eDecimal > Decimal128(std::numeric_limits<long long>::max()) ||
            eDecimal < Decimal128(std::numeric_limits<long long>::min())) {
            return false;
        }

        // This checks if e is an integral NumberDecimal.
        if (eDecimal != eDecimal.round(Decimal128::kRoundTowardZero)) {
            return false;
        }
    }

    long long eValue = e.numberLong();
    return performBitTest(expr, eValue);
}
}  // namespace

void MatchesSingleElementEvaluator::visit(const BitsAllClearMatchExpression* expr) {
    _result = evaluateBitTestExpressionSingleElement(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const BitsAllSetMatchExpression* expr) {
    _result = evaluateBitTestExpressionSingleElement(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const BitsAnyClearMatchExpression* expr) {
    _result = evaluateBitTestExpressionSingleElement(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const BitsAnySetMatchExpression* expr) {
    _result = evaluateBitTestExpressionSingleElement(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const NotMatchExpression* expr) {
    _result = !matchesSingleElement(expr->getChild(0), _elem, _details);
}

namespace {
/**
 * ArrayMatchFunc is a lambda ([](const BSONObj& anArray, MatchDetails* details) -> bool) which
 * returns whether or not the nested array, represented as the object 'anArray', matches.
 * 'anArray' must be the nested array at this expression's path.
 */
template <typename T, typename ArrayMatchFunc>
bool arrayMatchesSingleElement(const T* expr,
                               const BSONElement& elt,
                               MatchDetails* details,
                               ArrayMatchFunc& matchesArray) {
    if (elt.type() != BSONType::array) {
        return false;
    }

    return matchesArray(elt.embeddedObject(), details);
}
}  // namespace

void MatchesSingleElementEvaluator::visit(const ElemMatchObjectMatchExpression* expr) {
    auto arrayMatchFunc = [&](const BSONObj& anArray, MatchDetails* details) -> bool {
        BSONObjIterator i(anArray);
        MatchExpression* sub = expr->getChild(0);
        while (i.more()) {
            BSONElement inner = i.next();
            if (!inner.isABSONObj()) {
                continue;
            }
            if (matchesBSON(sub, inner.Obj(), nullptr)) {
                if (details && details->needRecord()) {
                    details->setElemMatchKey(inner.fieldName());
                }
                return true;
            }
        }
        return false;
    };
    _result = arrayMatchesSingleElement(expr, _elem, _details, arrayMatchFunc);
}

void MatchesSingleElementEvaluator::visit(const ElemMatchValueMatchExpression* expr) {
    auto arrayElementMatchesAll = [&](const BSONElement& e) -> bool {
        for (unsigned i = 0; i < expr->numChildren(); i++) {
            if (!matchesSingleElement(expr->getChild(i), e)) {
                return false;
            }
        }
        return true;
    };
    auto arrayMatchFunc = [&](const BSONObj& anArray, MatchDetails* details) -> bool {
        BSONObjIterator i(anArray);
        while (i.more()) {
            BSONElement inner = i.next();

            if (arrayElementMatchesAll(inner)) {
                if (details && details->needRecord()) {
                    details->setElemMatchKey(inner.fieldName());
                }
                return true;
            }
        }
        return false;
    };
    _result = arrayMatchesSingleElement(expr, _elem, _details, arrayMatchFunc);
}

void MatchesSingleElementEvaluator::visit(const SizeMatchExpression* expr) {
    auto arrayMatchFunc = [&](const BSONObj& anArray, MatchDetails* details) -> bool {
        if (expr->getData() < 0) {
            return false;
        }
        return anArray.nFields() == expr->getData();
    };
    _result = arrayMatchesSingleElement(expr, _elem, _details, arrayMatchFunc);
}

BSONElement findFirstMismatchInArray(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr,
                                     const BSONObj& anArray,
                                     MatchDetails* details) {
    auto iter = BSONObjIterator(anArray);
    match_expression_util::advanceBy(expr->startIndex(), iter);
    while (iter.more()) {
        auto element = iter.next();
        if (!matchesBSONElement(expr->getExpression()->getFilter(), element, details)) {
            return element;
        }
    }
    return {};
}

void MatchesSingleElementEvaluator::visit(
    const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) {

    auto arrayMatchFunc = [&](const BSONObj& anArray, MatchDetails* details) -> bool {
        return !findFirstMismatchInArray(expr, anArray, details);
    };
    _result = arrayMatchesSingleElement(expr, _elem, _details, arrayMatchFunc);
}

void MatchesSingleElementEvaluator::visit(
    const InternalSchemaMatchArrayIndexMatchExpression* expr) {
    /**
     * Matches 'anArray' if the element at 'expr->arrayIndex()' matches 'expr->getExpression()',
     * or if its size is less than 'expr->arrayIndex()'.
     */
    auto arrayMatchFunc = [&](const BSONObj& anArray, MatchDetails* details) -> bool {
        BSONElement element;
        auto iterator = BSONObjIterator(anArray);

        // Skip ahead to the element we want, bailing early if there aren't enough elements.
        for (auto i = 0LL; i <= expr->arrayIndex(); ++i) {
            if (!iterator.more()) {
                return true;
            }
            element = iterator.next();
        }

        return matchesBSONElement(expr->getExpression()->getFilter(), element, details);
    };
    _result = arrayMatchesSingleElement(expr, _elem, _details, arrayMatchFunc);
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaMaxItemsMatchExpression* expr) {
    auto arrayMatchFunc = [&](const BSONObj& anArray, MatchDetails* details) -> bool {
        return (anArray.nFields() <= expr->numItems());
    };
    _result = arrayMatchesSingleElement(expr, _elem, _details, arrayMatchFunc);
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaMinItemsMatchExpression* expr) {
    auto arrayMatchFunc = [&](const BSONObj& anArray, MatchDetails* details) -> bool {
        return (anArray.nFields() >= expr->numItems());
    };
    _result = arrayMatchesSingleElement(expr, _elem, _details, arrayMatchFunc);
}


BSONElement findFirstDuplicateValue(const InternalSchemaUniqueItemsMatchExpression* expr,
                                    const BSONObj& array) {
    auto set = expr->getComparator().makeBSONEltSet();
    for (auto&& elem : array) {
        if (!get<bool>(set.insert(elem))) {
            return elem;
        }
    }
    return {};
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaUniqueItemsMatchExpression* expr) {
    auto arrayMatchFunc = [&](const BSONObj& anArray, MatchDetails* details) -> bool {
        return !findFirstDuplicateValue(expr, anArray);
    };
    _result = arrayMatchesSingleElement(expr, _elem, _details, arrayMatchFunc);
}

namespace {

template <typename T>
bool evaluateInternalExprComparisonMatchExpression(const T* expr,
                                                   const BSONElement& elem,
                                                   MatchDetails* details) {
    // We use NonLeafArrayBehavior::kMatchSubpath traversal in the internal expr comparison match
    // expressions. This means matchesSingleElement() will be called
    // when an array is found anywhere along the patch we are matching against. When this
    // occurs, we return 'true' and depend on the corresponding ExprMatchExpression node to
    // filter properly.
    if (elem.type() == BSONType::array) {
        return true;
    }

    auto comp =
        elem.woCompare(expr->getData(), BSONElement::ComparisonRulesSet(0), expr->getCollator());

    switch (expr->matchType()) {
        case MatchExpression::INTERNAL_EXPR_GT:
            return comp > 0;
        case MatchExpression::INTERNAL_EXPR_GTE:
            return comp >= 0;
        case MatchExpression::INTERNAL_EXPR_LT:
            return comp < 0;
        case MatchExpression::INTERNAL_EXPR_LTE:
            return comp <= 0;
        case MatchExpression::INTERNAL_EXPR_EQ:
            return comp == 0;
        default:
            // This is a comparison match expression, so it must be either a $eq, $lt, $lte, $gt
            // or $gte expression.
            MONGO_UNREACHABLE_TASSERT(3994308);
    }
}
}  // namespace

void MatchesSingleElementEvaluator::visit(const InternalExprEqMatchExpression* expr) {
    _result = evaluateInternalExprComparisonMatchExpression(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const InternalExprGTMatchExpression* expr) {
    _result = evaluateInternalExprComparisonMatchExpression(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const InternalExprGTEMatchExpression* expr) {
    _result = evaluateInternalExprComparisonMatchExpression(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const InternalExprLTMatchExpression* expr) {
    _result = evaluateInternalExprComparisonMatchExpression(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const InternalExprLTEMatchExpression* expr) {
    _result = evaluateInternalExprComparisonMatchExpression(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const InternalEqHashedKey* expr) {
    const auto& rhs = expr->getData();
    tassert(7281401, "hashed value must be a long", rhs.type() == BSONType::numberLong);
    const auto hashVal = BSONElementHasher::hash64(_elem, BSONElementHasher::DEFAULT_HASH_SEED);
    _result = hashVal == rhs.numberLong();
}

void MatchesSingleElementEvaluator::visit(
    const InternalSchemaAllowedPropertiesMatchExpression* expr) {
    if (_elem.type() != BSONType::object) {
        _result = false;
        return;
    }

    _result = matchesBSONObj(expr, _elem.embeddedObject());
}

/**
 * If the input object matches 'condition', returns the result of matching it against
 * 'thenBranch'. Otherwise, returns the result of matching it against 'elseBranch'.
 */
void MatchesSingleElementEvaluator::visit(const InternalSchemaCondMatchExpression* expr) {
    _result = matchesSingleElement(expr->condition(), _elem, _details)
        ? matchesSingleElement(expr->thenBranch(), _elem, _details)
        : matchesSingleElement(expr->elseBranch(), _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaEqMatchExpression* expr) {
    _result = expr->getComparator().evaluate(expr->getRhsElem() == _elem);
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaFmodMatchExpression* expr) {
    if (!_elem.isNumber()) {
        _result = false;
        return;
    }
    std::uint32_t flags = Decimal128::SignalingFlag::kNoFlag;
    Decimal128 dec = _elem.numberDecimal().modulo(expr->getDivisor(), &flags);
    if (flags == Decimal128::SignalingFlag::kNoFlag) {
        _result = dec.isEqual(expr->getRemainder());
        return;
    }
    _result = false;
}

namespace {
template <typename T>
bool evaluateInternalSchemaStrLengthMatchExpressionSingleElement(const T* expr,
                                                                 const BSONElement& elem,
                                                                 MatchDetails* details) {
    if (elem.type() != BSONType::string) {
        return false;
    }

    auto len = str::lengthInUTF8CodePoints(elem.valueStringData());
    return expr->getComparator()(len);
};
}  // namespace

void MatchesSingleElementEvaluator::visit(const InternalSchemaMaxLengthMatchExpression* expr) {
    _result = evaluateInternalSchemaStrLengthMatchExpressionSingleElement(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaMinLengthMatchExpression* expr) {
    _result = evaluateInternalSchemaStrLengthMatchExpressionSingleElement(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaMaxPropertiesMatchExpression* expr) {
    if (_elem.type() != BSONType::object) {
        _result = false;
        return;
    }
    _result = (_elem.embeddedObject().nFields() <= expr->numProperties());
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaMinPropertiesMatchExpression* expr) {
    if (_elem.type() != BSONType::object) {
        _result = false;
        return;
    }
    _result = (_elem.embeddedObject().nFields() >= expr->numProperties());
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaObjectMatchExpression* expr) {
    if (_elem.type() != BSONType::object) {
        _result = false;
        return;
    }
    _result = matchesBSON(expr->getChild(0), _elem.Obj());
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaXorMatchExpression* expr) {
    bool found = false;
    for (size_t i = 0; i < expr->numChildren(); i++) {
        if (matchesSingleElement(expr->getChild(i), _elem, _details)) {
            if (found) {
                _result = false;
                return;
            }
            found = true;
        }
    }
    _result = found;
}


namespace {

template <typename T>
bool evaluateTypeMatchExpression(const T* expr, const BSONElement& e, MatchDetails* details) {
    return expr->typeSet().hasType(e.type());
}
}  // namespace

void MatchesSingleElementEvaluator::visit(const TypeMatchExpression* expr) {
    _result = evaluateTypeMatchExpression(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaTypeExpression* expr) {
    _result = evaluateTypeMatchExpression(expr, _elem, _details);
}

void MatchesSingleElementEvaluator::visit(const InternalSchemaBinDataSubTypeExpression* expr) {
    _result = _elem.type() == BSONType::binData && _elem.binDataType() == expr->getBinDataSubType();
}

void MatchesSingleElementEvaluator::visit(
    const InternalSchemaBinDataEncryptedTypeExpression* expr) {
    if (_elem.type() != BSONType::binData) {
        _result = false;
        return;
    }

    if (_elem.binDataType() != BinDataType::Encrypt) {
        _result = false;
        return;
    }

    int binDataLen;
    auto binData = _elem.binData(binDataLen);
    if (static_cast<size_t>(binDataLen) < sizeof(FleBlobHeader)) {
        _result = false;
        return;
    }

    auto fleBlobSubType = EncryptedBinDataType_parse(binData[0], IDLParserContext("subtype"));
    switch (fleBlobSubType) {
        case EncryptedBinDataType::kDeterministic:
        case EncryptedBinDataType::kRandom: {
            // Verify the type of the encrypted data.
            auto fleBlob = reinterpret_cast<const FleBlobHeader*>(binData);
            _result = expr->typeSet().hasType(static_cast<BSONType>(fleBlob->originalBsonType));
            return;
        }
        default:
            _result = false;
    }
}

void MatchesSingleElementEvaluator::visit(
    const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) {
    if (_elem.type() != BSONType::binData) {
        _result = false;
        return;
    }

    if (_elem.binDataType() != BinDataType::Encrypt) {
        _result = false;
        return;
    }

    int binDataLen;
    auto binData = _elem.binData(binDataLen);
    if (static_cast<size_t>(binDataLen) < sizeof(FleBlobHeader)) {
        _result = false;
        return;
    }

    EncryptedBinDataType subTypeByte = static_cast<EncryptedBinDataType>(binData[0]);
    switch (subTypeByte) {
        case EncryptedBinDataType::kFLE2EqualityIndexedValue:
        case EncryptedBinDataType::kFLE2RangeIndexedValue:
        case EncryptedBinDataType::kFLE2EqualityIndexedValueV2:
        case EncryptedBinDataType::kFLE2RangeIndexedValueV2:
        case EncryptedBinDataType::kFLE2TextIndexedValue:
        case EncryptedBinDataType::kFLE2UnindexedEncryptedValue:
        case EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2: {
            // Verify the type of the encrypted data.
            if (expr->typeSet().isEmpty()) {
                _result = true;
                return;
            } else {
                auto fleBlob = reinterpret_cast<const FleBlobHeader*>(binData);
                _result = expr->typeSet().hasType(static_cast<BSONType>(fleBlob->originalBsonType));
                return;
            }
        }
        default:
            _result = false;
    }
}

}  // namespace exec::matcher
}  // namespace mongo
