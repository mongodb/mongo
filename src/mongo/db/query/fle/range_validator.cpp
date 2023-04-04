/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "range_validator.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/util/assert_util.h"
#include <unordered_map>

namespace mongo {
namespace fle {
namespace {

template <class T>
bool hasRangeTypeToValidate(const T& value) {
    auto typeToValidate = EncryptedBinDataType::kFLE2FindRangePayloadV2;
    return isPayloadOfType(typeToValidate, value);
}

MatchExpression::MatchType rangeOpToMatchType(Fle2RangeOperator op) {
    switch (op) {
        case Fle2RangeOperator::kGt:
            return MatchExpression::GT;
        case Fle2RangeOperator::kGte:
            return MatchExpression::GTE;
        case Fle2RangeOperator::kLt:
            return MatchExpression::LT;
        case Fle2RangeOperator::kLte:
            return MatchExpression::LTE;
    }
    MONGO_UNREACHABLE_TASSERT(7030720);
}

Fle2RangeOperator matchTypeToRangeOp(MatchExpression::MatchType ty) {
    switch (ty) {
        case MatchExpression::GT:
            return Fle2RangeOperator::kGt;
        case MatchExpression::GTE:
            return Fle2RangeOperator::kGte;
        case MatchExpression::LT:
            return Fle2RangeOperator::kLt;
        case MatchExpression::LTE:
            return Fle2RangeOperator::kLte;
        default:
            break;
    }
    MONGO_UNREACHABLE_TASSERT(7030714);
}

void validateOneSidedRange(Fle2RangeOperator ty, const ParsedFindRangePayload& payload) {
    uassert(
        7030709, "One-sided range comparison cannot be a stub payload.", payload.edges.has_value());
    uassert(7030710,
            "One-sided range comparison can only have one valid operator.",
            !payload.secondOp.has_value());
    uassert(7030711,
            str::stream() << "Payload generated for " << payload.firstOp << " but was found under "
                          << ty,
            ty == payload.firstOp);
}

void validateOneSidedRange(const ComparisonMatchExpression& expr) {
    auto data = expr.getData();
    if (!hasRangeTypeToValidate(data)) {
        return;
    }
    auto payload = parseFindPayload<ParsedFindRangePayload>(data);
    validateOneSidedRange(matchTypeToRangeOp(expr.matchType()), payload);
}

/**
 * This struct holds information on BinData blobs with a specific payloadId within a $and
 * conjunction. For a two-sided range to be valid, there must be exactly two blobs where:
 *  1. One blob is a full payload.
 *  2. One blob is a stub.
 *  3. One blob is present under each endpoint operator that was specified when the blobs were
 *     generated client-side. This is to ensure that the syntax of the query an the encrypted
 *     semantics match.
 *
 * If any of these conditions are violated, a validation error is sent back to the
 * client so that users can re-generate the encrypted range payload for their query.
 */
struct RangePayloadValidator {
    Fle2RangeOperator firstOp;
    Fle2RangeOperator secondOp;

    bool seenFirstOp = false;
    bool seenSecondOp = false;
    bool seenPayload = false;
    bool seenStub = false;

    RangePayloadValidator(Fle2RangeOperator firstOp, Fle2RangeOperator secondOp)
        : firstOp(firstOp), secondOp(secondOp) {
        switch (firstOp) {
            case Fle2RangeOperator::kGt:
            case Fle2RangeOperator::kGte:
                uassert(7030700,
                        "Two-sided range predicate must have both lower and upper bounds.",
                        secondOp == Fle2RangeOperator::kLt || secondOp == Fle2RangeOperator::kLte);
                break;
            case Fle2RangeOperator::kLt:
            case Fle2RangeOperator::kLte:
                uassert(7030701,
                        "Two-sided range predicate must have both lower and upper bounds.",
                        secondOp == Fle2RangeOperator::kGt || secondOp == Fle2RangeOperator::kGte);
                break;
        }
    }

