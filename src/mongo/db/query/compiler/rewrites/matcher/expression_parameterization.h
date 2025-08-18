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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_hasher.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

// Adaptive container for storing a mapping between assigned InputParamIds and parameterized
// MatchExpressions. Uses a vector when 'size()' is below 'useMapThreshold', and builds a map
// for faster lookups once 'size()' reaches the threshold.
struct MatchExpressionInputParamIdContainer {
    using InputParamId = MatchExpression::InputParamId;

    MatchExpressionInputParamIdContainer(size_t useMapThreshold)
        : _useMapThreshold(useMapThreshold) {}

    // Moves the vector and clears other resources.
    operator std::vector<const MatchExpression*>() && {
        _expressionToInputParamIdMap.clear();
        return std::move(_inputParamIdToExpressionVector);
    }

    // Caller must ensure that inputParamId is an increasing sequence of integers.
    InputParamId insert(const MatchExpression* expr, InputParamId paramId) {
        if (_inputParamIdToExpressionVector.empty()) {
            _firstParamId = paramId;
        }
        _inputParamIdToExpressionVector.emplace_back(expr);
        tassert(8551600,
                "Parameters are not provided in an increasing sequence",
                paramId ==
                    _firstParamId +
                        static_cast<InputParamId>(_inputParamIdToExpressionVector.size()) - 1);

        if (_usingMap) {
            _expressionToInputParamIdMap.emplace(expr, paramId);
        } else if (size() >= _useMapThreshold) {
            // If size reaches given threshold, build a map for faster lookups.
            for (size_t i = 0; i < _inputParamIdToExpressionVector.size(); i++) {
                _expressionToInputParamIdMap.emplace(_inputParamIdToExpressionVector[i],
                                                     _firstParamId + i);
            }
            _usingMap = true;
        }

        return paramId;
    }

    boost::optional<InputParamId> find(const MatchExpression* expr) const {
        tassert(7909201,
                "Expected the map to be used for lookup as the number of input param ids "
                "exceeds specified threshold.",
                _usingMap == (size() >= _useMapThreshold));

        // If available, use the map to search for an equivalent expression. Otherwise linearly
        // search through the vector.
        if (_usingMap) {
            auto it = _expressionToInputParamIdMap.find(expr);
            if (it != _expressionToInputParamIdMap.end()) {
                return it->second;
            }
        } else {
            auto it = std::find_if(
                _inputParamIdToExpressionVector.begin(),
                _inputParamIdToExpressionVector.end(),
                [expr](const MatchExpression* m) -> bool { return m->equivalent(expr); });
            if (it != _inputParamIdToExpressionVector.end()) {
                return _firstParamId + (it - _inputParamIdToExpressionVector.begin());
            }
        }

        return boost::none;
    }

    bool usingMap() const {
        return _usingMap;
    }

    size_t size() const {
        return _inputParamIdToExpressionVector.size();
    }


private:
    const size_t _useMapThreshold;

    bool _usingMap = false;

    // Keep track of the id of the first parameter.
    InputParamId _firstParamId = 0;

    // Map from assigned InputParamId to parameterized MatchExpression. It can be safely represented
    // as a vector because in 'MatchExpressionParameterizationVisitorContext' we control that
    // inputParamId is an increasing sequence of integers starting from _firstParamId.
    std::vector<const MatchExpression*> _inputParamIdToExpressionVector;

    struct MatchExpressionsEqual {
        bool operator()(const MatchExpression* expr1, const MatchExpression* expr2) const {
            return expr1->equivalent(expr2);
        }
    };

    absl::flat_hash_map<const MatchExpression*,
                        InputParamId,
                        MatchExpressionHasher,
                        MatchExpressionsEqual>
        _expressionToInputParamIdMap;
};

/**
 * A context to track assigned input parameter IDs for auto-parameterization. Note that the
 * parameterized MatchExpressions must outlive this class.
 */
struct MatchExpressionParameterizationVisitorContext {
    using InputParamId = MatchExpression::InputParamId;

    static constexpr size_t kUseMapThreshold = 50;

    MatchExpressionParameterizationVisitorContext(
        boost::optional<size_t> inputMaxParamCount = boost::none, InputParamId startingParamId = 0)
        : maxParamCount(inputMaxParamCount), nextParamId(startingParamId) {}

