/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/FoldConstants.h"

#include "mozilla/FloatingPoint.h"

#include "jslibmath.h"

#include "frontend/ParseNode.h"
#include "frontend/Parser.h"
#include "js/Conversions.h"
#include "vm/StringType.h"

#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::frontend;

using mozilla::IsNaN;
using mozilla::IsNegative;
using mozilla::NegativeInfinity;
using mozilla::PositiveInfinity;
using JS::GenericNaN;
using JS::ToInt32;
using JS::ToUint32;

static bool
ContainsHoistedDeclaration(JSContext* cx, ParseNode* node, bool* result);

static bool
ListContainsHoistedDeclaration(JSContext* cx, ListNode* list, bool* result)
{
    for (ParseNode* node = list->pn_head; node; node = node->pn_next) {
        if (!ContainsHoistedDeclaration(cx, node, result))
            return false;
        if (*result)
            return true;
    }

    *result = false;
    return true;
}

// Determines whether the given ParseNode contains any declarations whose
// visibility will extend outside the node itself -- that is, whether the
// ParseNode contains any var statements.
//
// THIS IS NOT A GENERAL-PURPOSE FUNCTION.  It is only written to work in the
// specific context of deciding that |node|, as one arm of a ParseNodeKind::If
// controlled by a constant condition, contains a declaration that forbids
// |node| being completely eliminated as dead.
static bool
ContainsHoistedDeclaration(JSContext* cx, ParseNode* node, bool* result)
{
    if (!CheckRecursionLimit(cx))
        return false;

  restart:

    // With a better-typed AST, we would have distinct parse node classes for
    // expressions and for statements and would characterize expressions with
    // ExpressionKind and statements with StatementKind.  Perhaps someday.  In
    // the meantime we must characterize every ParseNodeKind, even the
    // expression/sub-expression ones that, if we handle all statement kinds
    // correctly, we'll never see.
    switch (node->getKind()) {
      // Base case.
      case ParseNodeKind::Var:
        *result = true;
        return true;

      // Non-global lexical declarations are block-scoped (ergo not hoistable).
      case ParseNodeKind::Let:
      case ParseNodeKind::Const:
        MOZ_ASSERT(node->isArity(PN_LIST));
        *result = false;
        return true;

      // Similarly to the lexical declarations above, classes cannot add hoisted
      // declarations
      case ParseNodeKind::Class:
        MOZ_ASSERT(node->isArity(PN_TERNARY));
        *result = false;
        return true;

      // Function declarations *can* be hoisted declarations.  But in the
      // magical world of the rewritten frontend, the declaration necessitated
      // by a nested function statement, not at body level, doesn't require
      // that we preserve an unreachable function declaration node against
      // dead-code removal.
      case ParseNodeKind::Function:
        MOZ_ASSERT(node->isArity(PN_CODE));
        *result = false;
        return true;

      case ParseNodeKind::Module:
        *result = false;
        return true;

      // Statements with no sub-components at all.
      case ParseNodeKind::EmptyStatement:
      case ParseNodeKind::Debugger:
        MOZ_ASSERT(node->isArity(PN_NULLARY));
        *result = false;
        return true;

      // Statements containing only an expression have no declarations.
      case ParseNodeKind::ExpressionStatement:
      case ParseNodeKind::Throw:
      case ParseNodeKind::Return:
        MOZ_ASSERT(node->isArity(PN_UNARY));
        *result = false;
        return true;

      // These two aren't statements in the spec, but we sometimes insert them
      // in statement lists anyway.
      case ParseNodeKind::InitialYield:
      case ParseNodeKind::YieldStar:
      case ParseNodeKind::Yield:
        MOZ_ASSERT(node->isArity(PN_UNARY));
        *result = false;
        return true;

      // Other statements with no sub-statement components.
      case ParseNodeKind::Break:
      case ParseNodeKind::Continue:
      case ParseNodeKind::Import:
      case ParseNodeKind::ImportSpecList:
      case ParseNodeKind::ImportSpec:
      case ParseNodeKind::ExportFrom:
      case ParseNodeKind::ExportDefault:
      case ParseNodeKind::ExportSpecList:
      case ParseNodeKind::ExportSpec:
      case ParseNodeKind::Export:
      case ParseNodeKind::ExportBatchSpec:
        *result = false;
        return true;

      // Statements possibly containing hoistable declarations only in the left
      // half, in ParseNode terms -- the loop body in AST terms.
      case ParseNodeKind::DoWhile:
        return ContainsHoistedDeclaration(cx, node->pn_left, result);

      // Statements possibly containing hoistable declarations only in the
      // right half, in ParseNode terms -- the loop body or nested statement
      // (usually a block statement), in AST terms.
      case ParseNodeKind::While:
      case ParseNodeKind::With:
        return ContainsHoistedDeclaration(cx, node->pn_right, result);

      case ParseNodeKind::Label:
        return ContainsHoistedDeclaration(cx, node->pn_expr, result);

      // Statements with more complicated structures.

      // if-statement nodes may have hoisted declarations in their consequent
      // and alternative components.
      case ParseNodeKind::If: {
        MOZ_ASSERT(node->isArity(PN_TERNARY));

        ParseNode* consequent = node->pn_kid2;
        if (!ContainsHoistedDeclaration(cx, consequent, result))
            return false;
        if (*result)
            return true;

        if ((node = node->pn_kid3))
            goto restart;

        *result = false;
        return true;
      }

      // try-statements have statements to execute, and one or both of a
      // catch-list and a finally-block.
      case ParseNodeKind::Try: {
        MOZ_ASSERT(node->isArity(PN_TERNARY));
        MOZ_ASSERT(node->pn_kid2 || node->pn_kid3,
                   "must have either catch(es) or finally");

        ParseNode* tryBlock = node->pn_kid1;
        if (!ContainsHoistedDeclaration(cx, tryBlock, result))
            return false;
        if (*result)
            return true;

        if (ParseNode* catchScope = node->pn_kid2) {
            MOZ_ASSERT(catchScope->isKind(ParseNodeKind::LexicalScope));

            ParseNode* catchNode = catchScope->pn_expr;
            MOZ_ASSERT(catchNode->isKind(ParseNodeKind::Catch));

            ParseNode* catchStatements = catchNode->pn_right;
            if (!ContainsHoistedDeclaration(cx, catchStatements, result))
                return false;
            if (*result)
                return true;
        }

        if (ParseNode* finallyBlock = node->pn_kid3)
            return ContainsHoistedDeclaration(cx, finallyBlock, result);

        *result = false;
        return true;
      }

      // A switch node's left half is an expression; only its right half (a
      // list of cases/defaults, or a block node) could contain hoisted
      // declarations.
      case ParseNodeKind::Switch:
        MOZ_ASSERT(node->isArity(PN_BINARY));
        return ContainsHoistedDeclaration(cx, node->pn_right, result);

      case ParseNodeKind::Case:
        return ContainsHoistedDeclaration(cx, node->as<CaseClause>().statementList(), result);

      case ParseNodeKind::For: {
        MOZ_ASSERT(node->isArity(PN_BINARY));

        ParseNode* loopHead = node->pn_left;
        MOZ_ASSERT(loopHead->isKind(ParseNodeKind::ForHead) ||
                   loopHead->isKind(ParseNodeKind::ForIn) ||
                   loopHead->isKind(ParseNodeKind::ForOf));

        if (loopHead->isKind(ParseNodeKind::ForHead)) {
            // for (init?; cond?; update?), with only init possibly containing
            // a hoisted declaration.  (Note: a lexical-declaration |init| is
            // (at present) hoisted in SpiderMonkey parlance -- but such
            // hoisting doesn't extend outside of this statement, so it is not
            // hoisting in the sense meant by ContainsHoistedDeclaration.)
            MOZ_ASSERT(loopHead->isArity(PN_TERNARY));

            ParseNode* init = loopHead->pn_kid1;
            if (init && init->isKind(ParseNodeKind::Var)) {
                *result = true;
                return true;
            }
        } else {
            MOZ_ASSERT(loopHead->isKind(ParseNodeKind::ForIn) ||
                       loopHead->isKind(ParseNodeKind::ForOf));

            // for each? (target in ...), where only target may introduce
            // hoisted declarations.
            //
            //   -- or --
            //
            // for (target of ...), where only target may introduce hoisted
            // declarations.
            //
            // Either way, if |target| contains a declaration, it's |loopHead|'s
            // first kid.
            MOZ_ASSERT(loopHead->isArity(PN_TERNARY));

            ParseNode* decl = loopHead->pn_kid1;
            if (decl && decl->isKind(ParseNodeKind::Var)) {
                *result = true;
                return true;
            }
        }

        ParseNode* loopBody = node->pn_right;
        return ContainsHoistedDeclaration(cx, loopBody, result);
      }

      case ParseNodeKind::LexicalScope: {
        MOZ_ASSERT(node->isArity(PN_SCOPE));
        ParseNode* expr = node->pn_expr;

        if (expr->isKind(ParseNodeKind::For) || expr->isKind(ParseNodeKind::Function))
            return ContainsHoistedDeclaration(cx, expr, result);

        MOZ_ASSERT(expr->isKind(ParseNodeKind::StatementList));
        return ListContainsHoistedDeclaration(cx, &node->pn_expr->as<ListNode>(), result);
      }

      // List nodes with all non-null children.
      case ParseNodeKind::StatementList:
        return ListContainsHoistedDeclaration(cx, &node->as<ListNode>(), result);

      // Grammar sub-components that should never be reached directly by this
      // method, because some parent component should have asserted itself.
      case ParseNodeKind::ObjectPropertyName:
      case ParseNodeKind::ComputedName:
      case ParseNodeKind::Spread:
      case ParseNodeKind::MutateProto:
      case ParseNodeKind::Colon:
      case ParseNodeKind::Shorthand:
      case ParseNodeKind::Conditional:
      case ParseNodeKind::TypeOfName:
      case ParseNodeKind::TypeOfExpr:
      case ParseNodeKind::Await:
      case ParseNodeKind::Void:
      case ParseNodeKind::Not:
      case ParseNodeKind::BitNot:
      case ParseNodeKind::DeleteName:
      case ParseNodeKind::DeleteProp:
      case ParseNodeKind::DeleteElem:
      case ParseNodeKind::DeleteExpr:
      case ParseNodeKind::Pos:
      case ParseNodeKind::Neg:
      case ParseNodeKind::PreIncrement:
      case ParseNodeKind::PostIncrement:
      case ParseNodeKind::PreDecrement:
      case ParseNodeKind::PostDecrement:
      case ParseNodeKind::Or:
      case ParseNodeKind::And:
      case ParseNodeKind::BitOr:
      case ParseNodeKind::BitXor:
      case ParseNodeKind::BitAnd:
      case ParseNodeKind::StrictEq:
      case ParseNodeKind::Eq:
      case ParseNodeKind::StrictNe:
      case ParseNodeKind::Ne:
      case ParseNodeKind::Lt:
      case ParseNodeKind::Le:
      case ParseNodeKind::Gt:
      case ParseNodeKind::Ge:
      case ParseNodeKind::InstanceOf:
      case ParseNodeKind::In:
      case ParseNodeKind::Lsh:
      case ParseNodeKind::Rsh:
      case ParseNodeKind::Ursh:
      case ParseNodeKind::Add:
      case ParseNodeKind::Sub:
      case ParseNodeKind::Star:
      case ParseNodeKind::Div:
      case ParseNodeKind::Mod:
      case ParseNodeKind::Pow:
      case ParseNodeKind::Assign:
      case ParseNodeKind::AddAssign:
      case ParseNodeKind::SubAssign:
      case ParseNodeKind::BitOrAssign:
      case ParseNodeKind::BitXorAssign:
      case ParseNodeKind::BitAndAssign:
      case ParseNodeKind::LshAssign:
      case ParseNodeKind::RshAssign:
      case ParseNodeKind::UrshAssign:
      case ParseNodeKind::MulAssign:
      case ParseNodeKind::DivAssign:
      case ParseNodeKind::ModAssign:
      case ParseNodeKind::PowAssign:
      case ParseNodeKind::Comma:
      case ParseNodeKind::Array:
      case ParseNodeKind::Object:
      case ParseNodeKind::Dot:
      case ParseNodeKind::Elem:
      case ParseNodeKind::Call:
      case ParseNodeKind::Name:
      case ParseNodeKind::TemplateString:
      case ParseNodeKind::TemplateStringList:
      case ParseNodeKind::TaggedTemplate:
      case ParseNodeKind::CallSiteObj:
      case ParseNodeKind::String:
      case ParseNodeKind::RegExp:
      case ParseNodeKind::True:
      case ParseNodeKind::False:
      case ParseNodeKind::Null:
      case ParseNodeKind::RawUndefined:
      case ParseNodeKind::This:
      case ParseNodeKind::Elision:
      case ParseNodeKind::Number:
      case ParseNodeKind::New:
      case ParseNodeKind::Generator:
      case ParseNodeKind::ParamsBody:
      case ParseNodeKind::Catch:
      case ParseNodeKind::ForIn:
      case ParseNodeKind::ForOf:
      case ParseNodeKind::ForHead:
      case ParseNodeKind::ClassMethod:
      case ParseNodeKind::ClassMethodList:
      case ParseNodeKind::ClassNames:
      case ParseNodeKind::NewTarget:
      case ParseNodeKind::PosHolder:
      case ParseNodeKind::SuperCall:
      case ParseNodeKind::SuperBase:
      case ParseNodeKind::SetThis:
        MOZ_CRASH("ContainsHoistedDeclaration should have indicated false on "
                  "some parent node without recurring to test this node");

      case ParseNodeKind::Pipeline:
        MOZ_ASSERT(node->isArity(PN_LIST));
        *result = false;
        return true;

      case ParseNodeKind::Limit: // invalid sentinel value
        MOZ_CRASH("unexpected ParseNodeKind::Limit in node");
    }

    MOZ_CRASH("invalid node kind");
}

