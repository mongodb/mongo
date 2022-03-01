/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/FoldConstants.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"  // mozilla::Maybe
#include "mozilla/Range.h"

#include "jslibmath.h"
#include "jsmath.h"
#include "jsnum.h"

#include "frontend/ParseNode.h"
#include "frontend/ParseNodeVisitor.h"
#include "frontend/Parser.h"
#include "frontend/ParserAtom.h"  // ParserAtomsTable, TaggedParserAtomIndex
#include "js/Conversions.h"
#include "js/friend/StackLimits.h"  // js::AutoCheckRecursionLimit
#include "js/Vector.h"
#include "util/StringBuffer.h"  // StringBuffer
#include "vm/StringType.h"

using namespace js;
using namespace js::frontend;

using JS::GenericNaN;
using JS::ToInt32;
using JS::ToUint32;
using mozilla::IsNaN;
using mozilla::IsNegative;
using mozilla::NegativeInfinity;
using mozilla::PositiveInfinity;

struct FoldInfo {
  JSContext* cx;
  ParserAtomsTable& parserAtoms;
  FullParseHandler* handler;
};

// Don't use ReplaceNode directly, because we want the constant folder to keep
// the attributes isInParens and isDirectRHSAnonFunction of the old node being
// replaced.
[[nodiscard]] inline bool TryReplaceNode(ParseNode** pnp, ParseNode* pn) {
  // convenience check: can call TryReplaceNode(pnp, alloc_parsenode())
  // directly, without having to worry about alloc returning null.
  if (!pn) {
    return false;
  }
  pn->setInParens((*pnp)->isInParens());
  pn->setDirectRHSAnonFunction((*pnp)->isDirectRHSAnonFunction());
  ReplaceNode(pnp, pn);
  return true;
}

static bool ContainsHoistedDeclaration(JSContext* cx, ParseNode* node,
                                       bool* result);

