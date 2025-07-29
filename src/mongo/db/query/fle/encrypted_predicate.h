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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"

#include <functional>
#include <memory>
#include <typeindex>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

/**
 * This file contains an abstract class that describes rewrites on agg Expressions and
 * MatchExpressions for individual encrypted index types. Subclasses of this class represent
 * concrete encrypted index types, like Equality and Range.
 *
 * This class is not responsible for traversing expression trees, but instead takes leaf
 * expressions that it may replace. Tree traversal is handled by the QueryRewriter.
 */

namespace mongo {
namespace fle {

// Virtual functions can't be templated, so in order to write a function which can take in either a
// BSONElement or a Value&, we need to create a variant type to use in function signatures.
// std::reference_wrapper is necessary to avoid copying the Value because references alone cannot be
// included in a variant. BSONElement can be passed by value because it is just a pointer into an
// owning BSONObj.
using BSONValue = std::variant<BSONElement, std::reference_wrapper<Value>>;

/**
 * Parse a find payload from either a BSONElement or a Value. All ParsedFindPayload types should
 * have constructors for both BSONElements and Values, which will enable this function to work on
 * both types.
 */
template <typename T>
T parseFindPayload(BSONValue payload) {
    return visit(OverloadedVisitor{[&](BSONElement payload) { return T(payload); },
                                   [&](Value payload) {
                                       return T(payload);
                                   }},
                 payload);
}

/**
 * Given an array of tags, return a $in predicate with __safeContent__.
 */
std::unique_ptr<Expression> makeTagDisjunction(ExpressionContext* expCtx,
                                               std::vector<Value>&& tags);

std::unique_ptr<MatchExpression> makeTagDisjunction(BSONArray&& tagArray);

/**
 * Convert a vector of PrfBlocks to a BSONArray for use in MatchExpression tag generation.
 */
BSONArray toBSONArray(std::vector<PrfBlock>&& vec);

/**
 * Convert a vector of PrfBlocks to a vector of Values for use in Agg tag generation.
 */
std::vector<Value> toValues(std::vector<PrfBlock>&& vec);


void logTagsExceeded(const ExceptionFor<ErrorCodes::FLEMaxTagLimitExceeded>& ex);

/**
 * Returns true of the BSONElement contains BinData(6) with the specified sub-sub type.
 */
bool isPayloadOfType(EncryptedBinDataType ty, const BSONElement& elt);
/**
 * Returns true of the Value contains BinData(6) with the specified sub-sub type.
 */
bool isPayloadOfType(EncryptedBinDataType ty, const Value& elt);

/**
 * Interface for implementing a server rewrite for an encrypted index. Each type of predicate
 * should have its own subclass that implements the virtual methods in this class.
 */
class EncryptedPredicate {
public:
    EncryptedPredicate(const QueryRewriterInterface* rewriter) : _rewriter(rewriter) {}
    virtual ~EncryptedPredicate() {};
    /**
     * Rewrite a terminal expression for this encrypted predicate. If this function returns
     * nullptr, then no rewrite needs to be done. Rewrites generally transform predicates from one
     * kind of expression to another, either a $in or an $_internalFle* runtime expression, and so
     * this function will allocate a new expression and return a unique_ptr to it.
     */
    template <typename T>
    std::unique_ptr<T> rewrite(T* expr) const {
        std::unique_ptr<T> tagDisjunction =
            _tryRewriteToTagDisjunction<T>([&]() { return this->rewriteToTagDisjunction(expr); });
        if (tagDisjunction) {
            return tagDisjunction;
        }
        // If we failed to generate the tag disjunction, we rewrite to a runtime comparison instead,
        // which generates a collection scan.
        return rewriteToRuntimeComparison(expr);
    }

protected:
    /**
     * This helper method expects to receive a functor that performs the FLE rewrite into a tag
     * disjunction. It wraps the invocation to the functor in a try/catch block that specifically
     * detect if the rewrite failed to generate tags because it exceeded the tag limit. In most
     * cases, if we receive the exception for ErrorCodes::FLEMaxTagLimitExceeded, we will log an
     * error for the exception, swallow the exception and return a nullptr to indicate we could not
     * generate the tag disjunction.
     *
     * Generally, the type of the rewritten FLE expression is the same input:
     * 1) MatchExpression -> MatchExpression tag disjunction.
     * 2) Expression -> Expression tag disjunction.
     *
     * However, encrypted text search predicates (only implemented as Expressions) may exhibit poor
     * performance at scale. This is because the query optimization rewrite relies on a residual
     * predicate which does not perform well at scale. This is most likely to affect text
     * predicates, because there is a higher chance that a collection with a substring index will
     * have a high number of documents with the same substring.
     *
     * We template this method to accept a templated functor, so we can accommodate
     * TextSearchPredicate::rewriteToTagDisjunctionAsMatch() using the same code path for this
     * try/catch block as other expressions (i.e EncryptedPredicate::rewrite()).
     */
    template <typename T, typename Fn>
    std::unique_ptr<T> _tryRewriteToTagDisjunction(Fn&& rewriteToTagDisjunctionFunc) const {
        auto mode = _rewriter->getEncryptedCollScanMode();
        if (mode != EncryptedCollScanMode::kForceAlways) {
            try {
                return rewriteToTagDisjunctionFunc();
            } catch (const ExceptionFor<ErrorCodes::FLEMaxTagLimitExceeded>& ex) {
                // LOGV2 can't be called from a header file, so this call is factored out to a
                // function defined in the cpp file.
                logTagsExceeded(ex);
                if (mode != EncryptedCollScanMode::kUseIfNeeded) {
                    throw;
                }
            }
        }
        return nullptr;
    }