/*
 * Fold from one constant type to another.
 * XXX handles only strings and numbers for now
 */
static bool
FoldType(JSContext* cx, ParseNode* pn, ParseNodeKind kind)
{
    if (!pn->isKind(kind)) {
        switch (kind) {
          case ParseNodeKind::Number:
            if (pn->isKind(ParseNodeKind::String)) {
                double d;
                if (!StringToNumber(cx, pn->pn_atom, &d))
                    return false;
                pn->pn_dval = d;
                pn->setKind(ParseNodeKind::Number);
                pn->setOp(JSOP_DOUBLE);
            }
            break;

          case ParseNodeKind::String:
            if (pn->isKind(ParseNodeKind::Number)) {
                pn->pn_atom = NumberToAtom(cx, pn->pn_dval);
                if (!pn->pn_atom)
                    return false;
                pn->setKind(ParseNodeKind::String);
                pn->setOp(JSOP_STRING);
            }
            break;

          default:;
        }
    }
    return true;
}

// Remove a ParseNode, **pnp, from a parse tree, putting another ParseNode,
// *pn, in its place.
//
// pnp points to a ParseNode pointer. This must be the only pointer that points
// to the parse node being replaced. The replacement, *pn, is unchanged except
// for its pn_next pointer; updating that is necessary if *pn's new parent is a
// list node.
static void
ReplaceNode(ParseNode** pnp, ParseNode* pn)
{
    pn->pn_next = (*pnp)->pn_next;
    *pnp = pn;
}