    /**
     * Mark a specific payload as having been seen under a given operator for this validator. If a
     * payload is valid, this function should be called exactly twice for every struct instance,
     * once under a $gt or $gte and once under a $lt or $lte.
     */
    void update(Fle2RangeOperator op, const ParsedFindRangePayload& payload) {
        uassert(7030702,
                "Both payloads in a two-sided range must be generated together.",
                payload.firstOp == this->firstOp);
        uassert(7030703,
                "Both payloads in a two-sided range must be generated together.",
                payload.secondOp.has_value());
        uassert(7030704,
                "Both payloads in a two-sided range must be generated together.",
                payload.secondOp.value() == this->secondOp);

        if (op == firstOp) {
            uassert(7030705,
                    str::stream() << "A payload cannot appear under multiple " << op
                                  << " operators.",
                    !seenFirstOp);
            seenFirstOp = true;
        } else if (op == secondOp) {
            uassert(7030706,
                    str::stream() << "A payload cannot appear under multiple " << op
                                  << " operators.",
                    !seenSecondOp);
            seenSecondOp = true;
        } else {
            uasserted(7030716,
                      str::stream() << "Payload generated for " << firstOp << " and " << secondOp
                                    << " but was found under " << op << ".");
        }

        if (payload.edges.has_value()) {
            uassert(7030707, "Payload should only appear once in query.", !seenPayload);
            seenPayload = true;
        } else {
            uassert(7030708, "Stub should only appear once in query.", !seenStub);
            seenStub = true;
        }
    }

