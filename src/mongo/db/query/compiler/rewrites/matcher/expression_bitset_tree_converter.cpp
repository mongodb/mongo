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

#include "mongo/db/query/compiler/rewrites/matcher/expression_bitset_tree_converter.h"

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_hasher.h"
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

namespace mongo {
using boolean_simplification::BitsetTreeNode;
using boolean_simplification::Minterm;

namespace {
/**
 * Context class for MatchExpression visitor 'BitsetVisitor'.
 */
struct Context {
    // Up to kThreshold number of predicates ExpressionMap is not used 'getOrAssignBitIndex'. This
    // exact value was selected empirically using benchmarks.
    static constexpr size_t kThreshold = 8;

    // Map between MatchExpression and a bit index assigned to this MatchExpression in the Bitset
    // building by the visitor.
    using ExpressionMap = stdx::
        unordered_map<const MatchExpression*, size_t, MatchExpressionHasher, MatchExpressionEq>;

    /**
     * A bit conflict occurs when we attempt to assign different values to the same bit. This
     * situation arises when we combine two terms that have conflicting values for a particular
     * bit.
     */
    enum BitConflict {
        // No bit conflicts.
        None,
        // Always true conflict arises in a disjunction.
        AlwaysTrue,
        // Always false conflict arises in a conjunction.
        AlwaysFalse
    };

    explicit Context(size_t maximumNumberOfUniquePredicates)
        : _maximumNumberOfUniquePredicates{maximumNumberOfUniquePredicates} {}

    /**
     * Stores the given MatchExpression and assign a bit index to it. Returns the bit index.
     */
    size_t getOrAssignBitIndex(const MatchExpression* expr) {
        if (expressions.size() < kThreshold) {
            auto pos = std::find_if(
                expressions.begin(), expressions.end(), [expr](const ExpressionBitInfo& info) {
                    return expr->equivalent(info.expression);
                });
            if (pos != expressions.end()) {
                return std::distance(expressions.begin(), pos);
            }

            expressions.emplace_back(expr);
            return expressions.size() - 1;
        }

        if (_map.empty()) {
            for (size_t i = 0; i < expressions.size(); ++i) {
                _map.emplace(expressions[i].expression, i);
            }
        }

        auto [it, inserted] = _map.try_emplace(expr, expressions.size());
        if (inserted) {
            expressions.emplace_back(expr);
        }
        return it->second;
    }

    size_t getMaxtermSize() const {
        return expressions.size();
    }

    bool isAborted() const {
        return containsSchemaExpressions || _maximumNumberOfUniquePredicates <= expressions.size();
    }

    BitsetTreeTransformResult::ExpressionList expressions;

    BitConflict bitConflict{None};

    size_t expressionSize{0};

    bool containsSchemaExpressions{false};

private:
    ExpressionMap _map;

    size_t _maximumNumberOfUniquePredicates;
};

/**
 * Visitor class which converts a MatchExpression tree to a Bitset tree. The visitor does not use
 * 'tree_walker' and explicitly visits the children of each node, as special handling for $elemMatch
 * is required.
 */
class BitsetVisitor : public MatchExpressionConstVisitor {
public:
    BitsetVisitor(Context& context, BitsetTreeNode& parent, bool isNegated, bool isRoot = false)
        : _context(context), _parent(parent), _isNegated{isNegated}, _isRoot{isRoot} {}

    void visit(const AndMatchExpression* expr) final {
        ++_context.expressionSize;

        BitsetTreeNode node{BitsetTreeNode::And, _isNegated};

        BitsetVisitor visitor{_context, node, /* isNegated */ false};

        for (size_t childIndex = 0; childIndex < expr->numChildren(); ++childIndex) {
            expr->getChild(childIndex)->acceptVisitor(&visitor);
            switch (_context.bitConflict) {
                case Context::None:
                    break;
                case Context::AlwaysFalse:
                    _context.bitConflict =
                        node.isNegated ? Context::AlwaysTrue : Context::AlwaysFalse;
                    return;
                case Context::AlwaysTrue:
                    _context.bitConflict = Context::None;
                    break;
            }

            if (MONGO_unlikely(_context.isAborted())) {
                return;
            }
        }

        appendChild(std::move(node));
    }

    void visit(const OrMatchExpression* expr) final {
        ++_context.expressionSize;

        BitsetTreeNode node{BitsetTreeNode::Or, _isNegated};

        BitsetVisitor visitor{_context, node, /* isNegated */ false};

        for (size_t childIndex = 0; childIndex < expr->numChildren(); ++childIndex) {
            expr->getChild(childIndex)->acceptVisitor(&visitor);
            switch (_context.bitConflict) {
                case Context::None:
                    break;
                case Context::AlwaysFalse:
                    _context.bitConflict = Context::None;
                    break;
                case Context::AlwaysTrue:
                    _context.bitConflict =
                        node.isNegated ? Context::AlwaysFalse : Context::AlwaysTrue;
                    return;
            }

            if (MONGO_unlikely(_context.isAborted())) {
                return;
            }
        }

        appendChild(std::move(node));
    }