static bool
IsEffectless(ParseNode* node)
{
    return node->isKind(ParseNodeKind::True) ||
           node->isKind(ParseNodeKind::False) ||
           node->isKind(ParseNodeKind::String) ||
           node->isKind(ParseNodeKind::TemplateString) ||
           node->isKind(ParseNodeKind::Number) ||
           node->isKind(ParseNodeKind::Null) ||
           node->isKind(ParseNodeKind::RawUndefined) ||
           node->isKind(ParseNodeKind::Function);
}

enum Truthiness { Truthy, Falsy, Unknown };

static Truthiness
Boolish(ParseNode* pn)
{
    switch (pn->getKind()) {
      case ParseNodeKind::Number:
        return (pn->pn_dval != 0 && !IsNaN(pn->pn_dval)) ? Truthy : Falsy;

      case ParseNodeKind::String:
      case ParseNodeKind::TemplateString:
        return (pn->pn_atom->length() > 0) ? Truthy : Falsy;

      case ParseNodeKind::True:
      case ParseNodeKind::Function:
        return Truthy;

      case ParseNodeKind::False:
      case ParseNodeKind::Null:
      case ParseNodeKind::RawUndefined:
        return Falsy;

      case ParseNodeKind::Void: {
        // |void <foo>| evaluates to |undefined| which isn't truthy.  But the
        // sense of this method requires that the expression be literally
        // replaceable with true/false: not the case if the nested expression
        // is effectful, might throw, &c.  Walk past the |void| (and nested
        // |void| expressions, for good measure) and check that the nested
        // expression doesn't break this requirement before indicating falsity.
        do {
            pn = pn->pn_kid;
        } while (pn->isKind(ParseNodeKind::Void));

        return IsEffectless(pn) ? Falsy : Unknown;
      }

      default:
        return Unknown;
    }
}

static bool
Fold(JSContext* cx, ParseNode** pnp, PerHandlerParser<FullParseHandler>& parser);

static bool
FoldCondition(JSContext* cx, ParseNode** nodePtr, PerHandlerParser<FullParseHandler>& parser)
{
    // Conditions fold like any other expression...
    if (!Fold(cx, nodePtr, parser))
        return false;

    // ...but then they sometimes can be further folded to constants.
    ParseNode* node = *nodePtr;
    Truthiness t = Boolish(node);
    if (t != Unknown) {
        // We can turn function nodes into constant nodes here, but mutating
        // function nodes is tricky --- in particular, mutating a function node
        // that appears on a method list corrupts the method list. However,
        // methods are M's in statements of the form 'this.foo = M;', which we
        // never fold, so we're okay.
        parser.prepareNodeForMutation(node);
        if (t == Truthy) {
            node->setKind(ParseNodeKind::True);
            node->setOp(JSOP_TRUE);
        } else {
            node->setKind(ParseNodeKind::False);
            node->setOp(JSOP_FALSE);
        }
        node->setArity(PN_NULLARY);
    }

    return true;
}

static bool
FoldTypeOfExpr(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::TypeOfExpr));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode*& expr = node->pn_kid;
    if (!Fold(cx, &expr, parser))
        return false;

    // Constant-fold the entire |typeof| if given a constant with known type.
    RootedPropertyName result(cx);
    if (expr->isKind(ParseNodeKind::String) || expr->isKind(ParseNodeKind::TemplateString))
        result = cx->names().string;
    else if (expr->isKind(ParseNodeKind::Number))
        result = cx->names().number;
    else if (expr->isKind(ParseNodeKind::Null))
        result = cx->names().object;
    else if (expr->isKind(ParseNodeKind::True) || expr->isKind(ParseNodeKind::False))
        result = cx->names().boolean;
    else if (expr->isKind(ParseNodeKind::Function))
        result = cx->names().function;

    if (result) {
        parser.prepareNodeForMutation(node);

        node->setKind(ParseNodeKind::String);
        node->setArity(PN_NULLARY);
        node->setOp(JSOP_NOP);
        node->pn_atom = result;
    }

    return true;
}

static bool
FoldDeleteExpr(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::DeleteExpr));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode*& expr = node->pn_kid;
    if (!Fold(cx, &expr, parser))
        return false;

    // Expression deletion evaluates the expression, then evaluates to true.
    // For effectless expressions, eliminate the expression evaluation.
    if (IsEffectless(expr)) {
        parser.prepareNodeForMutation(node);
        node->setKind(ParseNodeKind::True);
        node->setArity(PN_NULLARY);
        node->setOp(JSOP_TRUE);
    }

    return true;
}

static bool
FoldDeleteElement(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::DeleteElem));
    MOZ_ASSERT(node->isArity(PN_UNARY));
    MOZ_ASSERT(node->pn_kid->isKind(ParseNodeKind::Elem));

    ParseNode*& expr = node->pn_kid;
    if (!Fold(cx, &expr, parser))
        return false;

    // If we're deleting an element, but constant-folding converted our
    // element reference into a dotted property access, we must *also*
    // morph the node's kind.
    //
    // In principle this also applies to |super["foo"] -> super.foo|,
    // but we don't constant-fold |super["foo"]| yet.
    MOZ_ASSERT(expr->isKind(ParseNodeKind::Elem) || expr->isKind(ParseNodeKind::Dot));
    if (expr->isKind(ParseNodeKind::Dot))
        node->setKind(ParseNodeKind::DeleteProp);

    return true;
}

static bool
FoldDeleteProperty(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::DeleteProp));
    MOZ_ASSERT(node->isArity(PN_UNARY));
    MOZ_ASSERT(node->pn_kid->isKind(ParseNodeKind::Dot));

    ParseNode*& expr = node->pn_kid;
#ifdef DEBUG
    ParseNodeKind oldKind = expr->getKind();
#endif

    if (!Fold(cx, &expr, parser))
        return false;

    MOZ_ASSERT(expr->isKind(oldKind),
               "kind should have remained invariant under folding");

    return true;
}

static bool
FoldNot(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Not));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode*& expr = node->pn_kid;
    if (!FoldCondition(cx, &expr, parser))
        return false;

    if (expr->isKind(ParseNodeKind::Number)) {
        double d = expr->pn_dval;

        parser.prepareNodeForMutation(node);
        if (d == 0 || IsNaN(d)) {
            node->setKind(ParseNodeKind::True);
            node->setOp(JSOP_TRUE);
        } else {
            node->setKind(ParseNodeKind::False);
            node->setOp(JSOP_FALSE);
        }
        node->setArity(PN_NULLARY);
    } else if (expr->isKind(ParseNodeKind::True) || expr->isKind(ParseNodeKind::False)) {
        bool newval = !expr->isKind(ParseNodeKind::True);

        parser.prepareNodeForMutation(node);
        node->setKind(newval ? ParseNodeKind::True : ParseNodeKind::False);
        node->setArity(PN_NULLARY);
        node->setOp(newval ? JSOP_TRUE : JSOP_FALSE);
    }

    return true;
}