    /**
     * Reports whether the requested number of parameter IDs can be assigned within the
     * 'maxParamCount' limit. Used by callers that need to parameterize all or none of the arguments
     * of an expression because MatchExpressionSbePlanCacheKeySerializationVisitor visit() methods
     * expect those to either be fully parameterized or unparameterized. This must set
     * 'parameterized' to false if the requested IDs are not available, as the caller will then not
     * parameterize any of its arguments, which means the query will not be fully parameterized
     * even if we do not end up using all the allowed parameter IDs.
     */
    bool availableParamIds(int numIds) {
        if (!parameterized) {
            return false;
        }
        if (maxParamCount &&
            (static_cast<size_t>(nextParamId) + static_cast<size_t>(numIds)) > *maxParamCount) {
            parameterized = false;
            return false;
        }
        return true;
    }

    /**
     * Assigns a parameter ID to `expr` with the ability to reuse an already-assigned parameter id
     * if `expr` is equivalent to an expression we have seen before. This is used to model
     * dependencies within a query (e.g. $or[{a:1}, {a:1, b:1}] --> $or[{a:P0}, {a:P0, b:P1}]) and
     * to reduce the number of parameters. The reusable parameters use the same vector for tracking
     * as the non-reusable to ensure uniqueness of the parameterId.
     *
     * If 'maxParamCount' was specified, this stops creating new parameters once that limit has been
     * reached and returns boost::none instead.
     */
    boost::optional<InputParamId> nextReusableInputParamId(const MatchExpression* expr) {
        if (!parameterized) {
            return boost::none;
        }

        if (!expr) {
            return boost::none;
        }

        if (auto reusableParamId = inputParamIdToExpressionMap.find(expr); reusableParamId) {
            return reusableParamId;
        }

        // Couldn't find a param id to reuse. Create a new one.
        return nextInputParamId(expr);
    }

    /**
     * Assigns a parameter ID to 'expr'. This is not only a helper for
     * nextReusableInputParamId(); it is also called directly by visit() methods whose
     * expressions are deemed non-shareable.
     *
     * If 'maxParamCount' was specified, this stops creating new parameters once that limit has
     * been reached and returns boost::none instead.
     */
    boost::optional<InputParamId> nextInputParamId(const MatchExpression* expr) {
        if (!parameterized) {
            return boost::none;
        }
        if (maxParamCount && static_cast<size_t>(nextParamId) >= *maxParamCount) {
            parameterized = false;
            return boost::none;
        }

        return inputParamIdToExpressionMap.insert(expr, nextParamId++);
    }

    // Map from assigned InputParamId to parameterized MatchExpression.
    MatchExpressionInputParamIdContainer inputParamIdToExpressionMap{kUseMapThreshold};

    // This is the maximumum number of MatchExpression parameters a single CanonicalQuery
    // may have. A value of boost::none means unlimited.
    boost::optional<size_t> maxParamCount;

    // This is the next input parameter ID to assign. It may be initialized to a value > 0
    // to enable a forest of match expressions to be parameterized by allowing each tree to
    // continue parameter IDs from where the prior tree left off.
    InputParamId nextParamId;

    // This is changed to false if an attempt to parameterize ever failed (because it would
    // exceed 'maxParamCount').
    bool parameterized = true;
};

/**
 * An implementation of a MatchExpression visitor which assigns an optional input parameter
 * ID to each node which is eligible for auto-parameterization:
 *  - BitsAllClearMatchExpression
 *  - BitsAllSetMatchExpression
 *  - BitsAnyClearMatchExpression
 *  - BitsAnySetMatchExpression
 *  - BitTestMatchExpression (two parameter IDs for the position and mask)
 *  - Comparison expressions, unless compared against MinKey, MaxKey, null or NaN value or
 * array
 *      - EqualityMatchExpression
 *      - GTEMatchExpression
 *      - GTMatchExpression
 *      - LTEMatchExpression
 *      - LTMatchExpression
 *  - InMatchExpression, unless it contains an array, null or regexp value.
 *  - ModMatchExpression (two parameter IDs for the divider and reminder)
 *  - RegexMatchExpression (two parameter IDs for the compiled regex and raw value)
 *  - SizeMatchExpression
 *  - TypeMatchExpression, unless type value is Array
 *  - WhereMatchExpression
 */
class MatchExpressionParameterizationVisitor final : public MatchExpressionMutableVisitor {
public:
    MatchExpressionParameterizationVisitor(MatchExpressionParameterizationVisitorContext* context)
        : _context{context} {
        invariant(_context);
    }

