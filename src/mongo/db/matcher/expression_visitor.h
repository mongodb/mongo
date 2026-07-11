// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/tree_walker.h"
#include "mongo/util/modules.h"

namespace mongo {
class AlwaysFalseMatchExpression;
class AlwaysTrueMatchExpression;
class AndMatchExpression;
class BitsAllClearMatchExpression;
class BitsAllSetMatchExpression;
class BitsAnyClearMatchExpression;
class BitsAnySetMatchExpression;
class ElemMatchObjectMatchExpression;
class ElemMatchValueMatchExpression;
class EqualityMatchExpression;
class ExistsMatchExpression;
class ExprMatchExpression;
class GTEMatchExpression;
class GTMatchExpression;
class GeoMatchExpression;
class GeoNearMatchExpression;
class InMatchExpression;
class InternalBucketGeoWithinMatchExpression;
class InternalExprEqMatchExpression;
class InternalExprGTMatchExpression;
class InternalExprGTEMatchExpression;
class InternalExprLTMatchExpression;
class InternalExprLTEMatchExpression;
class InternalEqHashedKey;
class InternalSchemaAllElemMatchFromIndexMatchExpression;
class InternalSchemaAllowedPropertiesMatchExpression;
class InternalSchemaBinDataEncryptedTypeExpression;
class InternalSchemaBinDataFLE2EncryptedTypeExpression;
class InternalSchemaBinDataSubTypeExpression;
class InternalSchemaCondMatchExpression;
class InternalSchemaEqMatchExpression;
class InternalSchemaFmodMatchExpression;
class InternalSchemaMatchArrayIndexMatchExpression;
class InternalSchemaMaxItemsMatchExpression;
class InternalSchemaMaxLengthMatchExpression;
class InternalSchemaMaxPropertiesMatchExpression;
class InternalSchemaMinItemsMatchExpression;
class InternalSchemaMinLengthMatchExpression;
class InternalSchemaMinPropertiesMatchExpression;
class InternalSchemaObjectMatchExpression;
class InternalSchemaRootDocEqMatchExpression;
class InternalSchemaTypeExpression;
class InternalSchemaUniqueItemsMatchExpression;
class InternalSchemaXorMatchExpression;
class LTEMatchExpression;
class LTMatchExpression;
class ModMatchExpression;
class NorMatchExpression;
class NotMatchExpression;
class OrMatchExpression;
class RegexMatchExpression;
class SizeMatchExpression;
class TextMatchExpression;
class TextNoOpMatchExpression;
class TwoDPtInAnnulusExpression;
class TypeMatchExpression;
class WhereMatchExpression;
class WhereNoOpMatchExpression;

/**
 * Visitor pattern for the MatchExpression tree.
 *
 * This code is not responsible for traversing the tree, only for performing the double-dispatch.
 *
 * If the visitor doesn't intend to modify the tree, then the template argument 'IsConst' should be
 * set to 'true'. In this case all 'visit()' methods will take a const pointer to a visiting node.
 */
template <bool IsConst = false>
class MatchExpressionVisitor {
public:
    template <typename T>
    using MaybeConstPtr = tree_walker::MaybeConstPtr<IsConst, T>;

    virtual ~MatchExpressionVisitor() = default;
    virtual void visit(MaybeConstPtr<AlwaysFalseMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<AlwaysTrueMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<AndMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<BitsAllClearMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<BitsAllSetMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<BitsAnyClearMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<BitsAnySetMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<ElemMatchObjectMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<ElemMatchValueMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<EqualityMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<ExistsMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<ExprMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<GTEMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<GTMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<GeoMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<GeoNearMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalBucketGeoWithinMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalExprEqMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalExprGTMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalExprGTEMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalExprLTMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalExprLTEMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalEqHashedKey> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaAllElemMatchFromIndexMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaAllowedPropertiesMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaBinDataEncryptedTypeExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaBinDataFLE2EncryptedTypeExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaBinDataSubTypeExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaCondMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaEqMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaFmodMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaMatchArrayIndexMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaMaxItemsMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaMaxLengthMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaMaxPropertiesMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaMinItemsMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaMinLengthMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaMinPropertiesMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaObjectMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaRootDocEqMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaTypeExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaUniqueItemsMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<InternalSchemaXorMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<LTEMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<LTMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<ModMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<NorMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<NotMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<OrMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<RegexMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<SizeMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<TextMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<TextNoOpMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<TwoDPtInAnnulusExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<TypeMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<WhereMatchExpression> expr) = 0;
    virtual void visit(MaybeConstPtr<WhereNoOpMatchExpression> expr) = 0;
};

using MatchExpressionMutableVisitor = MatchExpressionVisitor<false>;
using MatchExpressionConstVisitor = MatchExpressionVisitor<true>;

/**
 * This class provides null implementations for all visit methods so that a derived class can
 * override visit method(s) only for interested 'MatchExpression' types. For example, if one wants
 * to visit only 'EqualityMatchExpression', one can override only void visit(const
 * EqualityMatchExpression*).
 *
 * struct EqVisitor : public SelectiveMatchExpressionVisitorBase<true> {
 *     // To avoid overloaded-virtual warnings.
 *     using SelectiveMatchExpressionVisitorBase<true>::visit;
 *
 *     void visit(const EqualityMatchExpression* expr) final {
 *         // logic for what to do with an EqualityMatchExpression.
 *     }
 * };
 *
 * NOTE: Take caution when deriving from this class as you lose the compile-time safety of ensuring
 * that new expressions must consider the impact in the corresponding visitor implementation.
 */
template <bool IsConst>
struct SelectiveMatchExpressionVisitorBase : public MatchExpressionVisitor<IsConst> {
    template <typename T>
    using MaybeConstPtr = tree_walker::MaybeConstPtr<IsConst, T>;