static bool
FoldUnaryArithmetic(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::BitNot) ||
               node->isKind(ParseNodeKind::Pos) ||
               node->isKind(ParseNodeKind::Neg),
               "need a different method for this node kind");
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode*& expr = node->pn_kid;
    if (!Fold(cx, &expr, parser))
        return false;

    if (expr->isKind(ParseNodeKind::Number) ||
        expr->isKind(ParseNodeKind::True) ||
        expr->isKind(ParseNodeKind::False))
    {
        double d = expr->isKind(ParseNodeKind::Number)
                   ? expr->pn_dval
                   : double(expr->isKind(ParseNodeKind::True));

        if (node->isKind(ParseNodeKind::BitNot))
            d = ~ToInt32(d);
        else if (node->isKind(ParseNodeKind::Neg))
            d = -d;
        else
            MOZ_ASSERT(node->isKind(ParseNodeKind::Pos)); // nothing to do

        parser.prepareNodeForMutation(node);
        node->setKind(ParseNodeKind::Number);
        node->setOp(JSOP_DOUBLE);
        node->setArity(PN_NULLARY);
        node->pn_dval = d;
    }

    return true;
}

static bool
FoldIncrementDecrement(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::PreIncrement) ||
               node->isKind(ParseNodeKind::PostIncrement) ||
               node->isKind(ParseNodeKind::PreDecrement) ||
               node->isKind(ParseNodeKind::PostDecrement));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode*& target = node->pn_kid;
    MOZ_ASSERT(parser.isValidSimpleAssignmentTarget(target, PermitAssignmentToFunctionCalls));

    if (!Fold(cx, &target, parser))
        return false;

    MOZ_ASSERT(parser.isValidSimpleAssignmentTarget(target, PermitAssignmentToFunctionCalls));

    return true;
}

static bool
FoldAndOr(JSContext* cx, ParseNode** nodePtr, PerHandlerParser<FullParseHandler>& parser)
{
    ParseNode* node = *nodePtr;

    MOZ_ASSERT(node->isKind(ParseNodeKind::And) || node->isKind(ParseNodeKind::Or));
    MOZ_ASSERT(node->isArity(PN_LIST));

    bool isOrNode = node->isKind(ParseNodeKind::Or);
    ParseNode** elem = &node->pn_head;
    do {
        if (!Fold(cx, elem, parser))
            return false;

        Truthiness t = Boolish(*elem);

        // If we don't know the constant-folded node's truthiness, we can't
        // reduce this node with its surroundings.  Continue folding any
        // remaining nodes.
        if (t == Unknown) {
            elem = &(*elem)->pn_next;
            continue;
        }

        // If the constant-folded node's truthiness will terminate the
        // condition -- `a || true || expr` or |b && false && expr| -- then
        // trailing nodes will never be evaluated.  Truncate the list after
        // the known-truthiness node, as it's the overall result.
        if ((t == Truthy) == isOrNode) {
            ParseNode* afterNext;
            for (ParseNode* next = (*elem)->pn_next; next; next = afterNext) {
                afterNext = next->pn_next;
                parser.freeTree(next);
                --node->pn_count;
            }

            // Terminate the original and/or list at the known-truthiness
            // node.
            (*elem)->pn_next = nullptr;
            elem = &(*elem)->pn_next;
            break;
        }

        MOZ_ASSERT((t == Truthy) == !isOrNode);

        // We've encountered a vacuous node that'll never short- circuit
        // evaluation.
        if ((*elem)->pn_next) {
            // This node is never the overall result when there are
            // subsequent nodes.  Remove it.
            ParseNode* elt = *elem;
            *elem = elt->pn_next;
            parser.freeTree(elt);
            --node->pn_count;
        } else {
            // Otherwise this node is the result of the overall expression,
            // so leave it alone.  And we're done.
            elem = &(*elem)->pn_next;
            break;
        }
    } while (*elem);

    // If the last node in the list was replaced, we need to update the
    // tail pointer in the original and/or node.
    node->pn_tail = elem;

    node->checkListConsistency();

    // If we removed nodes, we may have to replace a one-element list with
    // its element.
    if (node->pn_count == 1) {
        ParseNode* first = node->pn_head;
        ReplaceNode(nodePtr, first);

        node->setKind(ParseNodeKind::Null);
        node->setArity(PN_NULLARY);
        parser.freeTree(node);
    }

    return true;
}

static bool
FoldConditional(JSContext* cx, ParseNode** nodePtr, PerHandlerParser<FullParseHandler>& parser)
{
    ParseNode** nextNode = nodePtr;

    do {
        // |nextNode| on entry points to the C?T:F expression to be folded.
        // Reset it to exit the loop in the common case where F isn't another
        // ?: expression.
        nodePtr = nextNode;
        nextNode = nullptr;

        ParseNode* node = *nodePtr;
        MOZ_ASSERT(node->isKind(ParseNodeKind::Conditional));
        MOZ_ASSERT(node->isArity(PN_TERNARY));

        ParseNode*& expr = node->pn_kid1;
        if (!FoldCondition(cx, &expr, parser))
            return false;

        ParseNode*& ifTruthy = node->pn_kid2;
        if (!Fold(cx, &ifTruthy, parser))
            return false;

        ParseNode*& ifFalsy = node->pn_kid3;

        // If our C?T:F node has F as another ?: node, *iteratively* constant-
        // fold F *after* folding C and T (and possibly eliminating C and one
        // of T/F entirely); otherwise fold F normally.  Making |nextNode| non-
        // null causes this loop to run again to fold F.
        //
        // Conceivably we could instead/also iteratively constant-fold T, if T
        // were more complex than F.  Such an optimization is unimplemented.
        if (ifFalsy->isKind(ParseNodeKind::Conditional)) {
            nextNode = &ifFalsy;
        } else {
            if (!Fold(cx, &ifFalsy, parser))
                return false;
        }

        // Try to constant-fold based on the condition expression.
        Truthiness t = Boolish(expr);
        if (t == Unknown)
            continue;

        // Otherwise reduce 'C ? T : F' to T or F as directed by C.
        ParseNode* replacement;
        ParseNode* discarded;
        if (t == Truthy) {
            replacement = ifTruthy;
            discarded = ifFalsy;
        } else {
            replacement = ifFalsy;
            discarded = ifTruthy;
        }

        // Otherwise perform a replacement.  This invalidates |nextNode|, so
        // reset it (if the replacement requires folding) or clear it (if
        // |ifFalsy| is dead code) as needed.
        if (nextNode)
            nextNode = (*nextNode == replacement) ? nodePtr : nullptr;
        ReplaceNode(nodePtr, replacement);

        parser.freeTree(discarded);
    } while (nextNode);

    return true;
}