    bool isValid() const {
        return seenFirstOp && seenSecondOp && seenPayload && seenStub;
    }
};

using ValidatorMap = stdx::unordered_map<int32_t, RangePayloadValidator>;

void updateValidatorForPayload(ValidatorMap& map,
                               Fle2RangeOperator rangeOp,
                               ParsedFindRangePayload& payload) {
    if (!payload.secondOp.has_value()) {
        // If there is no secondOp in this payload then it should be treated as a
        // one-sided range that should be validated on its own.
        validateOneSidedRange(rangeOp, payload);
        return;
    }

    // At this point, we know that the payload is one side of a two-sided range.

    // Create a new validator for this payloadId if it's the first time it's seen.
    auto payloadEntry = map.find(payload.payloadId);
    if (payloadEntry == map.end()) {
        payloadEntry =
            map.insert({payload.payloadId,
                        RangePayloadValidator(payload.firstOp, payload.secondOp.value())})
                .first;
    }
    // Update the validator with information from this payload.
    payloadEntry->second.update(rangeOp, payload);
}

void validateTwoSidedRanges(const AndMatchExpression& expr) {
    // Keep track of a map from payloadId to the validator struct.
    ValidatorMap payloads;
    for (size_t i = 0; i < expr.numChildren(); i++) {
        auto child = expr.getChild(i);
        switch (child->matchType()) {
            case MatchExpression::GT:
            case MatchExpression::GTE:
            case MatchExpression::LT:
            case MatchExpression::LTE: {
                auto compExpr = dynamic_cast<const ComparisonMatchExpression*>(child);
                tassert(7030717, "Expression must be a comparison expression.", compExpr);
                auto data = compExpr->getData();
                if (!hasRangeTypeToValidate(data)) {
                    // Skip any comparison operators over non-encrypted data.
                    continue;
                }

                auto payload = parseFindPayload<ParsedFindRangePayload>(data);
                updateValidatorForPayload(
                    payloads, matchTypeToRangeOp(compExpr->matchType()), payload);
                break;
            }
            default:
                // Make sure to recursively handle other children in case there are further nestings
                // of $not, $nor, $or or $and.
                validateRanges(*child);
                break;
        }
    }

    // Once the entire operand list of the $and is traversed, make sure that all the
    // two-sided ranges had fully valid payloads.
    for (auto& [_, validator] : payloads) {
        uassert(7030715,
                str::stream() << "Payloads must be regenerated every time a query is modified.",
                validator.isValid());
    }
}
}  // namespace


void validateRanges(const MatchExpression& expr) {
    switch (expr.matchType()) {
        case MatchExpression::GT:
        case MatchExpression::GTE:
        case MatchExpression::LT:
        case MatchExpression::LTE: {
            auto compExpr = dynamic_cast<const ComparisonMatchExpression*>(&expr);
            tassert(7030712, "Expression must be a comparison expression.", compExpr);
            validateOneSidedRange(*compExpr);
            break;
        }
        case MatchExpression::AND: {
            auto andExpr = dynamic_cast<const AndMatchExpression*>(&expr);
            tassert(7030713, "Expression must be a $and expression.", andExpr);
            validateTwoSidedRanges(*andExpr);
            break;
        }
        case MatchExpression::OR:
        case MatchExpression::NOT:
        case MatchExpression::NOR: {
            for (size_t i = 0; i < expr.numChildren(); ++i) {
                validateRanges(*expr.getChild(i));
            }
            break;
        }
        default:
            break;
    }
}

namespace {

Fle2RangeOperator cmpOpToRangeOp(ExpressionCompare::CmpOp op) {
    switch (op) {
        case ExpressionCompare::GT:
            return Fle2RangeOperator::kGt;
        case ExpressionCompare::GTE:
            return Fle2RangeOperator::kGte;
        case ExpressionCompare::LT:
            return Fle2RangeOperator::kLt;
        case ExpressionCompare::LTE:
            return Fle2RangeOperator::kLte;
        case ExpressionCompare::EQ:
        case ExpressionCompare::NE:
        case ExpressionCompare::CMP:
            break;
    }
    MONGO_UNREACHABLE_TASSERT(7030800);
}

// Gets the first constant value from a comparison. All enrypted range predicates will have one
// fieldpath and one constant value. If no constant is found, returns none. If both are values, then
// this is not an encrypted predicate, but that case is handled by the caller.
boost::optional<Value> getFirstConstantFromComparison(const ExpressionCompare& expr) {
    auto& ops = expr.getChildren();
    auto leftConstant = dynamic_cast<ExpressionConstant*>(ops[0].get());
    auto rightConstant = dynamic_cast<ExpressionConstant*>(ops[1].get());

    if (leftConstant) {
        return leftConstant->getValue();
    } else if (rightConstant) {
        return rightConstant->getValue();
    } else {
        return boost::none;
    }
}

void validateOneSidedRange(const ExpressionCompare& expr) {
    auto v = getFirstConstantFromComparison(expr);
    if (!v.has_value()) {
        return;
    }

    if (!hasRangeTypeToValidate(v.value())) {
        return;
    }

    auto payload = parseFindPayload<ParsedFindRangePayload>(v.value());

    validateOneSidedRange(cmpOpToRangeOp(expr.getOp()), payload);
}


void validateTwoSidedRanges(const ExpressionAnd& expr) {
    ValidatorMap payloads;
    for (auto& child : expr.getOperandList()) {
        if (auto compExpr = dynamic_cast<const ExpressionCompare*>(child.get())) {
            switch (compExpr->getOp()) {
                case ExpressionCompare::GT:
                case ExpressionCompare::GTE:
                case ExpressionCompare::LT:
                case ExpressionCompare::LTE: {
                    auto data = getFirstConstantFromComparison(*compExpr);
                    if (!data.has_value()) {
                        continue;
                    }
                    if (!hasRangeTypeToValidate(data.value())) {
                        continue;
                    }
                    auto payload = parseFindPayload<ParsedFindRangePayload>(data.value());
                    updateValidatorForPayload(payloads, cmpOpToRangeOp(compExpr->getOp()), payload);
                    break;
                }
                case ExpressionCompare::EQ:
                case ExpressionCompare::NE:
                case ExpressionCompare::CMP:
                    continue;
            }
        } else {
            validateRanges(*child.get());
        }
    }
    // Once the entire operand list of the $and is traversed, make sure that all the
    // two-sided ranges had fully valid payloads.
    for (auto& [_, validator] : payloads) {
        uassert(7030718,
                str::stream() << "Payloads must be regenerated every time a query is modified.",
                validator.isValid());
    }
}
}  // namespace

void validateRanges(const Expression& expr) {
    if (auto compExpr = dynamic_cast<const ExpressionCompare*>(&expr)) {
        switch (compExpr->getOp()) {
            case ExpressionCompare::GT:
            case ExpressionCompare::GTE:
            case ExpressionCompare::LT:
            case ExpressionCompare::LTE: {
                validateOneSidedRange(*compExpr);
                break;
            }
            case ExpressionCompare::CMP:
            case ExpressionCompare::EQ:
            case ExpressionCompare::NE:
                break;
        }
    } else if (auto andExpr = dynamic_cast<const ExpressionAnd*>(&expr)) {
        validateTwoSidedRanges(*andExpr);
    } else {
        for (auto& child : expr.getChildren()) {
            validateRanges(*child.get());
        }
    }
}
}  // namespace fle
}  // namespace mongo
