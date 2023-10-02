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

#include "mongo/db/matcher/expression_bitset_tree_converter.h"

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
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"

namespace mongo {
using boolean_simplification::BitsetTreeNode;
using boolean_simplification::Maxterm;
using boolean_simplification::Minterm;

namespace {
/**
 * Context class for MatchExpression visitor 'BitsetVisitor'.
 */
struct Context {
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
        auto it = _map.find(expr);
        if (it != _map.end()) {
            return it->second;
        }

        const size_t bitIndex = expressions.size();
        expressions.emplace_back(expr->clone());
        _map[expressions.back().expression.get()] = bitIndex;

        return bitIndex;
    }

    size_t getMaxtermSize() const {
        return expressions.size();
    }

    bool isMaximumNumberOfUniquePredicatesExceeded() const {
        return _maximumNumberOfUniquePredicates < expressions.size();
    }

    std::vector<ExpressionBitInfo> expressions;

    BitConflict bitConflict{None};

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
    BitsetVisitor(Context& context, BitsetTreeNode& parent, bool isNegated)
        : _context(context), _parent(parent), _isNegated{isNegated} {}

    void visit(const AndMatchExpression* expr) final {
        BitsetTreeNode node{BitsetTreeNode::And, _isNegated};

        BitsetVisitor visitor{_context, node, /* isNegated */ false};

        for (size_t childIndex = 0; childIndex < expr->numChildren(); ++childIndex) {
            expr->getChild(childIndex)->acceptVisitor(&visitor);
            switch (_context.bitConflict) {
                case Context::None:
                    break;
                case Context::AlwaysFalse:
                    return;
                case Context::AlwaysTrue:
                    _context.bitConflict = Context::None;
                    break;
            }

            if (MONGO_unlikely(_context.isMaximumNumberOfUniquePredicatesExceeded())) {
                return;
            }
        }

        _parent.internalChildren.emplace_back(std::move(node));
    }

    void visit(const OrMatchExpression* expr) final {
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
                    return;
            }

            if (MONGO_unlikely(_context.isMaximumNumberOfUniquePredicatesExceeded())) {
                return;
            }
        }

        _parent.internalChildren.emplace_back(std::move(node));
    }

    void visit(const NorMatchExpression* expr) final {
        // NOR == NOT * OR == AND * NOT
        BitsetTreeNode node{BitsetTreeNode::Or, !_isNegated};

        BitsetVisitor visitor{_context, node, /* isNegated */ false};

        for (size_t childIndex = 0; childIndex < expr->numChildren(); ++childIndex) {
            expr->getChild(childIndex)->acceptVisitor(&visitor);
            switch (_context.bitConflict) {
                case Context::None:
                    break;
                case Context::AlwaysFalse:
                    return;
                case Context::AlwaysTrue:
                    _context.bitConflict = Context::None;
                    break;
            }

            if (MONGO_unlikely(_context.isMaximumNumberOfUniquePredicatesExceeded())) {
                return;
            }
        }

        _parent.internalChildren.emplace_back(std::move(node));
    }

    void visit(const NotMatchExpression* expr) final {
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
        _context.bitConflict = Context::AlwaysFalse;
    }
    void visit(const AlwaysTrueMatchExpression* expr) final {
        _context.bitConflict = Context::AlwaysTrue;
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

    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaCondMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaEqMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaFmodMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaTypeExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalSchemaXorMatchExpression* expr) final {
        visitLeafNode(expr);
    }
    void visit(const InternalEqHashedKey* expr) final {
        visitLeafNode(expr);
    }

private:
    /**
     * Returns the expressions's bit index if no conflicts happen.
     */
    boost::optional<size_t> visitLeafNode(const MatchExpression* expr) {
        const size_t bitIndex = _context.getOrAssignBitIndex(expr);
        // Process bit conflicts. See comments for BitConlict type for details.
        const bool hasConflict = _parent.leafChildren.size() > bitIndex &&
            _parent.leafChildren.mask[bitIndex] &&
            _parent.leafChildren.predicates[bitIndex] == _isNegated;
        if (hasConflict) {
            _context.bitConflict =
                _parent.type == BitsetTreeNode::And ? Context::AlwaysFalse : Context::AlwaysTrue;
            return boost::none;
        } else {
            _parent.leafChildren.set(bitIndex, !_isNegated);
            return bitIndex;
        }
    }

    Context& _context;
    BitsetTreeNode& _parent;
    const bool _isNegated;
};
}  // namespace

boost::optional<std::pair<boolean_simplification::BitsetTreeNode, std::vector<ExpressionBitInfo>>>
transformToBitsetTree(const MatchExpression* root, size_t maximumNumberOfUniquePredicates) {
    Context context{maximumNumberOfUniquePredicates};

    BitsetTreeNode bitsetRoot{BitsetTreeNode::And, false};
    BitsetVisitor visitor{context, bitsetRoot, false};
    root->acceptVisitor(&visitor);

    if (MONGO_unlikely(context.isMaximumNumberOfUniquePredicatesExceeded())) {
        return boost::none;
    }

    bitsetRoot.ensureBitsetSize(context.getMaxtermSize());
    switch (context.bitConflict) {
        case Context::None:
            break;
        case Context::AlwaysFalse:
            // Empty OR is always false.
            return {{BitsetTreeNode{BitsetTreeNode::Or, false}, std::move(context.expressions)}};
        case Context::AlwaysTrue:
            // Empty AND is always true.
            return {{BitsetTreeNode{BitsetTreeNode::And, false}, std::move(context.expressions)}};
    }

    // If we have just one child return it to avoid unnecessary $and or $or nodes with only one
    // child.
    if (bitsetRoot.leafChildren.mask.count() == 0 && bitsetRoot.internalChildren.size() == 1) {
        return {{std::move(bitsetRoot.internalChildren[0]), std::move(context.expressions)}};
    }

    return {{std::move(bitsetRoot), std::move(context.expressions)}};
}
}  // namespace mongo