    void visit(MaybeConstPtr<AlwaysFalseMatchExpression> expr) override {}
    void visit(MaybeConstPtr<AlwaysTrueMatchExpression> expr) override {}
    void visit(MaybeConstPtr<AndMatchExpression> expr) override {}
    void visit(MaybeConstPtr<BitsAllClearMatchExpression> expr) override {}
    void visit(MaybeConstPtr<BitsAllSetMatchExpression> expr) override {}
    void visit(MaybeConstPtr<BitsAnyClearMatchExpression> expr) override {}
    void visit(MaybeConstPtr<BitsAnySetMatchExpression> expr) override {}
    void visit(MaybeConstPtr<ElemMatchObjectMatchExpression> expr) override {}
    void visit(MaybeConstPtr<ElemMatchValueMatchExpression> expr) override {}
    void visit(MaybeConstPtr<EqualityMatchExpression> expr) override {}
    void visit(MaybeConstPtr<ExistsMatchExpression> expr) override {}
    void visit(MaybeConstPtr<ExprMatchExpression> expr) override {}
    void visit(MaybeConstPtr<GTEMatchExpression> expr) override {}
    void visit(MaybeConstPtr<GTMatchExpression> expr) override {}
    void visit(MaybeConstPtr<GeoMatchExpression> expr) override {}
    void visit(MaybeConstPtr<GeoNearMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalBucketGeoWithinMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalExprEqMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalExprGTMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalExprGTEMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalExprLTMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalExprLTEMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalEqHashedKey> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaAllElemMatchFromIndexMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaAllowedPropertiesMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaBinDataEncryptedTypeExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaBinDataFLE2EncryptedTypeExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaBinDataSubTypeExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaCondMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaEqMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaFmodMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaMatchArrayIndexMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaMaxItemsMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaMaxLengthMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaMaxPropertiesMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaMinItemsMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaMinLengthMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaMinPropertiesMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaObjectMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaRootDocEqMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaTypeExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaUniqueItemsMatchExpression> expr) override {}
    void visit(MaybeConstPtr<InternalSchemaXorMatchExpression> expr) override {}
    void visit(MaybeConstPtr<LTEMatchExpression> expr) override {}
    void visit(MaybeConstPtr<LTMatchExpression> expr) override {}
    void visit(MaybeConstPtr<ModMatchExpression> expr) override {}
    void visit(MaybeConstPtr<NorMatchExpression> expr) override {}
    void visit(MaybeConstPtr<NotMatchExpression> expr) override {}
    void visit(MaybeConstPtr<OrMatchExpression> expr) override {}
    void visit(MaybeConstPtr<RegexMatchExpression> expr) override {}
    void visit(MaybeConstPtr<SizeMatchExpression> expr) override {}
    void visit(MaybeConstPtr<TextMatchExpression> expr) override {}
    void visit(MaybeConstPtr<TextNoOpMatchExpression> expr) override {}
    void visit(MaybeConstPtr<TwoDPtInAnnulusExpression> expr) override {}
    void visit(MaybeConstPtr<TypeMatchExpression> expr) override {}
    void visit(MaybeConstPtr<WhereMatchExpression> expr) override {}
    void visit(MaybeConstPtr<WhereNoOpMatchExpression> expr) override {}
};

}  // namespace mongo
