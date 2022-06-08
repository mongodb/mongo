/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/query/tree_walker.h"

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
class EncryptedBetweenMatchExpression;
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
    virtual void visit(MaybeConstPtr<EncryptedBetweenMatchExpression> expr) = 0;
};

using MatchExpressionMutableVisitor = MatchExpressionVisitor<false>;
using MatchExpressionConstVisitor = MatchExpressionVisitor<true>;
}  // namespace mongo