static bool ListContainsHoistedDeclaration(JSContext* cx, ListNode* list,
                                           bool* result) {
  for (ParseNode* node : list->contents()) {
    if (!ContainsHoistedDeclaration(cx, node, result)) {
      return false;
    }
    if (*result) {
      return true;
    }
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
static bool ContainsHoistedDeclaration(JSContext* cx, ParseNode* node,
                                       bool* result) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

restart:

  // With a better-typed AST, we would have distinct parse node classes for
  // expressions and for statements and would characterize expressions with
  // ExpressionKind and statements with StatementKind.  Perhaps someday.  In
  // the meantime we must characterize every ParseNodeKind, even the
  // expression/sub-expression ones that, if we handle all statement kinds
  // correctly, we'll never see.
  switch (node->getKind()) {
    // Base case.
    case ParseNodeKind::VarStmt:
      *result = true;
      return true;

    // Non-global lexical declarations are block-scoped (ergo not hoistable).
    case ParseNodeKind::LetDecl:
    case ParseNodeKind::ConstDecl:
      MOZ_ASSERT(node->is<ListNode>());
      *result = false;
      return true;

    // Similarly to the lexical declarations above, classes cannot add hoisted
    // declarations
    case ParseNodeKind::ClassDecl:
      MOZ_ASSERT(node->is<ClassNode>());
      *result = false;
      return true;

    // Function declarations *can* be hoisted declarations.  But in the
    // magical world of the rewritten frontend, the declaration necessitated
    // by a nested function statement, not at body level, doesn't require
    // that we preserve an unreachable function declaration node against
    // dead-code removal.
    case ParseNodeKind::Function:
      *result = false;
      return true;

    case ParseNodeKind::Module:
      *result = false;
      return true;

    // Statements with no sub-components at all.
    case ParseNodeKind::EmptyStmt:
      MOZ_ASSERT(node->is<NullaryNode>());
      *result = false;
      return true;

    case ParseNodeKind::DebuggerStmt:
      MOZ_ASSERT(node->is<DebuggerStatement>());
      *result = false;
      return true;

    // Statements containing only an expression have no declarations.
    case ParseNodeKind::ExpressionStmt:
    case ParseNodeKind::ThrowStmt:
    case ParseNodeKind::ReturnStmt:
      MOZ_ASSERT(node->is<UnaryNode>());
      *result = false;
      return true;

    // These two aren't statements in the spec, but we sometimes insert them
    // in statement lists anyway.
    case ParseNodeKind::InitialYield:
    case ParseNodeKind::YieldStarExpr:
    case ParseNodeKind::YieldExpr:
      MOZ_ASSERT(node->is<UnaryNode>());
      *result = false;
      return true;

    // Other statements with no sub-statement components.
    case ParseNodeKind::BreakStmt:
    case ParseNodeKind::ContinueStmt:
    case ParseNodeKind::ImportDecl:
    case ParseNodeKind::ImportSpecList:
    case ParseNodeKind::ImportSpec:
    case ParseNodeKind::ImportNamespaceSpec:
    case ParseNodeKind::ExportFromStmt:
    case ParseNodeKind::ExportDefaultStmt:
    case ParseNodeKind::ExportSpecList:
    case ParseNodeKind::ExportSpec:
    case ParseNodeKind::ExportNamespaceSpec:
    case ParseNodeKind::ExportStmt:
    case ParseNodeKind::ExportBatchSpecStmt:
    case ParseNodeKind::CallImportExpr:
      *result = false;
      return true;

    // Statements possibly containing hoistable declarations only in the left
    // half, in ParseNode terms -- the loop body in AST terms.
    case ParseNodeKind::DoWhileStmt:
      return ContainsHoistedDeclaration(cx, node->as<BinaryNode>().left(),
                                        result);

    // Statements possibly containing hoistable declarations only in the
    // right half, in ParseNode terms -- the loop body or nested statement
    // (usually a block statement), in AST terms.
    case ParseNodeKind::WhileStmt:
    case ParseNodeKind::WithStmt:
      return ContainsHoistedDeclaration(cx, node->as<BinaryNode>().right(),
                                        result);

    case ParseNodeKind::LabelStmt:
      return ContainsHoistedDeclaration(
          cx, node->as<LabeledStatement>().statement(), result);

    // Statements with more complicated structures.

    // if-statement nodes may have hoisted declarations in their consequent
    // and alternative components.
    case ParseNodeKind::IfStmt: {
      TernaryNode* ifNode = &node->as<TernaryNode>();
      ParseNode* consequent = ifNode->kid2();
      if (!ContainsHoistedDeclaration(cx, consequent, result)) {
        return false;
      }
      if (*result) {
        return true;
      }

      if ((node = ifNode->kid3())) {
        goto restart;
      }

      *result = false;
      return true;
    }

    // try-statements have statements to execute, and one or both of a
    // catch-list and a finally-block.
    case ParseNodeKind::TryStmt: {
      TernaryNode* tryNode = &node->as<TernaryNode>();

      MOZ_ASSERT(tryNode->kid2() || tryNode->kid3(),
                 "must have either catch or finally");

      ParseNode* tryBlock = tryNode->kid1();
      if (!ContainsHoistedDeclaration(cx, tryBlock, result)) {
        return false;
      }
      if (*result) {
        return true;
      }

      if (ParseNode* catchScope = tryNode->kid2()) {
        BinaryNode* catchNode =
            &catchScope->as<LexicalScopeNode>().scopeBody()->as<BinaryNode>();
        MOZ_ASSERT(catchNode->isKind(ParseNodeKind::Catch));

        ParseNode* catchStatements = catchNode->right();
        if (!ContainsHoistedDeclaration(cx, catchStatements, result)) {
          return false;
        }
        if (*result) {
          return true;
        }
      }

      if (ParseNode* finallyBlock = tryNode->kid3()) {
        return ContainsHoistedDeclaration(cx, finallyBlock, result);
      }

      *result = false;
      return true;
    }

    // A switch node's left half is an expression; only its right half (a
    // list of cases/defaults, or a block node) could contain hoisted
    // declarations.
    case ParseNodeKind::SwitchStmt: {
      SwitchStatement* switchNode = &node->as<SwitchStatement>();
      return ContainsHoistedDeclaration(cx, &switchNode->lexicalForCaseList(),
                                        result);
    }

    case ParseNodeKind::Case: {
      CaseClause* caseClause = &node->as<CaseClause>();
      return ContainsHoistedDeclaration(cx, caseClause->statementList(),
                                        result);
    }

    case ParseNodeKind::ForStmt: {
      ForNode* forNode = &node->as<ForNode>();
      TernaryNode* loopHead = forNode->head();
      MOZ_ASSERT(loopHead->isKind(ParseNodeKind::ForHead) ||
                 loopHead->isKind(ParseNodeKind::ForIn) ||
                 loopHead->isKind(ParseNodeKind::ForOf));

      if (loopHead->isKind(ParseNodeKind::ForHead)) {
        // for (init?; cond?; update?), with only init possibly containing
        // a hoisted declaration.  (Note: a lexical-declaration |init| is
        // (at present) hoisted in SpiderMonkey parlance -- but such
        // hoisting doesn't extend outside of this statement, so it is not
        // hoisting in the sense meant by ContainsHoistedDeclaration.)
        ParseNode* init = loopHead->kid1();
        if (init && init->isKind(ParseNodeKind::VarStmt)) {
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
        ParseNode* decl = loopHead->kid1();
        if (decl && decl->isKind(ParseNodeKind::VarStmt)) {
          *result = true;
          return true;
        }
      }

      ParseNode* loopBody = forNode->body();
      return ContainsHoistedDeclaration(cx, loopBody, result);
    }

    case ParseNodeKind::LexicalScope: {
      LexicalScopeNode* scope = &node->as<LexicalScopeNode>();
      ParseNode* expr = scope->scopeBody();

      if (expr->isKind(ParseNodeKind::ForStmt) || expr->is<FunctionNode>()) {
        return ContainsHoistedDeclaration(cx, expr, result);
      }

      MOZ_ASSERT(expr->isKind(ParseNodeKind::StatementList));
      return ListContainsHoistedDeclaration(
          cx, &scope->scopeBody()->as<ListNode>(), result);
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
    case ParseNodeKind::PropertyDefinition:
    case ParseNodeKind::Shorthand:
    case ParseNodeKind::ConditionalExpr:
    case ParseNodeKind::TypeOfNameExpr:
    case ParseNodeKind::TypeOfExpr:
    case ParseNodeKind::AwaitExpr:
    case ParseNodeKind::VoidExpr:
    case ParseNodeKind::NotExpr:
    case ParseNodeKind::BitNotExpr:
    case ParseNodeKind::DeleteNameExpr:
    case ParseNodeKind::DeletePropExpr:
    case ParseNodeKind::DeleteElemExpr:
    case ParseNodeKind::DeleteOptionalChainExpr:
    case ParseNodeKind::DeleteExpr:
    case ParseNodeKind::PosExpr:
    case ParseNodeKind::NegExpr:
    case ParseNodeKind::PreIncrementExpr:
    case ParseNodeKind::PostIncrementExpr:
    case ParseNodeKind::PreDecrementExpr:
    case ParseNodeKind::PostDecrementExpr:
    case ParseNodeKind::CoalesceExpr:
    case ParseNodeKind::OrExpr:
    case ParseNodeKind::AndExpr:
    case ParseNodeKind::BitOrExpr:
    case ParseNodeKind::BitXorExpr:
    case ParseNodeKind::BitAndExpr:
    case ParseNodeKind::StrictEqExpr:
    case ParseNodeKind::EqExpr:
    case ParseNodeKind::StrictNeExpr:
    case ParseNodeKind::NeExpr:
    case ParseNodeKind::LtExpr:
    case ParseNodeKind::LeExpr:
    case ParseNodeKind::GtExpr:
    case ParseNodeKind::GeExpr:
    case ParseNodeKind::InstanceOfExpr:
    case ParseNodeKind::InExpr:
    case ParseNodeKind::PrivateInExpr:
    case ParseNodeKind::LshExpr:
    case ParseNodeKind::RshExpr:
    case ParseNodeKind::UrshExpr:
    case ParseNodeKind::AddExpr:
    case ParseNodeKind::SubExpr:
    case ParseNodeKind::MulExpr:
    case ParseNodeKind::DivExpr:
    case ParseNodeKind::ModExpr:
    case ParseNodeKind::PowExpr:
    case ParseNodeKind::InitExpr:
    case ParseNodeKind::AssignExpr:
    case ParseNodeKind::AddAssignExpr:
    case ParseNodeKind::SubAssignExpr:
    case ParseNodeKind::CoalesceAssignExpr:
    case ParseNodeKind::OrAssignExpr:
    case ParseNodeKind::AndAssignExpr:
    case ParseNodeKind::BitOrAssignExpr:
    case ParseNodeKind::BitXorAssignExpr:
    case ParseNodeKind::BitAndAssignExpr:
    case ParseNodeKind::LshAssignExpr:
    case ParseNodeKind::RshAssignExpr:
    case ParseNodeKind::UrshAssignExpr:
    case ParseNodeKind::MulAssignExpr:
    case ParseNodeKind::DivAssignExpr:
    case ParseNodeKind::ModAssignExpr:
    case ParseNodeKind::PowAssignExpr:
    case ParseNodeKind::CommaExpr:
    case ParseNodeKind::ArrayExpr:
    case ParseNodeKind::ObjectExpr:
    case ParseNodeKind::PropertyNameExpr:
    case ParseNodeKind::DotExpr:
    case ParseNodeKind::ElemExpr:
    case ParseNodeKind::Arguments:
    case ParseNodeKind::CallExpr:
    case ParseNodeKind::PrivateMemberExpr:
    case ParseNodeKind::OptionalChain:
    case ParseNodeKind::OptionalDotExpr:
    case ParseNodeKind::OptionalElemExpr:
    case ParseNodeKind::OptionalCallExpr:
    case ParseNodeKind::OptionalPrivateMemberExpr:
    case ParseNodeKind::Name:
    case ParseNodeKind::PrivateName:
    case ParseNodeKind::TemplateStringExpr:
    case ParseNodeKind::TemplateStringListExpr:
    case ParseNodeKind::TaggedTemplateExpr:
    case ParseNodeKind::CallSiteObj:
    case ParseNodeKind::StringExpr:
    case ParseNodeKind::RegExpExpr:
    case ParseNodeKind::TrueExpr:
    case ParseNodeKind::FalseExpr:
    case ParseNodeKind::NullExpr:
    case ParseNodeKind::RawUndefinedExpr:
    case ParseNodeKind::ThisExpr:
    case ParseNodeKind::Elision:
    case ParseNodeKind::NumberExpr:
    case ParseNodeKind::BigIntExpr:
    case ParseNodeKind::NewExpr:
    case ParseNodeKind::Generator:
    case ParseNodeKind::ParamsBody:
    case ParseNodeKind::Catch:
    case ParseNodeKind::ForIn:
    case ParseNodeKind::ForOf:
    case ParseNodeKind::ForHead:
    case ParseNodeKind::DefaultConstructor:
    case ParseNodeKind::ClassBodyScope:
    case ParseNodeKind::ClassMethod:
    case ParseNodeKind::ClassField:
    case ParseNodeKind::StaticClassBlock:
    case ParseNodeKind::ClassMemberList:
    case ParseNodeKind::ClassNames:
    case ParseNodeKind::NewTargetExpr:
    case ParseNodeKind::ImportMetaExpr:
    case ParseNodeKind::PosHolder:
    case ParseNodeKind::SuperCallExpr:
    case ParseNodeKind::SuperBase:
    case ParseNodeKind::SetThis:
      MOZ_CRASH(
          "ContainsHoistedDeclaration should have indicated false on "
          "some parent node without recurring to test this node");

    case ParseNodeKind::LastUnused:
    case ParseNodeKind::Limit:
      MOZ_CRASH("unexpected sentinel ParseNodeKind in node");
  }

  MOZ_CRASH("invalid node kind");
}

/*
 * Fold from one constant type to another.
 * XXX handles only strings and numbers for now
 */
static bool FoldType(FoldInfo info, ParseNode** pnp, ParseNodeKind kind) {
  const ParseNode* pn = *pnp;
  if (!pn->isKind(kind)) {
    switch (kind) {
      case ParseNodeKind::NumberExpr:
        if (pn->isKind(ParseNodeKind::StringExpr)) {
          double d;
          auto atom = pn->as<NameNode>().atom();
          if (!info.parserAtoms.toNumber(info.cx, atom, &d)) {
            return false;
          }
          if (!TryReplaceNode(
                  pnp, info.handler->newNumber(d, NoDecimal, pn->pn_pos))) {
            return false;
          }
        }
        break;

      case ParseNodeKind::StringExpr:
        if (pn->isKind(ParseNodeKind::NumberExpr)) {
          TaggedParserAtomIndex atom =
              pn->as<NumericLiteral>().toAtom(info.cx, info.parserAtoms);
          if (!atom) {
            return false;
          }
          if (!TryReplaceNode(
                  pnp, info.handler->newStringLiteral(atom, pn->pn_pos))) {
            return false;
          }
        }
        break;

      default:
        MOZ_CRASH("Invalid type in constant folding FoldType");
    }
  }
  return true;
}

static bool IsEffectless(ParseNode* node) {
  return node->isKind(ParseNodeKind::TrueExpr) ||
         node->isKind(ParseNodeKind::FalseExpr) ||
         node->isKind(ParseNodeKind::StringExpr) ||
         node->isKind(ParseNodeKind::TemplateStringExpr) ||
         node->isKind(ParseNodeKind::NumberExpr) ||
         node->isKind(ParseNodeKind::BigIntExpr) ||
         node->isKind(ParseNodeKind::NullExpr) ||
         node->isKind(ParseNodeKind::RawUndefinedExpr) ||
         node->isKind(ParseNodeKind::Function);
}

enum Truthiness { Truthy, Falsy, Unknown };

static Truthiness Boolish(ParseNode* pn) {
  switch (pn->getKind()) {
    case ParseNodeKind::NumberExpr:
      return (pn->as<NumericLiteral>().value() != 0 &&
              !IsNaN(pn->as<NumericLiteral>().value()))
                 ? Truthy
                 : Falsy;

    case ParseNodeKind::BigIntExpr:
      return (pn->as<BigIntLiteral>().isZero()) ? Falsy : Truthy;

    case ParseNodeKind::StringExpr:
    case ParseNodeKind::TemplateStringExpr:
      return (pn->as<NameNode>().atom() ==
              TaggedParserAtomIndex::WellKnown::empty())
                 ? Falsy
                 : Truthy;

    case ParseNodeKind::TrueExpr:
    case ParseNodeKind::Function:
      return Truthy;

    case ParseNodeKind::FalseExpr:
    case ParseNodeKind::NullExpr:
    case ParseNodeKind::RawUndefinedExpr:
      return Falsy;

    case ParseNodeKind::VoidExpr: {
      // |void <foo>| evaluates to |undefined| which isn't truthy.  But the
      // sense of this method requires that the expression be literally
      // replaceable with true/false: not the case if the nested expression
      // is effectful, might throw, &c.  Walk past the |void| (and nested
      // |void| expressions, for good measure) and check that the nested
      // expression doesn't break this requirement before indicating falsity.
      do {
        pn = pn->as<UnaryNode>().kid();
      } while (pn->isKind(ParseNodeKind::VoidExpr));

      return IsEffectless(pn) ? Falsy : Unknown;
    }

    default:
      return Unknown;
  }
}

static bool SimplifyCondition(FoldInfo info, ParseNode** nodePtr) {
  // Conditions fold like any other expression, but then they sometimes can be
  // further folded to constants. *nodePtr should already have been
  // constant-folded.

  ParseNode* node = *nodePtr;
  if (Truthiness t = Boolish(node); t != Unknown) {
    // We can turn function nodes into constant nodes here, but mutating
    // function nodes is tricky --- in particular, mutating a function node
    // that appears on a method list corrupts the method list. However,
    // methods are M's in statements of the form 'this.foo = M;', which we
    // never fold, so we're okay.
    if (!TryReplaceNode(nodePtr, info.handler->newBooleanLiteral(
                                     t == Truthy, node->pn_pos))) {
      return false;
    }
  }

  return true;
}

static bool FoldTypeOfExpr(FoldInfo info, ParseNode** nodePtr) {
  UnaryNode* node = &(*nodePtr)->as<UnaryNode>();
  MOZ_ASSERT(node->isKind(ParseNodeKind::TypeOfExpr));
  ParseNode* expr = node->kid();

  // Constant-fold the entire |typeof| if given a constant with known type.
  TaggedParserAtomIndex result;
  if (expr->isKind(ParseNodeKind::StringExpr) ||
      expr->isKind(ParseNodeKind::TemplateStringExpr)) {
    result = TaggedParserAtomIndex::WellKnown::string();
  } else if (expr->isKind(ParseNodeKind::NumberExpr)) {
    result = TaggedParserAtomIndex::WellKnown::number();
  } else if (expr->isKind(ParseNodeKind::BigIntExpr)) {
    result = TaggedParserAtomIndex::WellKnown::bigint();
  } else if (expr->isKind(ParseNodeKind::NullExpr)) {
    result = TaggedParserAtomIndex::WellKnown::object();
  } else if (expr->isKind(ParseNodeKind::TrueExpr) ||
             expr->isKind(ParseNodeKind::FalseExpr)) {
    result = TaggedParserAtomIndex::WellKnown::boolean();
  } else if (expr->is<FunctionNode>()) {
    result = TaggedParserAtomIndex::WellKnown::function();
  }

  if (result) {
    if (!TryReplaceNode(nodePtr,
                        info.handler->newStringLiteral(result, node->pn_pos))) {
      return false;
    }
  }

  return true;
}

static bool FoldDeleteExpr(FoldInfo info, ParseNode** nodePtr) {
  UnaryNode* node = &(*nodePtr)->as<UnaryNode>();

  MOZ_ASSERT(node->isKind(ParseNodeKind::DeleteExpr));
  ParseNode* expr = node->kid();

  // Expression deletion evaluates the expression, then evaluates to true.
  // For effectless expressions, eliminate the expression evaluation.
  if (IsEffectless(expr)) {
    if (!TryReplaceNode(nodePtr,
                        info.handler->newBooleanLiteral(true, node->pn_pos))) {
      return false;
    }
  }

  return true;
}

static bool FoldDeleteElement(FoldInfo info, ParseNode** nodePtr) {
  UnaryNode* node = &(*nodePtr)->as<UnaryNode>();
  MOZ_ASSERT(node->isKind(ParseNodeKind::DeleteElemExpr));
  ParseNode* expr = node->kid();

  // If we're deleting an element, but constant-folding converted our
  // element reference into a dotted property access, we must *also*
  // morph the node's kind.
  //
  // In principle this also applies to |super["foo"] -> super.foo|,
  // but we don't constant-fold |super["foo"]| yet.
  MOZ_ASSERT(expr->isKind(ParseNodeKind::ElemExpr) ||
             expr->isKind(ParseNodeKind::DotExpr));
  if (expr->isKind(ParseNodeKind::DotExpr)) {
    // newDelete will detect and use DeletePropExpr
    if (!TryReplaceNode(nodePtr,
                        info.handler->newDelete(node->pn_pos.begin, expr))) {
      return false;
    }
    MOZ_ASSERT((*nodePtr)->getKind() == ParseNodeKind::DeletePropExpr);
  }

  return true;
}

static bool FoldNot(FoldInfo info, ParseNode** nodePtr) {
  UnaryNode* node = &(*nodePtr)->as<UnaryNode>();
  MOZ_ASSERT(node->isKind(ParseNodeKind::NotExpr));

  if (!SimplifyCondition(info, node->unsafeKidReference())) {
    return false;
  }

  ParseNode* expr = node->kid();

  if (expr->isKind(ParseNodeKind::TrueExpr) ||
      expr->isKind(ParseNodeKind::FalseExpr)) {
    bool newval = !expr->isKind(ParseNodeKind::TrueExpr);

    if (!TryReplaceNode(
            nodePtr, info.handler->newBooleanLiteral(newval, node->pn_pos))) {
      return false;
    }
  }

  return true;
}

static bool FoldUnaryArithmetic(FoldInfo info, ParseNode** nodePtr) {
  UnaryNode* node = &(*nodePtr)->as<UnaryNode>();
  MOZ_ASSERT(node->isKind(ParseNodeKind::BitNotExpr) ||
                 node->isKind(ParseNodeKind::PosExpr) ||
                 node->isKind(ParseNodeKind::NegExpr),
             "need a different method for this node kind");

  ParseNode* expr = node->kid();

  if (expr->isKind(ParseNodeKind::NumberExpr) ||
      expr->isKind(ParseNodeKind::TrueExpr) ||
      expr->isKind(ParseNodeKind::FalseExpr)) {
    double d = expr->isKind(ParseNodeKind::NumberExpr)
                   ? expr->as<NumericLiteral>().value()
                   : double(expr->isKind(ParseNodeKind::TrueExpr));

    if (node->isKind(ParseNodeKind::BitNotExpr)) {
      d = ~ToInt32(d);
    } else if (node->isKind(ParseNodeKind::NegExpr)) {
      d = -d;
    } else {
      MOZ_ASSERT(node->isKind(ParseNodeKind::PosExpr));  // nothing to do
    }

    if (!TryReplaceNode(nodePtr,
                        info.handler->newNumber(d, NoDecimal, node->pn_pos))) {
      return false;
    }
  }

  return true;
}

static bool FoldAndOrCoalesce(FoldInfo info, ParseNode** nodePtr) {
  ListNode* node = &(*nodePtr)->as<ListNode>();

  MOZ_ASSERT(node->isKind(ParseNodeKind::AndExpr) ||
             node->isKind(ParseNodeKind::CoalesceExpr) ||
             node->isKind(ParseNodeKind::OrExpr));

  bool isOrNode = node->isKind(ParseNodeKind::OrExpr);
  bool isAndNode = node->isKind(ParseNodeKind::AndExpr);
  bool isCoalesceNode = node->isKind(ParseNodeKind::CoalesceExpr);
  ParseNode** elem = node->unsafeHeadReference();
  do {
    Truthiness t = Boolish(*elem);

    // If we don't know the constant-folded node's truthiness, we can't
    // reduce this node with its surroundings.  Continue folding any
    // remaining nodes.
    if (t == Unknown) {
      elem = &(*elem)->pn_next;
      continue;
    }

    bool isTruthyCoalesceNode =
        isCoalesceNode && !((*elem)->isKind(ParseNodeKind::NullExpr) ||
                            (*elem)->isKind(ParseNodeKind::VoidExpr) ||
                            (*elem)->isKind(ParseNodeKind::RawUndefinedExpr));
    bool canShortCircuit = (isOrNode && t == Truthy) ||
                           (isAndNode && t == Falsy) || isTruthyCoalesceNode;

    // If the constant-folded node's truthiness will terminate the
    // condition -- `a || true || expr` or `b && false && expr` or
    // `false ?? c ?? expr` -- then trailing nodes will never be
    // evaluated.  Truncate the list after the known-truthiness node,
    // as it's the overall result.
    if (canShortCircuit) {
      for (ParseNode* next = (*elem)->pn_next; next; next = next->pn_next) {
        node->unsafeDecrementCount();
      }

      // Terminate the original and/or list at the known-truthiness
      // node.
      (*elem)->pn_next = nullptr;
      elem = &(*elem)->pn_next;
      break;
    }

    // We've encountered a vacuous node that'll never short-circuit
    // evaluation.
    if ((*elem)->pn_next) {
      // This node is never the overall result when there are
      // subsequent nodes.  Remove it.
      ParseNode* elt = *elem;
      *elem = elt->pn_next;
      node->unsafeDecrementCount();
    } else {
      // Otherwise this node is the result of the overall expression,
      // so leave it alone.  And we're done.
      elem = &(*elem)->pn_next;
      break;
    }
  } while (*elem);

  node->unsafeReplaceTail(elem);

  // If we removed nodes, we may have to replace a one-element list with
  // its element.
  if (node->count() == 1) {
    ParseNode* first = node->head();
    if (!TryReplaceNode(nodePtr, first)) {
      ;
      return false;
    }
  }

  return true;
}

static bool Fold(FoldInfo info, ParseNode** pnp);

static bool FoldConditional(FoldInfo info, ParseNode** nodePtr) {
  ParseNode** nextNode = nodePtr;

  do {
    // |nextNode| on entry points to the C?T:F expression to be folded.
    // Reset it to exit the loop in the common case where F isn't another
    // ?: expression.
    nodePtr = nextNode;
    nextNode = nullptr;

    TernaryNode* node = &(*nodePtr)->as<TernaryNode>();
    MOZ_ASSERT(node->isKind(ParseNodeKind::ConditionalExpr));

    ParseNode** expr = node->unsafeKid1Reference();
    if (!Fold(info, expr)) {
      return false;
    }
    if (!SimplifyCondition(info, expr)) {
      return false;
    }

    ParseNode** ifTruthy = node->unsafeKid2Reference();
    if (!Fold(info, ifTruthy)) {
      return false;
    }

    ParseNode** ifFalsy = node->unsafeKid3Reference();

    // If our C?T:F node has F as another ?: node, *iteratively* constant-
    // fold F *after* folding C and T (and possibly eliminating C and one
    // of T/F entirely); otherwise fold F normally.  Making |nextNode| non-
    // null causes this loop to run again to fold F.
    //
    // Conceivably we could instead/also iteratively constant-fold T, if T
    // were more complex than F.  Such an optimization is unimplemented.
    if ((*ifFalsy)->isKind(ParseNodeKind::ConditionalExpr)) {
      MOZ_ASSERT((*ifFalsy)->is<TernaryNode>());
      nextNode = ifFalsy;
    } else {
      if (!Fold(info, ifFalsy)) {
        return false;
      }
    }

    // Try to constant-fold based on the condition expression.
    Truthiness t = Boolish(*expr);
    if (t == Unknown) {
      continue;
    }

    // Otherwise reduce 'C ? T : F' to T or F as directed by C.
    ParseNode* replacement = t == Truthy ? *ifTruthy : *ifFalsy;

    // Otherwise perform a replacement.  This invalidates |nextNode|, so
    // reset it (if the replacement requires folding) or clear it (if
    // |ifFalsy| is dead code) as needed.
    if (nextNode) {
      nextNode = (*nextNode == replacement) ? nodePtr : nullptr;
    }
    ReplaceNode(nodePtr, replacement);
  } while (nextNode);

  return true;
}

static bool FoldIf(FoldInfo info, ParseNode** nodePtr) {
  ParseNode** nextNode = nodePtr;

  do {
    // |nextNode| on entry points to the initial |if| to be folded.  Reset
    // it to exit the loop when the |else| arm isn't another |if|.
    nodePtr = nextNode;
    nextNode = nullptr;

    TernaryNode* node = &(*nodePtr)->as<TernaryNode>();
    MOZ_ASSERT(node->isKind(ParseNodeKind::IfStmt));

    ParseNode** expr = node->unsafeKid1Reference();
    if (!Fold(info, expr)) {
      return false;
    }
    if (!SimplifyCondition(info, expr)) {
      return false;
    }

    ParseNode** consequent = node->unsafeKid2Reference();
    if (!Fold(info, consequent)) {
      return false;
    }

    ParseNode** alternative = node->unsafeKid3Reference();
    if (*alternative) {
      // If in |if (C) T; else F;| we have |F| as another |if|,
      // *iteratively* constant-fold |F| *after* folding |C| and |T| (and
      // possibly completely replacing the whole thing with |T| or |F|);
      // otherwise fold F normally.  Making |nextNode| non-null causes
      // this loop to run again to fold F.
      if ((*alternative)->isKind(ParseNodeKind::IfStmt)) {
        MOZ_ASSERT((*alternative)->is<TernaryNode>());
        nextNode = alternative;
      } else {
        if (!Fold(info, alternative)) {
          return false;
        }
      }
    }

    // Eliminate the consequent or alternative if the condition has
    // constant truthiness.
    Truthiness t = Boolish(*expr);
    if (t == Unknown) {
      continue;
    }

    // Careful!  Either of these can be null: |replacement| in |if (0) T;|,
    // and |discarded| in |if (true) T;|.
    ParseNode* replacement;
    ParseNode* discarded;
    if (t == Truthy) {
      replacement = *consequent;
      discarded = *alternative;
    } else {
      replacement = *alternative;
      discarded = *consequent;
    }

    bool performReplacement = true;
    if (discarded) {
      // A declaration that hoists outside the discarded arm prevents the
      // |if| from being folded away.
      bool containsHoistedDecls;
      if (!ContainsHoistedDeclaration(info.cx, discarded,
                                      &containsHoistedDecls)) {
        return false;
      }

      performReplacement = !containsHoistedDecls;
    }

    if (!performReplacement) {
      continue;
    }

    if (!replacement) {
      // If there's no replacement node, we have a constantly-false |if|
      // with no |else|.  Replace the entire thing with an empty
      // statement list.
      if (!TryReplaceNode(nodePtr,
                          info.handler->newStatementList(node->pn_pos))) {
        return false;
      }
    } else {
      // Replacement invalidates |nextNode|, so reset it (if the
      // replacement requires folding) or clear it (if |alternative|
      // is dead code) as needed.
      if (nextNode) {
        nextNode = (*nextNode == replacement) ? nodePtr : nullptr;
      }
      ReplaceNode(nodePtr, replacement);
    }
  } while (nextNode);

  return true;
}

static double ComputeBinary(ParseNodeKind kind, double left, double right) {
  if (kind == ParseNodeKind::AddExpr) {
    return left + right;
  }

  if (kind == ParseNodeKind::SubExpr) {
    return left - right;
  }

  if (kind == ParseNodeKind::MulExpr) {
    return left * right;
  }

  if (kind == ParseNodeKind::ModExpr) {
    return NumberMod(left, right);
  }

  if (kind == ParseNodeKind::UrshExpr) {
    return ToUint32(left) >> (ToUint32(right) & 31);
  }

  if (kind == ParseNodeKind::DivExpr) {
    return NumberDiv(left, right);
  }

  MOZ_ASSERT(kind == ParseNodeKind::LshExpr || kind == ParseNodeKind::RshExpr);

  int32_t i = ToInt32(left);
  uint32_t j = ToUint32(right) & 31;
  return int32_t((kind == ParseNodeKind::LshExpr) ? uint32_t(i) << j : i >> j);
}

static bool FoldBinaryArithmetic(FoldInfo info, ParseNode** nodePtr) {
  ListNode* node = &(*nodePtr)->as<ListNode>();
  MOZ_ASSERT(node->isKind(ParseNodeKind::SubExpr) ||
             node->isKind(ParseNodeKind::MulExpr) ||
             node->isKind(ParseNodeKind::LshExpr) ||
             node->isKind(ParseNodeKind::RshExpr) ||
             node->isKind(ParseNodeKind::UrshExpr) ||
             node->isKind(ParseNodeKind::DivExpr) ||
             node->isKind(ParseNodeKind::ModExpr));
  MOZ_ASSERT(node->count() >= 2);

  // Fold each operand to a number if possible.
  ParseNode** listp = node->unsafeHeadReference();
  for (; *listp; listp = &(*listp)->pn_next) {
    if (!FoldType(info, listp, ParseNodeKind::NumberExpr)) {
      return false;
    }
  }
  node->unsafeReplaceTail(listp);

  // Now fold all leading numeric terms together into a single number.
  // (Trailing terms for the non-shift operations can't be folded together
  // due to floating point imprecision.  For example, if |x === -2**53|,
  // |x - 1 - 1 === -2**53| but |x - 2 === -2**53 - 2|.  Shifts could be
  // folded, but it doesn't seem worth the effort.)
  ParseNode** elem = node->unsafeHeadReference();
  ParseNode** next = &(*elem)->pn_next;
  if ((*elem)->isKind(ParseNodeKind::NumberExpr)) {
    ParseNodeKind kind = node->getKind();
    while (true) {
      if (!*next || !(*next)->isKind(ParseNodeKind::NumberExpr)) {
        break;
      }

      double d = ComputeBinary(kind, (*elem)->as<NumericLiteral>().value(),
                               (*next)->as<NumericLiteral>().value());

      TokenPos pos((*elem)->pn_pos.begin, (*next)->pn_pos.end);
      if (!TryReplaceNode(elem, info.handler->newNumber(d, NoDecimal, pos))) {
        return false;
      }

      (*elem)->pn_next = (*next)->pn_next;
      next = &(*elem)->pn_next;
      node->unsafeDecrementCount();
    }

    if (node->count() == 1) {
      MOZ_ASSERT(node->head() == *elem);
      MOZ_ASSERT((*elem)->isKind(ParseNodeKind::NumberExpr));

      if (!TryReplaceNode(nodePtr, *elem)) {
        return false;
      }
    }
  }

  return true;
}

static bool FoldExponentiation(FoldInfo info, ParseNode** nodePtr) {
  ListNode* node = &(*nodePtr)->as<ListNode>();
  MOZ_ASSERT(node->isKind(ParseNodeKind::PowExpr));
  MOZ_ASSERT(node->count() >= 2);

  // Fold each operand, ideally into a number.
  ParseNode** listp = node->unsafeHeadReference();
  for (; *listp; listp = &(*listp)->pn_next) {
    if (!FoldType(info, listp, ParseNodeKind::NumberExpr)) {
      return false;
    }
  }

  node->unsafeReplaceTail(listp);

  // Unlike all other binary arithmetic operators, ** is right-associative:
  // 2**3**5 is 2**(3**5), not (2**3)**5.  As list nodes singly-link their
  // children, full constant-folding requires either linear space or dodgy
  // in-place linked list reversal.  So we only fold one exponentiation: it's
  // easy and addresses common cases like |2**32|.
  if (node->count() > 2) {
    return true;
  }

  ParseNode* base = node->head();
  ParseNode* exponent = base->pn_next;
  if (!base->isKind(ParseNodeKind::NumberExpr) ||
      !exponent->isKind(ParseNodeKind::NumberExpr)) {
    return true;
  }

  double d1 = base->as<NumericLiteral>().value();
  double d2 = exponent->as<NumericLiteral>().value();

  return TryReplaceNode(nodePtr, info.handler->newNumber(
                                     ecmaPow(d1, d2), NoDecimal, node->pn_pos));
}

static bool FoldElement(FoldInfo info, ParseNode** nodePtr) {
  PropertyByValue* elem = &(*nodePtr)->as<PropertyByValue>();

  ParseNode* expr = &elem->expression();
  ParseNode* key = &elem->key();
  TaggedParserAtomIndex name;
  if (key->isKind(ParseNodeKind::StringExpr)) {
    auto keyIndex = key->as<NameNode>().atom();
    uint32_t index;
    if (info.parserAtoms.isIndex(keyIndex, &index)) {
      // Optimization 1: We have something like expr["100"]. This is
      // equivalent to expr[100] which is faster.
      if (!TryReplaceNode(
              elem->unsafeRightReference(),
              info.handler->newNumber(index, NoDecimal, key->pn_pos))) {
        return false;
      }
      key = &elem->key();
    } else {
      name = keyIndex;
    }
  } else if (key->isKind(ParseNodeKind::NumberExpr)) {
    auto* numeric = &key->as<NumericLiteral>();
    double number = numeric->value();
    if (number != ToUint32(number)) {
      // Optimization 2: We have something like expr[3.14]. The number
      // isn't an array index, so it converts to a string ("3.14"),
      // enabling optimization 3 below.
      name = numeric->toAtom(info.cx, info.parserAtoms);
      if (!name) {
        return false;
      }
    }
  }

  // If we don't have a name, we can't optimize to getprop.
  if (!name) {
    return true;
  }

  // Optimization 3: We have expr["foo"] where foo is not an index.  Convert
  // to a property access (like expr.foo) that optimizes better downstream.

  NameNode* propertyNameExpr = info.handler->newPropertyName(name, key->pn_pos);
  if (!propertyNameExpr) {
    return false;
  }
  if (!TryReplaceNode(
          nodePtr, info.handler->newPropertyAccess(expr, propertyNameExpr))) {
    return false;
  }

  return true;
}

static bool FoldAdd(FoldInfo info, ParseNode** nodePtr) {
  ListNode* node = &(*nodePtr)->as<ListNode>();

  MOZ_ASSERT(node->isKind(ParseNodeKind::AddExpr));
  MOZ_ASSERT(node->count() >= 2);

  // Fold leading numeric operands together:
  //
  //   (1 + 2 + x)  becomes  (3 + x)
  //
  // Don't go past the leading operands: additions after a string are
  // string concatenations, not additions: ("1" + 2 + 3 === "123").
  ParseNode** current = node->unsafeHeadReference();
  ParseNode** next = &(*current)->pn_next;
  if ((*current)->isKind(ParseNodeKind::NumberExpr)) {
    do {
      if (!(*next)->isKind(ParseNodeKind::NumberExpr)) {
        break;
      }

      double left = (*current)->as<NumericLiteral>().value();
      double right = (*next)->as<NumericLiteral>().value();
      TokenPos pos((*current)->pn_pos.begin, (*next)->pn_pos.end);

      if (!TryReplaceNode(
              current, info.handler->newNumber(left + right, NoDecimal, pos))) {
        return false;
      }

      (*current)->pn_next = (*next)->pn_next;
      next = &(*current)->pn_next;

      node->unsafeDecrementCount();
    } while (*next);
  }

  // If any operands remain, attempt string concatenation folding.
  do {
    // If no operands remain, we're done.
    if (!*next) {
      break;
    }

    // (number + string) is string concatenation *only* at the start of
    // the list: (x + 1 + "2" !== x + "12") when x is a number.
    if ((*current)->isKind(ParseNodeKind::NumberExpr) &&
        (*next)->isKind(ParseNodeKind::StringExpr)) {
      if (!FoldType(info, current, ParseNodeKind::StringExpr)) {
        return false;
      }
      next = &(*current)->pn_next;
    }

    // The first string forces all subsequent additions to be
    // string concatenations.
    do {
      if ((*current)->isKind(ParseNodeKind::StringExpr)) {
        break;
      }

      current = next;
      next = &(*current)->pn_next;
    } while (*next);

    // If there's nothing left to fold, we're done.
    if (!*next) {
      break;
    }

    do {
      // Concat all strings.
      MOZ_ASSERT((*current)->isKind(ParseNodeKind::StringExpr));

      // To avoid unnecessarily copy when there's no strings after the
      // first item, lazily construct StringBuffer and append the first item.
      mozilla::Maybe<StringBuffer> accum;
      TaggedParserAtomIndex firstAtom;
      firstAtom = (*current)->as<NameNode>().atom();

      do {
        // Try folding the next operand to a string.
        if (!FoldType(info, next, ParseNodeKind::StringExpr)) {
          return false;
        }

        // Stop glomming once folding doesn't produce a string.
        if (!(*next)->isKind(ParseNodeKind::StringExpr)) {
          break;
        }

        if (!accum) {
          accum.emplace(info.cx);
          if (!accum->append(info.parserAtoms, firstAtom)) {
            return false;
          }
        }
        // Append this string and remove the node.
        if (!accum->append(info.parserAtoms, (*next)->as<NameNode>().atom())) {
          return false;
        }

        (*current)->pn_next = (*next)->pn_next;
        next = &(*current)->pn_next;

        node->unsafeDecrementCount();
      } while (*next);

      // Replace with concatenation if we multiple nodes.
      if (accum) {
        auto combination = accum->finishParserAtom(info.parserAtoms);
        if (!combination) {
          return false;
        }

        // Replace |current|'s string with the entire combination.
        MOZ_ASSERT((*current)->isKind(ParseNodeKind::StringExpr));
        (*current)->as<NameNode>().setAtom(combination);
      }

      // If we're out of nodes, we're done.
      if (!*next) {
        break;
      }

      current = next;
      next = &(*current)->pn_next;

      // If we're out of nodes *after* the non-foldable-to-string
      // node, we're done.
      if (!*next) {
        break;
      }

      // Otherwise find the next node foldable to a string, and loop.
      do {
        current = next;

        if (!FoldType(info, current, ParseNodeKind::StringExpr)) {
          return false;
        }
        next = &(*current)->pn_next;
      } while (!(*current)->isKind(ParseNodeKind::StringExpr) && *next);
    } while (*next);
  } while (false);

  MOZ_ASSERT(!*next, "must have considered all nodes here");
  MOZ_ASSERT(!(*current)->pn_next, "current node must be the last node");

  node->unsafeReplaceTail(&(*current)->pn_next);

  if (node->count() == 1) {
    // We reduced the list to a constant.  Replace the ParseNodeKind::Add node
    // with that constant.
    ReplaceNode(nodePtr, *current);
  }

  return true;
}

class FoldVisitor : public RewritingParseNodeVisitor<FoldVisitor> {
  using Base = RewritingParseNodeVisitor;

  JSContext* cx;
  ParserAtomsTable& parserAtoms;
  FullParseHandler* handler;

  FoldInfo info() const { return FoldInfo{cx, parserAtoms, handler}; }

 public:
  explicit FoldVisitor(JSContext* cx, ParserAtomsTable& parserAtoms,
                       FullParseHandler* handler)
      : RewritingParseNodeVisitor(cx),
        cx(cx),
        parserAtoms(parserAtoms),
        handler(handler) {}

  bool visitElemExpr(ParseNode*& pn) {
    return Base::visitElemExpr(pn) && FoldElement(info(), &pn);
  }

  bool visitTypeOfExpr(ParseNode*& pn) {
    return Base::visitTypeOfExpr(pn) && FoldTypeOfExpr(info(), &pn);
  }

  bool visitDeleteExpr(ParseNode*& pn) {
    return Base::visitDeleteExpr(pn) && FoldDeleteExpr(info(), &pn);
  }

  bool visitDeleteElemExpr(ParseNode*& pn) {
    return Base::visitDeleteElemExpr(pn) && FoldDeleteElement(info(), &pn);
  }

  bool visitNotExpr(ParseNode*& pn) {
    return Base::visitNotExpr(pn) && FoldNot(info(), &pn);
  }

  bool visitBitNotExpr(ParseNode*& pn) {
    return Base::visitBitNotExpr(pn) && FoldUnaryArithmetic(info(), &pn);
  }

  bool visitPosExpr(ParseNode*& pn) {
    return Base::visitPosExpr(pn) && FoldUnaryArithmetic(info(), &pn);
  }

  bool visitNegExpr(ParseNode*& pn) {
    return Base::visitNegExpr(pn) && FoldUnaryArithmetic(info(), &pn);
  }

  bool visitPowExpr(ParseNode*& pn) {
    return Base::visitPowExpr(pn) && FoldExponentiation(info(), &pn);
  }

  bool visitMulExpr(ParseNode*& pn) {
    return Base::visitMulExpr(pn) && FoldBinaryArithmetic(info(), &pn);
  }

  bool visitDivExpr(ParseNode*& pn) {
    return Base::visitDivExpr(pn) && FoldBinaryArithmetic(info(), &pn);
  }

  bool visitModExpr(ParseNode*& pn) {
    return Base::visitModExpr(pn) && FoldBinaryArithmetic(info(), &pn);
  }

  bool visitAddExpr(ParseNode*& pn) {
    return Base::visitAddExpr(pn) && FoldAdd(info(), &pn);
  }

  bool visitSubExpr(ParseNode*& pn) {
    return Base::visitSubExpr(pn) && FoldBinaryArithmetic(info(), &pn);
  }

  bool visitLshExpr(ParseNode*& pn) {
    return Base::visitLshExpr(pn) && FoldBinaryArithmetic(info(), &pn);
  }

  bool visitRshExpr(ParseNode*& pn) {
    return Base::visitRshExpr(pn) && FoldBinaryArithmetic(info(), &pn);
  }

  bool visitUrshExpr(ParseNode*& pn) {
    return Base::visitUrshExpr(pn) && FoldBinaryArithmetic(info(), &pn);
  }

  bool visitAndExpr(ParseNode*& pn) {
    // Note that this does result in the unfortunate fact that dead arms of this
    // node get constant folded. The same goes for visitOr and visitCoalesce.
    return Base::visitAndExpr(pn) && FoldAndOrCoalesce(info(), &pn);
  }

  bool visitOrExpr(ParseNode*& pn) {
    return Base::visitOrExpr(pn) && FoldAndOrCoalesce(info(), &pn);
  }

  bool visitCoalesceExpr(ParseNode*& pn) {
    return Base::visitCoalesceExpr(pn) && FoldAndOrCoalesce(info(), &pn);
  }

  bool visitConditionalExpr(ParseNode*& pn) {
    // Don't call base-class visitConditional because FoldConditional processes
    // pn's child nodes specially to save stack space.
    return FoldConditional(info(), &pn);
  }

 private:
  bool internalVisitCall(BinaryNode* node) {
    MOZ_ASSERT(node->isKind(ParseNodeKind::CallExpr) ||
               node->isKind(ParseNodeKind::OptionalCallExpr) ||
               node->isKind(ParseNodeKind::SuperCallExpr) ||
               node->isKind(ParseNodeKind::NewExpr) ||
               node->isKind(ParseNodeKind::TaggedTemplateExpr));

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
    // As an exception to this, we do allow folding the function in
    // `(function() { ... })()` (the module pattern), because that lets us
    // constant fold code inside that function.
    //
    // See bug 537673 and bug 1182373.
    ParseNode* callee = node->left();
    if (node->isKind(ParseNodeKind::NewExpr) || !callee->isInParens() ||
        callee->is<FunctionNode>()) {
      if (!visit(*node->unsafeLeftReference())) {
        return false;
      }
    }

    if (!visit(*node->unsafeRightReference())) {
      return false;
    }

    return true;
  }

 public:
  bool visitCallExpr(ParseNode*& pn) {
    return internalVisitCall(&pn->as<BinaryNode>());
  }

  bool visitOptionalCallExpr(ParseNode*& pn) {
    return internalVisitCall(&pn->as<BinaryNode>());
  }

  bool visitNewExpr(ParseNode*& pn) {
    return internalVisitCall(&pn->as<BinaryNode>());
  }

  bool visitSuperCallExpr(ParseNode*& pn) {
    return internalVisitCall(&pn->as<BinaryNode>());
  }

  bool visitTaggedTemplateExpr(ParseNode*& pn) {
    return internalVisitCall(&pn->as<BinaryNode>());
  }

  bool visitIfStmt(ParseNode*& pn) {
    // Don't call base-class visitIf because FoldIf processes pn's child nodes
    // specially to save stack space.
    return FoldIf(info(), &pn);
  }

  bool visitForStmt(ParseNode*& pn) {
    if (!Base::visitForStmt(pn)) {
      return false;
    }

    ForNode& stmt = pn->as<ForNode>();
    if (stmt.left()->isKind(ParseNodeKind::ForHead)) {
      TernaryNode& head = stmt.left()->as<TernaryNode>();
      ParseNode** test = head.unsafeKid2Reference();
      if (*test) {
        if (!SimplifyCondition(info(), test)) {
          return false;
        }
        if ((*test)->isKind(ParseNodeKind::TrueExpr)) {
          *test = nullptr;
        }
      }
    }

    return true;
  }

  bool visitWhileStmt(ParseNode*& pn) {
    BinaryNode& node = pn->as<BinaryNode>();
    return Base::visitWhileStmt(pn) &&
           SimplifyCondition(info(), node.unsafeLeftReference());
  }

  bool visitDoWhileStmt(ParseNode*& pn) {
    BinaryNode& node = pn->as<BinaryNode>();
    return Base::visitDoWhileStmt(pn) &&
           SimplifyCondition(info(), node.unsafeRightReference());
  }

  bool visitFunction(ParseNode*& pn) {
    FunctionNode& node = pn->as<FunctionNode>();

    // Don't constant-fold inside "use asm" code, as this could create a parse
    // tree that doesn't type-check as asm.js.
    if (node.funbox()->useAsmOrInsideUseAsm()) {
      return true;
    }

    return Base::visitFunction(pn);
  }

  bool visitArrayExpr(ParseNode*& pn) {
    if (!Base::visitArrayExpr(pn)) {
      return false;
    }

    ListNode* list = &pn->as<ListNode>();
    // Empty arrays are non-constant, since we cannot easily determine their
    // type.
    if (list->hasNonConstInitializer() && list->count() > 0) {
      for (ParseNode* node : list->contents()) {
        if (!node->isConstant()) {
          return true;
        }
      }
      list->unsetHasNonConstInitializer();
    }
    return true;
  }

  bool visitObjectExpr(ParseNode*& pn) {
    if (!Base::visitObjectExpr(pn)) {
      return false;
    }

    ListNode* list = &pn->as<ListNode>();
    if (list->hasNonConstInitializer()) {
      for (ParseNode* node : list->contents()) {
        if (node->getKind() != ParseNodeKind::PropertyDefinition) {
          return true;
        }
        BinaryNode* binary = &node->as<BinaryNode>();
        if (binary->left()->isKind(ParseNodeKind::ComputedName)) {
          return true;
        }
        if (!binary->right()->isConstant()) {
          return true;
        }
      }
      list->unsetHasNonConstInitializer();
    }
    return true;
  }
};

static bool Fold(JSContext* cx, ParserAtomsTable& parserAtoms,
                 FullParseHandler* handler, ParseNode** pnp) {
  FoldVisitor visitor(cx, parserAtoms, handler);
  return visitor.visit(*pnp);
}
static bool Fold(FoldInfo info, ParseNode** pnp) {
  return Fold(info.cx, info.parserAtoms, info.handler, pnp);
}

bool frontend::FoldConstants(JSContext* cx, ParserAtomsTable& parserAtoms,
                             ParseNode** pnp, FullParseHandler* handler) {
  AutoTraceLog traceLog(TraceLoggerForCurrentThread(cx),
                        TraceLogger_BytecodeFoldConstants);

  return Fold(cx, parserAtoms, handler, pnp);
}