static bool
FoldIf(JSContext* cx, ParseNode** nodePtr, PerHandlerParser<FullParseHandler>& parser)
{
    ParseNode** nextNode = nodePtr;

    do {
        // |nextNode| on entry points to the initial |if| to be folded.  Reset
        // it to exit the loop when the |else| arm isn't another |if|.
        nodePtr = nextNode;
        nextNode = nullptr;

        ParseNode* node = *nodePtr;
        MOZ_ASSERT(node->isKind(ParseNodeKind::If));
        MOZ_ASSERT(node->isArity(PN_TERNARY));

        ParseNode*& expr = node->pn_kid1;
        if (!FoldCondition(cx, &expr, parser))
            return false;

        ParseNode*& consequent = node->pn_kid2;
        if (!Fold(cx, &consequent, parser))
            return false;

        ParseNode*& alternative = node->pn_kid3;
        if (alternative) {
            // If in |if (C) T; else F;| we have |F| as another |if|,
            // *iteratively* constant-fold |F| *after* folding |C| and |T| (and
            // possibly completely replacing the whole thing with |T| or |F|);
            // otherwise fold F normally.  Making |nextNode| non-null causes
            // this loop to run again to fold F.
            if (alternative->isKind(ParseNodeKind::If)) {
                nextNode = &alternative;
            } else {
                if (!Fold(cx, &alternative, parser))
                    return false;
            }
        }

        // Eliminate the consequent or alternative if the condition has
        // constant truthiness.
        Truthiness t = Boolish(expr);
        if (t == Unknown)
            continue;

        // Careful!  Either of these can be null: |replacement| in |if (0) T;|,
        // and |discarded| in |if (true) T;|.
        ParseNode* replacement;
        ParseNode* discarded;
        if (t == Truthy) {
            replacement = consequent;
            discarded = alternative;
        } else {
            replacement = alternative;
            discarded = consequent;
        }

        bool performReplacement = true;
        if (discarded) {
            // A declaration that hoists outside the discarded arm prevents the
            // |if| from being folded away.
            bool containsHoistedDecls;
            if (!ContainsHoistedDeclaration(cx, discarded, &containsHoistedDecls))
                return false;

            performReplacement = !containsHoistedDecls;
        }

        if (!performReplacement)
            continue;

        if (!replacement) {
            // If there's no replacement node, we have a constantly-false |if|
            // with no |else|.  Replace the entire thing with an empty
            // statement list.
            parser.prepareNodeForMutation(node);
            node->setKind(ParseNodeKind::StatementList);
            node->setArity(PN_LIST);
            node->makeEmpty();
        } else {
            // Replacement invalidates |nextNode|, so reset it (if the
            // replacement requires folding) or clear it (if |alternative|
            // is dead code) as needed.
            if (nextNode)
                nextNode = (*nextNode == replacement) ? nodePtr : nullptr;
            ReplaceNode(nodePtr, replacement);

            // Morph the original node into a discardable node, then
            // aggressively free it and the discarded arm (if any) to suss out
            // any bugs in the preceding logic.
            node->setKind(ParseNodeKind::StatementList);
            node->setArity(PN_LIST);
            node->makeEmpty();
            if (discarded)
                node->append(discarded);
            parser.freeTree(node);
        }
    } while (nextNode);

    return true;
}

static bool
FoldFunction(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Function));
    MOZ_ASSERT(node->isArity(PN_CODE));

    // Don't constant-fold inside "use asm" code, as this could create a parse
    // tree that doesn't type-check as asm.js.
    if (node->pn_funbox->useAsmOrInsideUseAsm())
        return true;

    // Note: pn_body is null for lazily-parsed functions.
    if (ParseNode*& functionBody = node->pn_body) {
        if (!Fold(cx, &functionBody, parser))
            return false;
    }

    return true;
}

static double
ComputeBinary(ParseNodeKind kind, double left, double right)
{
    if (kind == ParseNodeKind::Add)
        return left + right;

    if (kind == ParseNodeKind::Sub)
        return left - right;

    if (kind == ParseNodeKind::Star)
        return left * right;

    if (kind == ParseNodeKind::Mod)
        return right == 0 ? GenericNaN() : js_fmod(left, right);

    if (kind == ParseNodeKind::Ursh)
        return ToUint32(left) >> (ToUint32(right) & 31);

    if (kind == ParseNodeKind::Div) {
        if (right == 0) {
#if defined(XP_WIN)
            /* XXX MSVC miscompiles such that (NaN == 0) */
            if (IsNaN(right))
                return GenericNaN();
#endif
            if (left == 0 || IsNaN(left))
                return GenericNaN();
            if (IsNegative(left) != IsNegative(right))
                return NegativeInfinity<double>();
            return PositiveInfinity<double>();
        }

        return left / right;
    }

    MOZ_ASSERT(kind == ParseNodeKind::Lsh || kind == ParseNodeKind::Rsh);

    int32_t i = ToInt32(left);
    uint32_t j = ToUint32(right) & 31;
    return int32_t((kind == ParseNodeKind::Lsh) ? uint32_t(i) << j : i >> j);
}

static bool
FoldModule(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Module));
    MOZ_ASSERT(node->isArity(PN_CODE));

    ParseNode*& moduleBody = node->pn_body;
    MOZ_ASSERT(moduleBody);
    return Fold(cx, &moduleBody, parser);
}

static bool
FoldBinaryArithmetic(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Sub) ||
               node->isKind(ParseNodeKind::Star) ||
               node->isKind(ParseNodeKind::Lsh) ||
               node->isKind(ParseNodeKind::Rsh) ||
               node->isKind(ParseNodeKind::Ursh) ||
               node->isKind(ParseNodeKind::Div) ||
               node->isKind(ParseNodeKind::Mod));
    MOZ_ASSERT(node->isArity(PN_LIST));
    MOZ_ASSERT(node->pn_count >= 2);

    // Fold each operand, ideally into a number.
    ParseNode** listp = &node->pn_head;
    for (; *listp; listp = &(*listp)->pn_next) {
        if (!Fold(cx, listp, parser))
            return false;

        if (!FoldType(cx, *listp, ParseNodeKind::Number))
            return false;
    }

    // Repoint the list's tail pointer.
    node->pn_tail = listp;

    // Now fold all leading numeric terms together into a single number.
    // (Trailing terms for the non-shift operations can't be folded together
    // due to floating point imprecision.  For example, if |x === -2**53|,
    // |x - 1 - 1 === -2**53| but |x - 2 === -2**53 - 2|.  Shifts could be
    // folded, but it doesn't seem worth the effort.)
    ParseNode* elem = node->pn_head;
    ParseNode* next = elem->pn_next;
    if (elem->isKind(ParseNodeKind::Number)) {
        ParseNodeKind kind = node->getKind();
        while (true) {
            if (!next || !next->isKind(ParseNodeKind::Number))
                break;

            double d = ComputeBinary(kind, elem->pn_dval, next->pn_dval);

            ParseNode* afterNext = next->pn_next;
            parser.freeTree(next);
            next = afterNext;
            elem->pn_next = next;

            elem->setKind(ParseNodeKind::Number);
            elem->setOp(JSOP_DOUBLE);
            elem->setArity(PN_NULLARY);
            elem->pn_dval = d;

            node->pn_count--;
        }

        if (node->pn_count == 1) {
            MOZ_ASSERT(node->pn_head == elem);
            MOZ_ASSERT(elem->isKind(ParseNodeKind::Number));

            double d = elem->pn_dval;
            node->setKind(ParseNodeKind::Number);
            node->setArity(PN_NULLARY);
            node->setOp(JSOP_DOUBLE);
            node->pn_dval = d;

            parser.freeTree(elem);
        }
    }

    return true;
}

static bool
FoldExponentiation(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Pow));
    MOZ_ASSERT(node->isArity(PN_LIST));
    MOZ_ASSERT(node->pn_count >= 2);

    // Fold each operand, ideally into a number.
    ParseNode** listp = &node->pn_head;
    for (; *listp; listp = &(*listp)->pn_next) {
        if (!Fold(cx, listp, parser))
            return false;

        if (!FoldType(cx, *listp, ParseNodeKind::Number))
            return false;
    }

    // Repoint the list's tail pointer.
    node->pn_tail = listp;

    // Unlike all other binary arithmetic operators, ** is right-associative:
    // 2**3**5 is 2**(3**5), not (2**3)**5.  As list nodes singly-link their
    // children, full constant-folding requires either linear space or dodgy
    // in-place linked list reversal.  So we only fold one exponentiation: it's
    // easy and addresses common cases like |2**32|.
    if (node->pn_count > 2)
        return true;

    ParseNode* base = node->pn_head;
    ParseNode* exponent = base->pn_next;
    if (!base->isKind(ParseNodeKind::Number) || !exponent->isKind(ParseNodeKind::Number))
        return true;

    double d1 = base->pn_dval, d2 = exponent->pn_dval;

    parser.prepareNodeForMutation(node);
    node->setKind(ParseNodeKind::Number);
    node->setArity(PN_NULLARY);
    node->setOp(JSOP_DOUBLE);
    node->pn_dval = ecmaPow(d1, d2);
    return true;
}