    void visit(const NorMatchExpression* expr) final {
        ++_context.expressionSize;

        // NOR == NOT * OR == AND * NOT
        BitsetTreeNode node{BitsetTreeNode::Or, !_isNegated};

        BitsetVisitor visitor{_context, node, /* isNegated */ false};

        for (size_t childIndex = 0; childIndex < expr->numChildren(); ++childIndex) {
            expr->getChild(childIndex)->acceptVisitor(&visitor);
            switch (_context.bitConflict) {
                case Context::None:
                    break;
                case Context::AlwaysFalse:
                    _context.bitConflict = Context::None;
                    break;
                case Context::AlwaysTrue:
                    _context.bitConflict =
                        node.isNegated ? Context::AlwaysFalse : Context::AlwaysTrue;
                    return;
            }

            if (MONGO_unlikely(_context.isAborted())) {
                return;
            }
        }

        appendChild(std::move(node));
    }

    void visit(const NotMatchExpression* expr) final {
        // Don't increase 'expressionSize', because NOT is considered to be a part of its child.
        // predicate.
        BitsetVisitor visitor{_context, _parent, !_isNegated};
        expr->getChild(0)->acceptVisitor(&visitor);
    }

    void visit(const ElemMatchValueMatchExpression* expr) final {
        visitLeafNode(expr);
    }

    void visit(const ElemMatchObjectMatchExpression* expr) final {
        visitLeafNode(expr);
    }

    void visit(const BitsAllClearMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const BitsAllSetMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const BitsAnyClearMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const BitsAnySetMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const EqualityMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const GTEMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const GTMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const LTEMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const LTMatchExpression* expr) final {
        visitLeafNode(expr);
    }

    void visit(const InMatchExpression* expr) final {
        visitLeafNode(expr);
    }

    void visit(const ModMatchExpression* expr) final {
        visitLeafNode(expr);
    }

    void visit(const RegexMatchExpression* expr) final {
        visitLeafNode(expr);
    }

    void visit(const SizeMatchExpression* expr) final {
        visitLeafNode(expr);
    }

    void visit(const TypeMatchExpression* expr) final {
        visitLeafNode(expr);
    }

    void visit(const AlwaysFalseMatchExpression* expr) final {
        ++_context.expressionSize;
        _context.bitConflict = _isNegated ? Context::AlwaysTrue : Context::AlwaysFalse;
    }
    void visit(const AlwaysTrueMatchExpression* expr) final {
        ++_context.expressionSize;
        _context.bitConflict = _isNegated ? Context::AlwaysFalse : Context::AlwaysTrue;
    }

    void visit(const ExistsMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const ExprMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const GeoMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const GeoNearMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalExprEqMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalExprGTMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalExprGTEMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalExprLTMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalExprLTEMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const TextMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {
        visitLeafNode(expr);
    }

    void visit(const WhereMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        visitLeafNode(expr);
    }

    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaBinDataEncryptedTypeExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaBinDataSubTypeExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaCondMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaEqMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaFmodMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaMaxItemsMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaMaxLengthMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaMinItemsMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaMinLengthMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaObjectMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaRootDocEqMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaTypeExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalSchemaXorMatchExpression*) final {
        visitiInternalSchemaNode();
    }
    void visit(const InternalEqHashedKey* expr) final {
        visitLeafNode(expr);
    }

private:
    void appendChild(BitsetTreeNode&& child) {
        if (_isRoot) {
            _parent = std::move(child);
        } else {
            _parent.internalChildren.emplace_back(std::move(child));
        }
    }

    void visitLeafNode(const MatchExpression* expr) {
        ++_context.expressionSize;

        const size_t bitIndex = _context.getOrAssignBitIndex(expr);
        if (_context.isAborted()) {
            return;
        }

        // Process bit conflicts. See comments for BitConlict type for details.
        const bool hasConflict = _parent.leafChildren.size() > bitIndex &&
            _parent.leafChildren.mask[bitIndex] &&
            _parent.leafChildren.predicates[bitIndex] == _isNegated;
        if (hasConflict) {
            _context.bitConflict =
                _parent.type == BitsetTreeNode::And ? Context::AlwaysFalse : Context::AlwaysTrue;
        } else {
            _parent.leafChildren.set(bitIndex, !_isNegated);
        }
    }

    void visitiInternalSchemaNode() {
        _context.containsSchemaExpressions = true;
    }

    Context& _context;
    BitsetTreeNode& _parent;
    const bool _isNegated;
    const bool _isRoot;
};
}  // namespace

boost::optional<BitsetTreeTransformResult> transformToBitsetTree(
    const MatchExpression* root, size_t maximumNumberOfUniquePredicates) {
    Context context{maximumNumberOfUniquePredicates};

    BitsetTreeNode bitsetRoot{BitsetTreeNode::And, /* isNegated */ false};
    BitsetVisitor visitor{context, bitsetRoot, /* isNegated */ false, /* isRoot */ true};
    root->acceptVisitor(&visitor);

    if (MONGO_unlikely(context.isAborted())) {
        return boost::none;
    }

    bitsetRoot.ensureBitsetSize(context.getMaxtermSize());

    switch (context.bitConflict) {
        case Context::None:
            break;
        case Context::AlwaysFalse:
            // Empty OR is always false.
            return {{BitsetTreeNode{BitsetTreeNode::Or, false},
                     std::move(context.expressions),
                     context.expressionSize}};
        case Context::AlwaysTrue:
            // Empty AND is always true.
            return {{BitsetTreeNode{BitsetTreeNode::And, false},
                     std::move(context.expressions),
                     context.expressionSize}};
    }

    return {{std::move(bitsetRoot), std::move(context.expressions), context.expressionSize}};
}
}  // namespace mongo
