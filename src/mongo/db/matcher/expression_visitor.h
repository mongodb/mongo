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
class InternalExprEqMatchExpression;
class InternalSchemaAllElemMatchFromIndexMatchExpression;
class InternalSchemaAllowedPropertiesMatchExpression;
class InternalSchemaBinDataEncryptedTypeExpression;
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
    virtual ~MatchExpressionVisitor() = default;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, AlwaysFalseMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, AlwaysTrueMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, AndMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, BitsAllClearMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, BitsAllSetMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, BitsAnyClearMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, BitsAnySetMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, ElemMatchObjectMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ElemMatchValueMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, EqualityMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ExistsMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ExprMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, GTEMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, GTMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, GeoMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, GeoNearMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, InMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, InternalExprEqMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaAllElemMatchFromIndexMatchExpression>
            expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaAllowedPropertiesMatchExpression>
            expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaBinDataEncryptedTypeExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaBinDataSubTypeExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaCondMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaEqMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaFmodMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaMatchArrayIndexMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaMaxItemsMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaMaxLengthMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaMaxPropertiesMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaMinItemsMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaMinLengthMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaMinPropertiesMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaObjectMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaRootDocEqMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, InternalSchemaTypeExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaUniqueItemsMatchExpression> expr) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, InternalSchemaXorMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, LTEMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, LTMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ModMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, NorMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, NotMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, OrMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, RegexMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, SizeMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, TextMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, TextNoOpMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, TwoDPtInAnnulusExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, TypeMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, WhereMatchExpression> expr) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, WhereNoOpMatchExpression> expr) = 0;
};

using MatchExpressionMutableVisitor = MatchExpressionVisitor<false>;
using MatchExpressionConstVisitor = MatchExpressionVisitor<true>;
}  // namespace mongo