static bool
FoldList(JSContext* cx, ParseNode* list, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(list->isArity(PN_LIST));

    ParseNode** elem = &list->pn_head;
    for (; *elem; elem = &(*elem)->pn_next) {
        if (!Fold(cx, elem, parser))
            return false;
    }

    // Repoint the list's tail pointer if the final element was replaced.
    list->pn_tail = elem;

    list->checkListConsistency();

    return true;
}

static bool
FoldReturn(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Return));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    if (ParseNode*& expr = node->pn_kid) {
        if (!Fold(cx, &expr, parser))
            return false;
    }

    return true;
}

static bool
FoldTry(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Try));
    MOZ_ASSERT(node->isArity(PN_TERNARY));

    ParseNode*& statements = node->pn_kid1;
    if (!Fold(cx, &statements, parser))
        return false;

    if (ParseNode*& catchScope = node->pn_kid2) {
        if (!Fold(cx, &catchScope, parser))
            return false;
    }

    if (ParseNode*& finally = node->pn_kid3) {
        if (!Fold(cx, &finally, parser))
            return false;
    }

    return true;
}

static bool
FoldCatch(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Catch));
    MOZ_ASSERT(node->isArity(PN_BINARY));

    if (ParseNode*& declPattern = node->pn_left) {
        if (!Fold(cx, &declPattern, parser))
            return false;
    }

    if (ParseNode*& statements = node->pn_right) {
        if (!Fold(cx, &statements, parser))
            return false;
    }

    return true;
}

static bool
FoldClass(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Class));
    MOZ_ASSERT(node->isArity(PN_TERNARY));

    if (ParseNode*& classNames = node->pn_kid1) {
        if (!Fold(cx, &classNames, parser))
            return false;
    }

    if (ParseNode*& heritage = node->pn_kid2) {
        if (!Fold(cx, &heritage, parser))
            return false;
    }

    ParseNode*& body = node->pn_kid3;
    return Fold(cx, &body, parser);
}

static bool
FoldElement(JSContext* cx, ParseNode** nodePtr, PerHandlerParser<FullParseHandler>& parser)
{
    ParseNode* node = *nodePtr;

    MOZ_ASSERT(node->isKind(ParseNodeKind::Elem));
    MOZ_ASSERT(node->isArity(PN_BINARY));

    ParseNode*& expr = node->pn_left;
    if (!Fold(cx, &expr, parser))
        return false;

    ParseNode*& key = node->pn_right;
    if (!Fold(cx, &key, parser))
        return false;

    PropertyName* name = nullptr;
    if (key->isKind(ParseNodeKind::String)) {
        JSAtom* atom = key->pn_atom;
        uint32_t index;

        if (atom->isIndex(&index)) {
            // Optimization 1: We have something like expr["100"]. This is
            // equivalent to expr[100] which is faster.
            key->setKind(ParseNodeKind::Number);
            key->setOp(JSOP_DOUBLE);
            key->pn_dval = index;
        } else {
            name = atom->asPropertyName();
        }
    } else if (key->isKind(ParseNodeKind::Number)) {
        double number = key->pn_dval;
        if (number != ToUint32(number)) {
            // Optimization 2: We have something like expr[3.14]. The number
            // isn't an array index, so it converts to a string ("3.14"),
            // enabling optimization 3 below.
            JSAtom* atom = ToAtom<NoGC>(cx, DoubleValue(number));
            if (!atom)
                return false;
            name = atom->asPropertyName();
        }
    }

    // If we don't have a name, we can't optimize to getprop.
    if (!name)
        return true;

    // Optimization 3: We have expr["foo"] where foo is not an index.  Convert
    // to a property access (like expr.foo) that optimizes better downstream.
    ParseNode* dottedAccess = parser.newPropertyAccess(expr, name, node->pn_pos.end);
    if (!dottedAccess)
        return false;
    dottedAccess->setInParens(node->isInParens());
    ReplaceNode(nodePtr, dottedAccess);

    // If we've replaced |expr["prop"]| with |expr.prop|, we can now free the
    // |"prop"| and |expr["prop"]| nodes -- but not the |expr| node that we're
    // now using as a sub-node of |dottedAccess|.  Munge |expr["prop"]| into a
    // node with |"prop"| as its only child, that'll pass AST sanity-checking
    // assertions during freeing, then free it.
    node->setKind(ParseNodeKind::TypeOfExpr);
    node->setArity(PN_UNARY);
    node->pn_kid = key;
    parser.freeTree(node);

    return true;
}

static bool
FoldAdd(JSContext* cx, ParseNode** nodePtr, PerHandlerParser<FullParseHandler>& parser)
{
    ParseNode* node = *nodePtr;

    MOZ_ASSERT(node->isKind(ParseNodeKind::Add));
    MOZ_ASSERT(node->isArity(PN_LIST));
    MOZ_ASSERT(node->pn_count >= 2);

    // Generically fold all operands first.
    if (!FoldList(cx, node, parser))
        return false;

    // Fold leading numeric operands together:
    //
    //   (1 + 2 + x)  becomes  (3 + x)
    //
    // Don't go past the leading operands: additions after a string are
    // string concatenations, not additions: ("1" + 2 + 3 === "123").
    ParseNode* current = node->pn_head;
    ParseNode* next = current->pn_next;
    if (current->isKind(ParseNodeKind::Number)) {
        do {
            if (!next->isKind(ParseNodeKind::Number))
                break;

            current->pn_dval += next->pn_dval;
            current->pn_next = next->pn_next;
            parser.freeTree(next);
            next = current->pn_next;

            MOZ_ASSERT(node->pn_count > 1);
            node->pn_count--;
        } while (next);
    }

    // If any operands remain, attempt string concatenation folding.
    do {
        // If no operands remain, we're done.
        if (!next)
            break;

        // (number + string) is string concatenation *only* at the start of
        // the list: (x + 1 + "2" !== x + "12") when x is a number.
        if (current->isKind(ParseNodeKind::Number) && next->isKind(ParseNodeKind::String)) {
            if (!FoldType(cx, current, ParseNodeKind::String))
                return false;
            next = current->pn_next;
        }

        // The first string forces all subsequent additions to be
        // string concatenations.
        do {
            if (current->isKind(ParseNodeKind::String))
                break;

            current = next;
            next = next->pn_next;
        } while (next);

        // If there's nothing left to fold, we're done.
        if (!next)
            break;

        RootedString combination(cx);
        RootedString tmp(cx);
        do {
            // Create a rope of the current string and all succeeding
            // constants that we can convert to strings, then atomize it
            // and replace them all with that fresh string.
            MOZ_ASSERT(current->isKind(ParseNodeKind::String));

            combination = current->pn_atom;

            do {
                // Try folding the next operand to a string.
                if (!FoldType(cx, next, ParseNodeKind::String))
                    return false;

                // Stop glomming once folding doesn't produce a string.
                if (!next->isKind(ParseNodeKind::String))
                    break;

                // Add this string to the combination and remove the node.
                tmp = next->pn_atom;
                combination = ConcatStrings<CanGC>(cx, combination, tmp);
                if (!combination)
                    return false;

                current->pn_next = next->pn_next;
                parser.freeTree(next);
                next = current->pn_next;

                MOZ_ASSERT(node->pn_count > 1);
                node->pn_count--;
            } while (next);

            // Replace |current|'s string with the entire combination.
            MOZ_ASSERT(current->isKind(ParseNodeKind::String));
            combination = AtomizeString(cx, combination);
            if (!combination)
                return false;
            current->pn_atom = &combination->asAtom();


            // If we're out of nodes, we're done.
            if (!next)
                break;

            current = next;
            next = current->pn_next;

            // If we're out of nodes *after* the non-foldable-to-string
            // node, we're done.
            if (!next)
                break;

            // Otherwise find the next node foldable to a string, and loop.
            do {
                current = next;
                next = current->pn_next;

                if (!FoldType(cx, current, ParseNodeKind::String))
                    return false;
                next = current->pn_next;
            } while (!current->isKind(ParseNodeKind::String) && next);
        } while (next);
    } while (false);

    MOZ_ASSERT(!next, "must have considered all nodes here");
    MOZ_ASSERT(!current->pn_next, "current node must be the last node");

    node->pn_tail = &current->pn_next;
    node->checkListConsistency();

    if (node->pn_count == 1) {
        // We reduced the list to a constant.  Replace the ParseNodeKind::Add node
        // with that constant.
        ReplaceNode(nodePtr, current);

        // Free the old node to aggressively verify nothing uses it.
        node->setKind(ParseNodeKind::True);
        node->setArity(PN_NULLARY);
        node->setOp(JSOP_TRUE);
        parser.freeTree(node);
    }

    return true;
}

