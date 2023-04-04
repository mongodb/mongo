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

#include "equality_predicate.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_tags.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::fle {

REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE(EQ, EqualityPredicate);
REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE(MATCH_IN, EqualityPredicate);
REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE(ExpressionCompare, EqualityPredicate);
REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE(ExpressionIn, EqualityPredicate);

std::vector<PrfBlock> EqualityPredicate::generateTags(BSONValue payload) const {
    ParsedFindEqualityPayload tokens = parseFindPayload<ParsedFindEqualityPayload>(payload);

    return readTags(_rewriter->getTagQueryInterface(),
                    _rewriter->getESCNss(),
                    tokens.escToken,
                    tokens.edcToken,
                    tokens.maxCounter);
}

std::unique_ptr<MatchExpression> EqualityPredicate::rewriteToTagDisjunction(
    MatchExpression* expr) const {
    switch (expr->matchType()) {
        case MatchExpression::EQ: {
            auto eqExpr = static_cast<EqualityMatchExpression*>(expr);
            auto payload = eqExpr->getData();
            if (!isPayload(payload)) {
                return nullptr;
            }
            return makeTagDisjunction(toBSONArray(generateTags(payload)));
        }
        case MatchExpression::MATCH_IN: {
            auto inExpr = static_cast<InMatchExpression*>(expr);
            size_t numFFPs = 0;
            for (auto& eq : inExpr->getEqualities()) {
                if (isPayload(eq)) {
                    ++numFFPs;
                }
            }
            if (numFFPs == 0) {
                return nullptr;
            }
            // All elements in an encrypted $in expression should be FFPs.
            uassert(6329400,
                    "If any elements in a $in expression are encrypted, then all elements should "
                    "be encrypted.",
                    numFFPs == inExpr->getEqualities().size());
            auto backingBSONBuilder = BSONArrayBuilder();

            for (auto& eq : inExpr->getEqualities()) {
                auto obj = generateTags(eq);
                for (auto&& elt : obj) {
                    backingBSONBuilder.appendBinData(elt.size(), BinDataGeneral, elt.data());
                }
            }

            auto backingBSON = backingBSONBuilder.arr();
            return makeTagDisjunction(std::move(backingBSON));
        }
        default:
            MONGO_UNREACHABLE_TASSERT(6911300);
    }
    MONGO_UNREACHABLE_TASSERT(6911302);
};

namespace {
template <typename PayloadT>
boost::intrusive_ptr<ExpressionInternalFLEEqual> generateFleEqualMatch(StringData path,
                                                                       const PayloadT& ffp,
                                                                       ExpressionContext* expCtx) {
    auto tokens = ParsedFindEqualityPayload(ffp);

    // Generate { $_internalFleEq: { field: "$field_name", server:  F_s[ f, 2, v, 2 ] }
    return make_intrusive<ExpressionInternalFLEEqual>(
        expCtx,
        ExpressionFieldPath::createPathFromString(
            expCtx, path.toString(), expCtx->variablesParseState),
        FLEServerMetadataEncryptionTokenGenerator::generateServerZerosEncryptionToken(
            tokens.serverDataDerivedToken));
}


template <typename PayloadT>
std::unique_ptr<ExpressionInternalFLEEqual> generateFleEqualMatchUnique(StringData path,
                                                                        const PayloadT& ffp,
                                                                        ExpressionContext* expCtx) {
    auto tokens = ParsedFindEqualityPayload(ffp);

    // Generate { $_internalFleEq: { field: "$field_name", server:  F_s[ f, 2, v, 2 ] }
    return std::make_unique<ExpressionInternalFLEEqual>(
        expCtx,
        ExpressionFieldPath::createPathFromString(
            expCtx, path.toString(), expCtx->variablesParseState),
        FLEServerMetadataEncryptionTokenGenerator::generateServerZerosEncryptionToken(
            tokens.serverDataDerivedToken));
}

std::unique_ptr<MatchExpression> generateFleEqualMatchAndExpr(StringData path,
                                                              const BSONElement ffp,
                                                              ExpressionContext* expCtx) {
    auto fleEqualMatch = generateFleEqualMatch(path, ffp, expCtx);

    return std::make_unique<ExprMatchExpression>(fleEqualMatch, expCtx);
}
}  // namespace