    /**
     * Check if the passed-in payload is a FLE2 find payload for the right encrypted index type.
     */
    virtual bool isPayload(const BSONElement& elt) const {
        auto optType = getEncryptedBinDataType(elt);
        return optType && this->validateType(*optType);
    }

    /**
     * Check if the passed-in payload is a FLE2 find payload for the right encrypted index type.
     */
    virtual bool isPayload(const Value& v) const {
        auto optType = getEncryptedBinDataType(v);
        return optType && this->validateType(*optType);
    }

    /**
     * Check if the payload type is a deprecated payload type, which, if encountered, causes
     * the rewrite to throw an error.
     */
    virtual bool isDeprecatedPayloadType(EncryptedBinDataType type) const {
        return false;
    }

    /**
     * Generate tags from a FLE2 Find Payload. This function takes in a variant of BSONElement and
     * Value so that it can be used in both the MatchExpression and Aggregation contexts. Virtual
     * functions can't also be templated, which is why we need the runtime dispatch on the variant.
     */
    virtual std::vector<PrfBlock> generateTags(BSONValue payload) const = 0;

    /**
     * Rewrite to a tag disjunction on the __safeContent__ field.
     */
    virtual std::unique_ptr<MatchExpression> rewriteToTagDisjunction(
        MatchExpression* expr) const = 0;
    /**
     * Rewrite to a tag disjunction on the __safeContent__ field.
     */
    virtual std::unique_ptr<Expression> rewriteToTagDisjunction(Expression* expr) const = 0;

    /**
     * Rewrite to an expression which can generate tags at runtime during an encrypted collscan.
     */
    virtual std::unique_ptr<MatchExpression> rewriteToRuntimeComparison(
        MatchExpression* expr) const = 0;
    /**
     * Rewrite to an expression which can generate tags at runtime during an encrypted collscan.
     */
    virtual std::unique_ptr<Expression> rewriteToRuntimeComparison(Expression* expr) const = 0;

    const QueryRewriterInterface* _rewriter;

private:
    /**
     * Sub-subtype associated with the find payload for this encrypted predicate.
     */
    virtual EncryptedBinDataType encryptedBinDataType() const = 0;