static bool
FoldCall(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Call) ||
               node->isKind(ParseNodeKind::SuperCall) ||
               node->isKind(ParseNodeKind::TaggedTemplate));
    MOZ_ASSERT(node->isArity(PN_LIST));

    // Don't fold a parenthesized callable component in an invocation, as this
    // might cause a different |this| value to be used, changing semantics:
    //
    //   var prop = "global";
    //   var obj = { prop: "obj", f: function() { return this.prop; } };
    //   assertEq((true ? obj.f : null)(), "global");
    //   assertEq(obj.f(), "obj");
    //   assertEq((true ? obj.f : null)``, "global");
    //   assertEq(obj.f``, "obj");
    //
    // See bug 537673 and bug 1182373.
    ParseNode** listp = &node->pn_head;
    if ((*listp)->isInParens())
        listp = &(*listp)->pn_next;

    for (; *listp; listp = &(*listp)->pn_next) {
        if (!Fold(cx, listp, parser))
            return false;
    }

    // If the last node in the list was replaced, pn_tail points into the wrong node.
    node->pn_tail = listp;

    node->checkListConsistency();
    return true;
}

static bool
FoldForInOrOf(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::ForIn) ||
               node->isKind(ParseNodeKind::ForOf));
    MOZ_ASSERT(node->isArity(PN_TERNARY));
    MOZ_ASSERT(!node->pn_kid2);

    return Fold(cx, &node->pn_kid1, parser) &&
           Fold(cx, &node->pn_kid3, parser);
}

static bool
FoldForHead(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::ForHead));
    MOZ_ASSERT(node->isArity(PN_TERNARY));

    if (ParseNode*& init = node->pn_kid1) {
        if (!Fold(cx, &init, parser))
            return false;
    }

    if (ParseNode*& test = node->pn_kid2) {
        if (!FoldCondition(cx, &test, parser))
            return false;

        if (test->isKind(ParseNodeKind::True)) {
            parser.freeTree(test);
            test = nullptr;
        }
    }

    if (ParseNode*& update = node->pn_kid3) {
        if (!Fold(cx, &update, parser))
            return false;
    }

    return true;
}

static bool
FoldDottedProperty(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Dot));
    MOZ_ASSERT(node->isArity(PN_NAME));

    // Iterate through a long chain of dotted property accesses to find the
    // most-nested non-dotted property node, then fold that.
    ParseNode** nested = &node->pn_expr;
    while ((*nested)->isKind(ParseNodeKind::Dot)) {
        MOZ_ASSERT((*nested)->isArity(PN_NAME));
        nested = &(*nested)->pn_expr;
    }

    return Fold(cx, nested, parser);
}

static bool
FoldName(JSContext* cx, ParseNode* node, PerHandlerParser<FullParseHandler>& parser)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::Name));
    MOZ_ASSERT(node->isArity(PN_NAME));

    if (!node->pn_expr)
        return true;

    return Fold(cx, &node->pn_expr, parser);
}