std::unique_ptr<MatchExpression> EqualityPredicate::rewriteToRuntimeComparison(
    MatchExpression* expr) const {
    switch (expr->matchType()) {
        case MatchExpression::EQ: {
            auto eqExpr = static_cast<EqualityMatchExpression*>(expr);
            auto payload = eqExpr->getData();
            if (!isPayload(payload)) {
                return nullptr;
            }
            return generateFleEqualMatchAndExpr(
                eqExpr->path(), payload, _rewriter->getExpressionContext());
        }
        case MatchExpression::MATCH_IN: {
            auto inExpr = static_cast<InMatchExpression*>(expr);
            size_t numFFPs = 0;
            for (auto& eq : inExpr->getEqualities()) {
                if (isPayload(eq)) {
                    ++numFFPs;
                }
            }
            if (numFFPs == 0) {
                return nullptr;
            }
            uassert(6911301,
                    "If any elements in a $in expression are encrypted, then all elements should "
                    "be encrypted.",
                    numFFPs == inExpr->getEqualities().size());
            std::vector<std::unique_ptr<MatchExpression>> matches;
            matches.reserve(numFFPs);

            for (auto& eq : inExpr->getEqualities()) {
                auto exprMatch = generateFleEqualMatchAndExpr(
                    expr->path(), eq, _rewriter->getExpressionContext());
                matches.push_back(std::move(exprMatch));
            }

            auto orExpr = std::make_unique<OrMatchExpression>(std::move(matches));
            return orExpr;
        }
        default:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

/*
 * Helper function for code shared between tag disjunction and runtime evaluation for the equality
 * case.
 */
boost::optional<std::pair<ExpressionFieldPath*, ExpressionConstant*>>
EqualityPredicate::extractDetailsFromComparison(ExpressionCompare* expr) const {
    auto equalitiesList = expr->getChildren();

    auto leftConstant = dynamic_cast<ExpressionConstant*>(equalitiesList[0].get());
    auto rightConstant = dynamic_cast<ExpressionConstant*>(equalitiesList[1].get());

    bool isLeftFFP = leftConstant && isPayload(leftConstant->getValue());
    bool isRightFFP = rightConstant && isPayload(rightConstant->getValue());

    uassert(6334100,
            "Cannot compare two encrypted constants to each other",
            !(isLeftFFP && isRightFFP));

    // No FLE Find Payload
    if (!isLeftFFP && !isRightFFP) {
        return boost::none;
    }

    auto leftFieldPath = dynamic_cast<ExpressionFieldPath*>(equalitiesList[0].get());
    auto rightFieldPath = dynamic_cast<ExpressionFieldPath*>(equalitiesList[1].get());

    uassert(6672413,
            "Queryable Encryption only supports comparisons between a field path and a constant",
            leftFieldPath || rightFieldPath);
    auto fieldPath = leftFieldPath ? leftFieldPath : rightFieldPath;
    auto constChild = isLeftFFP ? leftConstant : rightConstant;
    return {{fieldPath, constChild}};
}

/**
 * Perform validation on $in add expressions, and return a pre-processed fieldpath expression for
 * use by the rewrite. This factors out common validation code for the runtime and tag rewrite
 * cases.
 */
boost::optional<const ExpressionFieldPath*> EqualityPredicate::validateIn(
    ExpressionIn* inExpr, ExpressionArray* inList) const {
    auto leftExpr = inExpr->getOperandList()[0].get();
    auto& equalitiesList = inList->getChildren();
    size_t numFFPs = 0;

    for (auto& equality : equalitiesList) {
        // For each expression representing a FleFindPayload...
        if (auto constChild = dynamic_cast<ExpressionConstant*>(equality.get())) {
            if (!isPayload(constChild->getValue())) {
                continue;
            }

            numFFPs++;
        }
    }

    // Finally, construct an $or of all of the $ins.
    if (numFFPs == 0) {
        return boost::none;
    }

    uassert(6334102,
            "If any elements in an comparison expression are encrypted, then all elements "
            "should "
            "be encrypted.",
            numFFPs == equalitiesList.size());

    auto leftFieldPath = dynamic_cast<const ExpressionFieldPath*>(leftExpr);
    uassert(6672417,
            "$in is only supported with Queryable Encryption when the first argument is a "
            "field path",
            leftFieldPath != nullptr);
    return leftFieldPath;
}

std::unique_ptr<Expression> EqualityPredicate::rewriteToTagDisjunction(Expression* expr) const {
    if (auto eqExpr = dynamic_cast<ExpressionCompare*>(expr); eqExpr) {
        if (eqExpr->getOp() != ExpressionCompare::EQ && eqExpr->getOp() != ExpressionCompare::NE) {
            return nullptr;
        }
        auto details = extractDetailsFromComparison(eqExpr);
        if (!details) {
            return nullptr;
        }
        auto [_, constChild] = details.value();

        std::vector<boost::intrusive_ptr<Expression>> orListElems;
        auto payload = constChild->getValue();
        auto tags = toValues(generateTags(std::ref(payload)));
        auto disjunction = makeTagDisjunction(_rewriter->getExpressionContext(), std::move(tags));

        if (eqExpr->getOp() == ExpressionCompare::NE) {
            std::vector<boost::intrusive_ptr<Expression>> notChild{disjunction.release()};
            return std::make_unique<ExpressionNot>(_rewriter->getExpressionContext(),
                                                   std::move(notChild));
        }
        return disjunction;
    } else if (auto inExpr = dynamic_cast<ExpressionIn*>(expr)) {
        if (auto inList = dynamic_cast<ExpressionArray*>(inExpr->getOperandList()[1].get())) {
            if (!validateIn(inExpr, inList)) {
                return nullptr;
            }
            auto& equalitiesList = inList->getChildren();
            std::vector<Value> allTags;
            for (auto& equality : equalitiesList) {
                // For each expression representing a FleFindPayload...
                if (auto constChild = dynamic_cast<ExpressionConstant*>(equality.get())) {
                    // ... rewrite the payload to a list of tags...
                    auto payload = constChild->getValue();
                    auto tags = toValues(generateTags(std::ref(payload)));
                    allTags.insert(allTags.end(),
                                   std::make_move_iterator(tags.begin()),
                                   std::make_move_iterator(tags.end()));
                }
            }
            return makeTagDisjunction(_rewriter->getExpressionContext(), std::move(allTags));
        }
        return nullptr;
    }
    MONGO_UNREACHABLE_TASSERT(6911303);
}

std::unique_ptr<Expression> EqualityPredicate::rewriteToRuntimeComparison(Expression* expr) const {
    if (auto eqExpr = dynamic_cast<ExpressionCompare*>(expr); eqExpr) {
        if (eqExpr->getOp() != ExpressionCompare::EQ && eqExpr->getOp() != ExpressionCompare::NE) {
            return nullptr;
        }
        auto details = extractDetailsFromComparison(eqExpr);
        if (!details) {
            return nullptr;
        }
        auto [fieldPath, constChild] = details.value();
        auto fleEqualExpr =
            generateFleEqualMatchUnique(fieldPath->getFieldPathWithoutCurrentPrefix().fullPath(),
                                        constChild->getValue(),
                                        _rewriter->getExpressionContext());
        if (eqExpr->getOp() == ExpressionCompare::NE) {
            std::vector<boost::intrusive_ptr<Expression>> notChild{fleEqualExpr.release()};
            return std::make_unique<ExpressionNot>(_rewriter->getExpressionContext(),
                                                   std::move(notChild));
        }
        return fleEqualExpr;
    } else if (auto inExpr = dynamic_cast<ExpressionIn*>(expr)) {
        if (auto inList = dynamic_cast<ExpressionArray*>(inExpr->getOperandList()[1].get())) {
            auto leftFieldPath = validateIn(inExpr, inList);
            if (!leftFieldPath) {
                return nullptr;
            }
            auto& equalitiesList = inList->getChildren();
            std::vector<boost::intrusive_ptr<Expression>> orListElems;
            for (auto& equality : equalitiesList) {
                if (auto constChild = dynamic_cast<ExpressionConstant*>(equality.get())) {
                    auto fleEqExpr = generateFleEqualMatch(
                        leftFieldPath.value()->getFieldPathWithoutCurrentPrefix().fullPath(),
                        constChild->getValue(),
                        _rewriter->getExpressionContext());
                    orListElems.push_back(fleEqExpr);
                }
            }
            return std::make_unique<ExpressionOr>(_rewriter->getExpressionContext(),
                                                  std::move(orListElems));
        }
        return nullptr;
    }
    MONGO_UNREACHABLE_TASSERT(6911304);
}
}  // namespace mongo::fle