    /**
     * Checks if type is a deprecated payload type and if so, throws an exception.
     * Otherwise returns whether type is the expected find payload for this predicate.
     */
    bool validateType(EncryptedBinDataType type) const;
};

/**
 * Encrypted predicate rewrites are registered at startup time using MONGO_INITIALIZER blocks.
 * MatchExpression rewrites are keyed on the MatchExpressionType enum, and Agg Expression rewrites
 * are keyed on the dynamic type for the Expression subclass.
 */

using ExpressionRewriteFunction =
    std::function<std::unique_ptr<Expression>(QueryRewriterInterface*, Expression*)>;
using ExpressionToRewriteMap =
    stdx::unordered_map<std::type_index, std::vector<ExpressionRewriteFunction>>;

extern ExpressionToRewriteMap aggPredicateRewriteMap;

using MatchRewriteFunction =
    std::function<std::unique_ptr<MatchExpression>(QueryRewriterInterface*, MatchExpression*)>;
using MatchTypeToRewriteMap =
    stdx::unordered_map<MatchExpression::MatchType, std::vector<MatchRewriteFunction>>;

extern MatchTypeToRewriteMap matchPredicateRewriteMap;

/**
 * Register an agg rewrite if a condition is true at startup time.
 */
#define REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_GUARDED(className, rewriteClass, isEnabledExpr)  \
    MONGO_INITIALIZER(encryptedAggPredicateRewriteFor_##className##_##rewriteClass)               \
    (InitializerContext*) {                                                                       \
        if (aggPredicateRewriteMap.find(typeid(className)) == aggPredicateRewriteMap.end()) {     \
            aggPredicateRewriteMap[typeid(className)] = std::vector<ExpressionRewriteFunction>(); \
        }                                                                                         \
        aggPredicateRewriteMap[typeid(className)].push_back([](auto* rewriter, auto* expr) {      \
            if (isEnabledExpr) {                                                                  \
                return rewriteClass{rewriter}.rewrite(expr);                                      \
            } else {                                                                              \
                return std::unique_ptr<Expression>(nullptr);                                      \
            }                                                                                     \
        });                                                                                       \
    };

/**
 * Register an agg rewrite unconditionally.
 */
#define REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE(matchType, rewriteClass) \
    REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_GUARDED(matchType, rewriteClass, true)

/**
 * Register an agg rewrite behind a feature flag.
 */
#define REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_WITH_FLAG(matchType, rewriteClass, featureFlag) \
    REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_GUARDED(                                            \
        matchType, rewriteClass, (featureFlag).canBeEnabled())

/**
 * Register a MatchExpression rewrite if a condition is true at startup time.
 */
#define REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_GUARDED(matchType, rewriteClass, isEnabledExpr) \
    MONGO_INITIALIZER(encryptedMatchPredicateRewriteFor_##matchType##_##rewriteClass)              \
    (InitializerContext*) {                                                                        \
        if (matchPredicateRewriteMap.find(MatchExpression::matchType) ==                           \
            matchPredicateRewriteMap.end()) {                                                      \
            matchPredicateRewriteMap[MatchExpression::matchType] =                                 \
                std::vector<MatchRewriteFunction>();                                               \
        }                                                                                          \
        matchPredicateRewriteMap[MatchExpression::matchType].push_back(                            \
            [](auto* rewriter, auto* expr) {                                                       \
                if (isEnabledExpr) {                                                               \
                    return rewriteClass{rewriter}.rewrite(expr);                                   \
                } else {                                                                           \
                    return std::unique_ptr<MatchExpression>(nullptr);                              \
                }                                                                                  \
            });                                                                                    \
    };
/**
 * Register a MatchExpression rewrite unconditionally.
 */
#define REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE(matchType, rewriteClass) \
    REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_GUARDED(matchType, rewriteClass, true)

/**
 * Register a MatchExpression rewrite behind a feature flag.
 */
#define REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(matchType, rewriteClass, featureFlag) \
    REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_GUARDED(                                            \
        matchType, rewriteClass, (featureFlag).canBeEnabled())
}  // namespace fle
}  // namespace mongo