bool
Fold(JSContext* cx, ParseNode** pnp, PerHandlerParser<FullParseHandler>& parser)
{
    if (!CheckRecursionLimit(cx))
        return false;

    ParseNode* pn = *pnp;

    switch (pn->getKind()) {
      case ParseNodeKind::EmptyStatement:
      case ParseNodeKind::RegExp:
      case ParseNodeKind::String:
      case ParseNodeKind::True:
      case ParseNodeKind::False:
      case ParseNodeKind::Null:
      case ParseNodeKind::RawUndefined:
      case ParseNodeKind::Elision:
      case ParseNodeKind::Number:
      case ParseNodeKind::Debugger:
      case ParseNodeKind::Break:
      case ParseNodeKind::Continue:
      case ParseNodeKind::TemplateString:
      case ParseNodeKind::Generator:
      case ParseNodeKind::ExportBatchSpec:
      case ParseNodeKind::ObjectPropertyName:
      case ParseNodeKind::PosHolder:
        MOZ_ASSERT(pn->isArity(PN_NULLARY));
        return true;

      case ParseNodeKind::SuperBase:
      case ParseNodeKind::TypeOfName:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        MOZ_ASSERT(pn->pn_kid->isKind(ParseNodeKind::Name));
        MOZ_ASSERT(!pn->pn_kid->expr());
        return true;

      case ParseNodeKind::TypeOfExpr:
        return FoldTypeOfExpr(cx, pn, parser);

      case ParseNodeKind::DeleteName: {
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        MOZ_ASSERT(pn->pn_kid->isKind(ParseNodeKind::Name));
        return true;
      }

      case ParseNodeKind::DeleteExpr:
        return FoldDeleteExpr(cx, pn, parser);

      case ParseNodeKind::DeleteElem:
        return FoldDeleteElement(cx, pn, parser);

      case ParseNodeKind::DeleteProp:
        return FoldDeleteProperty(cx, pn, parser);

      case ParseNodeKind::Conditional:
        return FoldConditional(cx, pnp, parser);

      case ParseNodeKind::If:
        return FoldIf(cx, pnp, parser);

      case ParseNodeKind::Not:
        return FoldNot(cx, pn, parser);

      case ParseNodeKind::BitNot:
      case ParseNodeKind::Pos:
      case ParseNodeKind::Neg:
        return FoldUnaryArithmetic(cx, pn, parser);

      case ParseNodeKind::PreIncrement:
      case ParseNodeKind::PostIncrement:
      case ParseNodeKind::PreDecrement:
      case ParseNodeKind::PostDecrement:
        return FoldIncrementDecrement(cx, pn, parser);

      case ParseNodeKind::ExpressionStatement:
      case ParseNodeKind::Throw:
      case ParseNodeKind::MutateProto:
      case ParseNodeKind::ComputedName:
      case ParseNodeKind::Spread:
      case ParseNodeKind::Export:
      case ParseNodeKind::Void:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        return Fold(cx, &pn->pn_kid, parser);

      case ParseNodeKind::ExportDefault:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        return Fold(cx, &pn->pn_left, parser);

      case ParseNodeKind::This:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        if (ParseNode*& expr = pn->pn_kid)
            return Fold(cx, &expr, parser);
        return true;

      case ParseNodeKind::Pipeline:
        return true;

      case ParseNodeKind::And:
      case ParseNodeKind::Or:
        return FoldAndOr(cx, pnp, parser);

      case ParseNodeKind::Function:
        return FoldFunction(cx, pn, parser);

      case ParseNodeKind::Module:
        return FoldModule(cx, pn, parser);

      case ParseNodeKind::Sub:
      case ParseNodeKind::Star:
      case ParseNodeKind::Lsh:
      case ParseNodeKind::Rsh:
      case ParseNodeKind::Ursh:
      case ParseNodeKind::Div:
      case ParseNodeKind::Mod:
        return FoldBinaryArithmetic(cx, pn, parser);

      case ParseNodeKind::Pow:
        return FoldExponentiation(cx, pn, parser);

      // Various list nodes not requiring care to minimally fold.  Some of
      // these could be further folded/optimized, but we don't make the effort.
      case ParseNodeKind::BitOr:
      case ParseNodeKind::BitXor:
      case ParseNodeKind::BitAnd:
      case ParseNodeKind::StrictEq:
      case ParseNodeKind::Eq:
      case ParseNodeKind::StrictNe:
      case ParseNodeKind::Ne:
      case ParseNodeKind::Lt:
      case ParseNodeKind::Le:
      case ParseNodeKind::Gt:
      case ParseNodeKind::Ge:
      case ParseNodeKind::InstanceOf:
      case ParseNodeKind::In:
      case ParseNodeKind::Comma:
      case ParseNodeKind::New:
      case ParseNodeKind::Array:
      case ParseNodeKind::Object:
      case ParseNodeKind::StatementList:
      case ParseNodeKind::ClassMethodList:
      case ParseNodeKind::TemplateStringList:
      case ParseNodeKind::Var:
      case ParseNodeKind::Const:
      case ParseNodeKind::Let:
      case ParseNodeKind::ParamsBody:
      case ParseNodeKind::CallSiteObj:
      case ParseNodeKind::ExportSpecList:
      case ParseNodeKind::ImportSpecList:
        return FoldList(cx, pn, parser);

      case ParseNodeKind::InitialYield:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        MOZ_ASSERT(pn->pn_kid->isKind(ParseNodeKind::Assign) &&
                   pn->pn_kid->pn_left->isKind(ParseNodeKind::Name) &&
                   pn->pn_kid->pn_right->isKind(ParseNodeKind::Generator));
        return true;

      case ParseNodeKind::YieldStar:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        return Fold(cx, &pn->pn_kid, parser);

      case ParseNodeKind::Yield:
      case ParseNodeKind::Await:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        if (!pn->pn_kid)
            return true;
        return Fold(cx, &pn->pn_kid, parser);

      case ParseNodeKind::Return:
        return FoldReturn(cx, pn, parser);

      case ParseNodeKind::Try:
        return FoldTry(cx, pn, parser);

      case ParseNodeKind::Catch:
        return FoldCatch(cx, pn, parser);

      case ParseNodeKind::Class:
        return FoldClass(cx, pn, parser);

      case ParseNodeKind::Elem:
        return FoldElement(cx, pnp, parser);

      case ParseNodeKind::Add:
        return FoldAdd(cx, pnp, parser);

      case ParseNodeKind::Call:
      case ParseNodeKind::SuperCall:
      case ParseNodeKind::TaggedTemplate:
        return FoldCall(cx, pn, parser);

      case ParseNodeKind::Switch:
      case ParseNodeKind::Colon:
      case ParseNodeKind::Assign:
      case ParseNodeKind::AddAssign:
      case ParseNodeKind::SubAssign:
      case ParseNodeKind::BitOrAssign:
      case ParseNodeKind::BitAndAssign:
      case ParseNodeKind::BitXorAssign:
      case ParseNodeKind::LshAssign:
      case ParseNodeKind::RshAssign:
      case ParseNodeKind::UrshAssign:
      case ParseNodeKind::DivAssign:
      case ParseNodeKind::ModAssign:
      case ParseNodeKind::MulAssign:
      case ParseNodeKind::PowAssign:
      case ParseNodeKind::Import:
      case ParseNodeKind::ExportFrom:
      case ParseNodeKind::Shorthand:
      case ParseNodeKind::For:
      case ParseNodeKind::ClassMethod:
      case ParseNodeKind::ImportSpec:
      case ParseNodeKind::ExportSpec:
      case ParseNodeKind::SetThis:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        return Fold(cx, &pn->pn_left, parser) &&
               Fold(cx, &pn->pn_right, parser);

      case ParseNodeKind::NewTarget:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        MOZ_ASSERT(pn->pn_left->isKind(ParseNodeKind::PosHolder));
        MOZ_ASSERT(pn->pn_right->isKind(ParseNodeKind::PosHolder));
        return true;

      case ParseNodeKind::ClassNames:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        if (ParseNode*& outerBinding = pn->pn_left) {
            if (!Fold(cx, &outerBinding, parser))
                return false;
        }
        return Fold(cx, &pn->pn_right, parser);

      case ParseNodeKind::DoWhile:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        return Fold(cx, &pn->pn_left, parser) &&
               FoldCondition(cx, &pn->pn_right, parser);

      case ParseNodeKind::While:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        return FoldCondition(cx, &pn->pn_left, parser) &&
               Fold(cx, &pn->pn_right, parser);

      case ParseNodeKind::Case: {
        MOZ_ASSERT(pn->isArity(PN_BINARY));

        // pn_left is null for DefaultClauses.
        if (pn->pn_left) {
            if (!Fold(cx, &pn->pn_left, parser))
                return false;
        }
        return Fold(cx, &pn->pn_right, parser);
      }

      case ParseNodeKind::With:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        return Fold(cx, &pn->pn_left, parser) &&
               Fold(cx, &pn->pn_right, parser);

      case ParseNodeKind::ForIn:
      case ParseNodeKind::ForOf:
        return FoldForInOrOf(cx, pn, parser);

      case ParseNodeKind::ForHead:
        return FoldForHead(cx, pn, parser);

      case ParseNodeKind::Label:
        MOZ_ASSERT(pn->isArity(PN_NAME));
        return Fold(cx, &pn->pn_expr, parser);

      case ParseNodeKind::Dot:
        return FoldDottedProperty(cx, pn, parser);

      case ParseNodeKind::LexicalScope:
        MOZ_ASSERT(pn->isArity(PN_SCOPE));
        if (!pn->scopeBody())
            return true;
        return Fold(cx, &pn->pn_u.scope.body, parser);

      case ParseNodeKind::Name:
        return FoldName(cx, pn, parser);

      case ParseNodeKind::Limit: // invalid sentinel value
        MOZ_CRASH("invalid node kind");
    }

    MOZ_CRASH("shouldn't reach here");
    return false;
}

bool
frontend::FoldConstants(JSContext* cx, ParseNode** pnp, PerHandlerParser<FullParseHandler>* parser)
{
    // Don't constant-fold inside "use asm" code, as this could create a parse
    // tree that doesn't type-check as asm.js.
    if (parser->pc->useAsmOrInsideUseAsm())
        return true;

    AutoTraceLog traceLog(TraceLoggerForCurrentThread(cx), TraceLogger_BytecodeFoldConstants);
    return Fold(cx, pnp, *parser);
}