    void visit(AlwaysFalseMatchExpression* expr) final {}
    void visit(AlwaysTrueMatchExpression* expr) final {}
    void visit(AndMatchExpression* expr) final {}
    void visit(BitsAllClearMatchExpression* expr) final;
    void visit(BitsAllSetMatchExpression* expr) final;
    void visit(BitsAnyClearMatchExpression* expr) final;
    void visit(BitsAnySetMatchExpression* expr) final;
    void visit(ElemMatchObjectMatchExpression* matchExpr) final {}
    void visit(ElemMatchValueMatchExpression* matchExpr) final {}
    void visit(EqualityMatchExpression* expr) final;
    void visit(ExistsMatchExpression* expr) final {}
    void visit(ExprMatchExpression* expr) final {}
    void visit(GTEMatchExpression* expr) final;
    void visit(GTMatchExpression* expr) final;
    void visit(GeoMatchExpression* expr) final {}
    void visit(GeoNearMatchExpression* expr) final {}
    void visit(InMatchExpression* expr) final;
    void visit(InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(InternalExprEqMatchExpression* expr) final {}
    void visit(InternalExprGTMatchExpression* expr) final {}
    void visit(InternalExprGTEMatchExpression* expr) final {}
    void visit(InternalExprLTMatchExpression* expr) final {}
    void visit(InternalExprLTEMatchExpression* expr) final {}
    void visit(InternalEqHashedKey* expr) final {
        // Don't support parameterization of InternEqHashedKey because it is not implemented
        // in SBE.
    }
    void visit(InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {}
    void visit(InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(InternalSchemaCondMatchExpression* expr) final {}
    void visit(InternalSchemaEqMatchExpression* expr) final {}
    void visit(InternalSchemaFmodMatchExpression* expr) final {}
    void visit(InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(InternalSchemaObjectMatchExpression* expr) final {}
    void visit(InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(InternalSchemaTypeExpression* expr) final {}
    void visit(InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(InternalSchemaXorMatchExpression* expr) final {}
    void visit(LTEMatchExpression* expr) final;
    void visit(LTMatchExpression* expr) final;
    void visit(ModMatchExpression* expr) final;
    void visit(NorMatchExpression* expr) final {}
    void visit(NotMatchExpression* expr) final {}
    void visit(OrMatchExpression* expr) final {}
    void visit(RegexMatchExpression* expr) final;
    void visit(SizeMatchExpression* expr) final;
    void visit(TextMatchExpression* expr) final {}
    void visit(TextNoOpMatchExpression* expr) final {}
    void visit(TwoDPtInAnnulusExpression* expr) final {}
    void visit(TypeMatchExpression* expr) final;
    void visit(WhereMatchExpression* expr) final;
    void visit(WhereNoOpMatchExpression* expr) final {}

private:
    void visitComparisonMatchExpression(ComparisonMatchExpressionBase* expr);

    void visitBitTestExpression(BitTestMatchExpression* expr);

    MatchExpressionParameterizationVisitorContext* _context;
};

/**
 * A match expression tree walker compatible with tree_walker::walk() to be used with
 * MatchExpressionParameterizationVisitor.
 */
class MatchExpressionParameterizationWalker {
public:
    MatchExpressionParameterizationWalker(MatchExpressionParameterizationVisitor* visitor)
        : _visitor{visitor} {
        invariant(_visitor);
    }

    void preVisit(MatchExpression* expr) {
        expr->acceptVisitor(_visitor);
    }

    void postVisit(MatchExpression* expr) {}

    void inVisit(long count, MatchExpression* expr) {}

private:
    MatchExpressionParameterizationVisitor* _visitor;
};

/**
 * Assigns an optional input parameter ID to each node which is eligible for
 * auto-parameterization.
 * - tree - The MatchExpression to be parameterized.
 * - maxParamCount - Optional maximum number of parameters that can be created. If the
 *   number of parameters would exceed this value, no parameterization will be performed.
 * - startingParamId - Optional first parameter ID to use. This enables parameterizing a forest
 *   of match expressions, where each tree continues IDs where the prior one left off.
 * - parameterized - Optional output argument. If non-null, the method sets this output to
 *   indicate whether parameterization was actually done.
 *
 * Returns a vector-form map to a parameterized MatchExpression from assigned InputParamId. (The
 * vector index serves as the map key.)
 */
std::vector<const MatchExpression*> parameterizeMatchExpression(
    MatchExpression* tree,
    boost::optional<size_t> maxParamCount = boost::none,
    MatchExpression::InputParamId startingParamId = 0,
    bool* parameterized = nullptr);

/**
 * Sets max param count in parameterizeMatchExpression to 0, clearing MatchExpression
 * auto-parameterization before CanonicalQuery to ABT translation.
 */
std::vector<const MatchExpression*> unparameterizeMatchExpression(MatchExpression* tree);

}  // namespace mongo
