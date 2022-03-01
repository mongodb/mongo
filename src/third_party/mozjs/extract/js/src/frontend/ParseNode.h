/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ParseNode_h
#define frontend_ParseNode_h

#include "mozilla/Assertions.h"
#include "mozilla/Span.h"  // mozilla::Span

#include <iterator>
#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"  // js::Bit

#include "frontend/FunctionSyntaxKind.h"  // FunctionSyntaxKind
#include "frontend/NameAnalysisTypes.h"   // PrivateNameKind
#include "frontend/ParserAtom.h"          // TaggedParserAtomIndex
#include "frontend/Stencil.h"             // BigIntStencil
#include "frontend/Token.h"
#include "js/RootingAPI.h"
#include "vm/BytecodeUtil.h"
#include "vm/Scope.h"
#include "vm/ScopeKind.h"
#include "vm/StringType.h"

// [SMDOC] ParseNode tree lifetime information
//
// - All the `ParseNode` instances MUST BE explicitly allocated in the context's
//   `LifoAlloc`. This is typically implemented by the `FullParseHandler` or it
//   can be reimplemented with a custom `new_`.
//
// - The tree is bulk-deallocated when the parser is deallocated. Consequently,
//   references to a subtree MUST NOT exist once the parser has been
//   deallocated.
//
// - This bulk-deallocation DOES NOT run destructors.
//
// - Instances of `LexicalScope::ParserData` and `ClassBodyScope::ParserData`
//   MUST BE allocated as instances of `ParseNode`, in the same `LifoAlloc`.
//   They are bulk-deallocated alongside the rest of the tree.

struct JSContext;

namespace JS {
class BigInt;
}

namespace js {

class GenericPrinter;
class LifoAlloc;
class RegExpObject;

namespace frontend {

class ParserBase;
class ParseContext;
class ParserAtomsTable;
struct ExtensibleCompilationStencil;
class ParserSharedBase;
class FullParseHandler;

class FunctionBox;

#define FOR_EACH_PARSE_NODE_KIND(F)                              \
  F(EmptyStmt, NullaryNode)                                      \
  F(ExpressionStmt, UnaryNode)                                   \
  F(CommaExpr, ListNode)                                         \
  F(ConditionalExpr, ConditionalExpression)                      \
  F(PropertyDefinition, PropertyDefinition)                      \
  F(Shorthand, BinaryNode)                                       \
  F(PosExpr, UnaryNode)                                          \
  F(NegExpr, UnaryNode)                                          \
  F(PreIncrementExpr, UnaryNode)                                 \
  F(PostIncrementExpr, UnaryNode)                                \
  F(PreDecrementExpr, UnaryNode)                                 \
  F(PostDecrementExpr, UnaryNode)                                \
  F(PropertyNameExpr, NameNode)                                  \
  F(DotExpr, PropertyAccess)                                     \
  F(ElemExpr, PropertyByValue)                                   \
  F(PrivateMemberExpr, PrivateMemberAccess)                      \
  F(OptionalDotExpr, OptionalPropertyAccess)                     \
  F(OptionalChain, UnaryNode)                                    \
  F(OptionalElemExpr, OptionalPropertyByValue)                   \
  F(OptionalPrivateMemberExpr, OptionalPrivateMemberAccess)      \
  F(OptionalCallExpr, BinaryNode)                                \
  F(ArrayExpr, ListNode)                                         \
  F(Elision, NullaryNode)                                        \
  F(StatementList, ListNode)                                     \
  F(LabelStmt, LabeledStatement)                                 \
  F(ObjectExpr, ListNode)                                        \
  F(CallExpr, BinaryNode)                                        \
  F(Arguments, ListNode)                                         \
  F(Name, NameNode)                                              \
  F(ObjectPropertyName, NameNode)                                \
  F(PrivateName, NameNode)                                       \
  F(ComputedName, UnaryNode)                                     \
  F(NumberExpr, NumericLiteral)                                  \
  F(BigIntExpr, BigIntLiteral)                                   \
  F(StringExpr, NameNode)                                        \
  F(TemplateStringListExpr, ListNode)                            \
  F(TemplateStringExpr, NameNode)                                \
  F(TaggedTemplateExpr, BinaryNode)                              \
  F(CallSiteObj, CallSiteNode)                                   \
  F(RegExpExpr, RegExpLiteral)                                   \
  F(TrueExpr, BooleanLiteral)                                    \
  F(FalseExpr, BooleanLiteral)                                   \
  F(NullExpr, NullLiteral)                                       \
  F(RawUndefinedExpr, RawUndefinedLiteral)                       \
  F(ThisExpr, UnaryNode)                                         \
  F(Function, FunctionNode)                                      \
  F(Module, ModuleNode)                                          \
  F(IfStmt, TernaryNode)                                         \
  F(SwitchStmt, SwitchStatement)                                 \
  F(Case, CaseClause)                                            \
  F(WhileStmt, BinaryNode)                                       \
  F(DoWhileStmt, BinaryNode)                                     \
  F(ForStmt, ForNode)                                            \
  F(BreakStmt, BreakStatement)                                   \
  F(ContinueStmt, ContinueStatement)                             \
  F(VarStmt, ListNode)                                           \
  F(ConstDecl, ListNode)                                         \
  F(WithStmt, BinaryNode)                                        \
  F(ReturnStmt, UnaryNode)                                       \
  F(NewExpr, BinaryNode)                                         \
  /* Delete operations.  These must be sequential. */            \
  F(DeleteNameExpr, UnaryNode)                                   \
  F(DeletePropExpr, UnaryNode)                                   \
  F(DeleteElemExpr, UnaryNode)                                   \
  F(DeleteOptionalChainExpr, UnaryNode)                          \
  F(DeleteExpr, UnaryNode)                                       \
  F(TryStmt, TernaryNode)                                        \
  F(Catch, BinaryNode)                                           \
  F(ThrowStmt, UnaryNode)                                        \
  F(DebuggerStmt, DebuggerStatement)                             \
  F(Generator, NullaryNode)                                      \
  F(InitialYield, UnaryNode)                                     \
  F(YieldExpr, UnaryNode)                                        \
  F(YieldStarExpr, UnaryNode)                                    \
  F(LexicalScope, LexicalScopeNode)                              \
  F(LetDecl, ListNode)                                           \
  F(ImportDecl, BinaryNode)                                      \
  F(ImportSpecList, ListNode)                                    \
  F(ImportSpec, BinaryNode)                                      \
  F(ImportNamespaceSpec, UnaryNode)                              \
  F(ExportStmt, UnaryNode)                                       \
  F(ExportFromStmt, BinaryNode)                                  \
  F(ExportDefaultStmt, BinaryNode)                               \
  F(ExportSpecList, ListNode)                                    \
  F(ExportSpec, BinaryNode)                                      \
  F(ExportNamespaceSpec, UnaryNode)                              \
  F(ExportBatchSpecStmt, NullaryNode)                            \
  F(ForIn, TernaryNode)                                          \
  F(ForOf, TernaryNode)                                          \
  F(ForHead, TernaryNode)                                        \
  F(ParamsBody, ListNode)                                        \
  F(Spread, UnaryNode)                                           \
  F(MutateProto, UnaryNode)                                      \
  F(ClassDecl, ClassNode)                                        \
  F(DefaultConstructor, ClassMethod)                             \
  F(ClassBodyScope, ClassBodyScopeNode)                          \
  F(ClassMethod, ClassMethod)                                    \
  F(StaticClassBlock, StaticClassBlock)                          \
  F(ClassField, ClassField)                                      \
  F(ClassMemberList, ListNode)                                   \
  F(ClassNames, ClassNames)                                      \
  F(NewTargetExpr, BinaryNode)                                   \
  F(PosHolder, NullaryNode)                                      \
  F(SuperBase, UnaryNode)                                        \
  F(SuperCallExpr, BinaryNode)                                   \
  F(SetThis, BinaryNode)                                         \
  F(ImportMetaExpr, BinaryNode)                                  \
  F(CallImportExpr, BinaryNode)                                  \
  F(InitExpr, BinaryNode)                                        \
                                                                 \
  /* Unary operators. */                                         \
  F(TypeOfNameExpr, UnaryNode)                                   \
  F(TypeOfExpr, UnaryNode)                                       \
  F(VoidExpr, UnaryNode)                                         \
  F(NotExpr, UnaryNode)                                          \
  F(BitNotExpr, UnaryNode)                                       \
  F(AwaitExpr, UnaryNode)                                        \
                                                                 \
  /*                                                             \
   * Binary operators.                                           \
   * This list must be kept in the same order in several places: \
   *   - The binary operators in ParseNode.h                     \
   *   - the binary operators in TokenKind.h                     \
   *   - the precedence list in Parser.cpp                       \
   *   - the JSOp code list in BytecodeEmitter.cpp               \
   */                                                            \
  F(CoalesceExpr, ListNode)                                      \
  F(OrExpr, ListNode)                                            \
  F(AndExpr, ListNode)                                           \
  F(BitOrExpr, ListNode)                                         \
  F(BitXorExpr, ListNode)                                        \
  F(BitAndExpr, ListNode)                                        \
  F(StrictEqExpr, ListNode)                                      \
  F(EqExpr, ListNode)                                            \
  F(StrictNeExpr, ListNode)                                      \
  F(NeExpr, ListNode)                                            \
  F(LtExpr, ListNode)                                            \
  F(LeExpr, ListNode)                                            \
  F(GtExpr, ListNode)                                            \
  F(GeExpr, ListNode)                                            \
  F(InstanceOfExpr, ListNode)                                    \
  F(InExpr, ListNode)                                            \
  F(PrivateInExpr, ListNode)                                     \
  F(LshExpr, ListNode)                                           \
  F(RshExpr, ListNode)                                           \
  F(UrshExpr, ListNode)                                          \
  F(AddExpr, ListNode)                                           \
  F(SubExpr, ListNode)                                           \
  F(MulExpr, ListNode)                                           \
  F(DivExpr, ListNode)                                           \
  F(ModExpr, ListNode)                                           \
  F(PowExpr, ListNode)                                           \
                                                                 \
  /* Assignment operators (= += -= etc.). */                     \
  /* AssignmentNode::test assumes all these are consecutive. */  \
  F(AssignExpr, AssignmentNode)                                  \
  F(AddAssignExpr, AssignmentNode)                               \
  F(SubAssignExpr, AssignmentNode)                               \
  F(CoalesceAssignExpr, AssignmentNode)                          \
  F(OrAssignExpr, AssignmentNode)                                \
  F(AndAssignExpr, AssignmentNode)                               \
  F(BitOrAssignExpr, AssignmentNode)                             \
  F(BitXorAssignExpr, AssignmentNode)                            \
  F(BitAndAssignExpr, AssignmentNode)                            \
  F(LshAssignExpr, AssignmentNode)                               \
  F(RshAssignExpr, AssignmentNode)                               \
  F(UrshAssignExpr, AssignmentNode)                              \
  F(MulAssignExpr, AssignmentNode)                               \
  F(DivAssignExpr, AssignmentNode)                               \
  F(ModAssignExpr, AssignmentNode)                               \
  F(PowAssignExpr, AssignmentNode)

/*
 * Parsing builds a tree of nodes that directs code generation.  This tree is
 * not a concrete syntax tree in all respects (for example, || and && are left
 * associative, but (A && B && C) translates into the right-associated tree
 * <A && <B && C>> so that code generation can emit a left-associative branch
 * around <B && C> when A is false).  Nodes are labeled by kind.
 *
 * The long comment after this enum block describes the kinds in detail.
 */
enum class ParseNodeKind : uint16_t {
  // These constants start at 1001, the better to catch
  LastUnused = 1000,
#define EMIT_ENUM(name, _type) name,
  FOR_EACH_PARSE_NODE_KIND(EMIT_ENUM)
#undef EMIT_ENUM
      Limit,
  Start = LastUnused + 1,
  BinOpFirst = ParseNodeKind::CoalesceExpr,
  BinOpLast = ParseNodeKind::PowExpr,
  AssignmentStart = ParseNodeKind::AssignExpr,
  AssignmentLast = ParseNodeKind::PowAssignExpr,
};

inline bool IsDeleteKind(ParseNodeKind kind) {
  return ParseNodeKind::DeleteNameExpr <= kind &&
         kind <= ParseNodeKind::DeleteExpr;
}

inline bool IsTypeofKind(ParseNodeKind kind) {
  return ParseNodeKind::TypeOfNameExpr <= kind &&
         kind <= ParseNodeKind::TypeOfExpr;
}

/*
 * <Definitions>
 * Function (FunctionNode)
 *   funbox: ptr to js::FunctionBox
 *   body: ParamsBody or null for lazily-parsed function, ordinarily;
 *         ParseNodeKind::LexicalScope for implicit function in genexpr
 *   syntaxKind: the syntax of the function
 * ParamsBody (ListNode)
 *   head: list of formal parameters with
 *           * Name node with non-empty name for SingleNameBinding without
 *             Initializer
 *           * AssignExpr node for SingleNameBinding with Initializer
 *           * Name node with empty name for destructuring
 *               expr: Array or Object for BindingPattern without
 *                     Initializer, Assign for BindingPattern with
 *                     Initializer
 *         followed by either:
 *           * StatementList node for function body statements
 *           * ReturnStmt for expression closure
 *   count: number of formal parameters + 1
 * Spread (UnaryNode)
 *   kid: expression being spread
 * ClassDecl (ClassNode)
 *   kid1: ClassNames for class name. can be null for anonymous class.
 *   kid2: expression after `extends`. null if no expression
 *   kid3: either of
 *           * ClassMemberList, if anonymous class
 *           * LexicalScopeNode which contains ClassMemberList as scopeBody,
 *             if named class
 * ClassNames (ClassNames)
 *   left: Name node for outer binding, or null if the class is an expression
 *         that doesn't create an outer binding
 *   right: Name node for inner binding
 * ClassMemberList (ListNode)
 *   head: list of N ClassMethod, ClassField or StaticClassBlock nodes
 *   count: N >= 0
 * DefaultConstructor (ClassMethod)
 *   name: propertyName
 *   method: methodDefinition
 * ClassMethod (ClassMethod)
 *   name: propertyName
 *   method: methodDefinition
 *   initializerIfPrivate: initializer to stamp private method onto instance
 * Module (ModuleNode)
 *   body: statement list of the module
 *
 * <Statements>
 * StatementList (ListNode)
 *   head: list of N statements
 *   count: N >= 0
 * IfStmt (TernaryNode)
 *   kid1: cond
 *   kid2: then
 *   kid3: else or null
 * SwitchStmt (SwitchStatement)
 *   left: discriminant
 *   right: LexicalScope node that contains the list of Case nodes, with at
 *          most one default node.
 *   hasDefault: true if there's a default case
 * Case (CaseClause)
 *   left: case-expression if CaseClause, or null if DefaultClause
 *   right: StatementList node for this case's statements
 * WhileStmt (BinaryNode)
 *   left: cond
 *   right: body
 * DoWhileStmt (BinaryNode)
 *   left: body
 *   right: cond
 * ForStmt (ForNode)
 *   left: one of
 *           * ForIn: for (x in y) ...
 *           * ForOf: for (x of x) ...
 *           * ForHead: for (;;) ...
 *   right: body
 * ForIn (TernaryNode)
 *   kid1: declaration or expression to left of 'in'
 *   kid2: null
 *   kid3: object expr to right of 'in'
 * ForOf (TernaryNode)
 *   kid1: declaration or expression to left of 'of'
 *   kid2: null
 *   kid3: expr to right of 'of'
 * ForHead (TernaryNode)
 *   kid1:  init expr before first ';' or nullptr
 *   kid2:  cond expr before second ';' or nullptr
 *   kid3:  update expr after second ';' or nullptr
 * ThrowStmt (UnaryNode)
 *   kid: thrown exception
 * TryStmt (TernaryNode)
 *   kid1: try block
 *   kid2: null or LexicalScope for catch-block with scopeBody pointing to a
 *         Catch node
 *   kid3: null or finally block
 * Catch (BinaryNode)
 *   left: Name, Array, or Object catch var node
 *         (Array or Object if destructuring),
 *         or null if optional catch binding
 *   right: catch block statements
 * BreakStmt (BreakStatement)
 *   label: label or null
 * ContinueStmt (ContinueStatement)
 *   label: label or null
 * WithStmt (BinaryNode)
 *   left: head expr
 *   right: body
 * VarStmt, LetDecl, ConstDecl (ListNode)
 *   head: list of N Name or AssignExpr nodes
 *         each name node has either
 *           atom: variable name
 *           expr: initializer or null
 *         or
 *           atom: variable name
 *         each assignment node has
 *           left: pattern
 *           right: initializer
 *   count: N > 0
 * ReturnStmt (UnaryNode)
 *   kid: returned expression, or null if none
 * ExpressionStmt (UnaryNode)
 *   kid: expr
 * EmptyStmt (NullaryNode)
 *   (no fields)
 * LabelStmt (LabeledStatement)
 *   atom: label
 *   expr: labeled statement
 * ImportDecl (BinaryNode)
 *   left: ImportSpecList import specifiers
 *   right: String module specifier
 * ImportSpecList (ListNode)
 *   head: list of N ImportSpec nodes
 *   count: N >= 0 (N = 0 for `import {} from ...`)
 * ImportSpec (BinaryNode)
 *   left: import name
 *   right: local binding name
 * ImportNamespaceSpec (UnaryNode)
 *   kid: local binding name
 * ExportStmt (UnaryNode)
 *   kid: declaration expression
 * ExportFromStmt (BinaryNode)
 *   left: ExportSpecList export specifiers
 *   right: String module specifier
 * ExportSpecList (ListNode)
 *   head: list of N ExportSpec nodes
 *   count: N >= 0 (N = 0 for `export {}`)
 * ExportSpec (BinaryNode)
 *   left: local binding name
 *   right: export name
 * ExportNamespaceSpec (UnaryNode)
 *   kid: export name
 * ExportDefaultStmt (BinaryNode)
 *   left: export default declaration or expression
 *   right: Name node for assignment
 *
 * <Expressions>
 * The `Expr` suffix is used for nodes that can appear anywhere an expression
 * could appear.  It is not used on a few weird kinds like Arguments and
 * CallSiteObj that are always the child node of an expression node, but which
 * can't stand alone.
 *
 * All left-associated binary trees of the same type are optimized into lists
 * to avoid recursion when processing expression chains.
 *
 * CommaExpr (ListNode)
 *   head: list of N comma-separated exprs
 *   count: N >= 2
 * AssignExpr (BinaryNode)
 *   left: target of assignment
 *   right: value to assign
 * AddAssignExpr, SubAssignExpr, CoalesceAssignExpr, OrAssignExpr,
 * AndAssignExpr, BitOrAssignExpr, BitXorAssignExpr, BitAndAssignExpr,
 * LshAssignExpr, RshAssignExpr, UrshAssignExpr, MulAssignExpr, DivAssignExpr,
 * ModAssignExpr, PowAssignExpr (AssignmentNode)
 *   left: target of assignment
 *   right: value to assign
 * ConditionalExpr (ConditionalExpression)
 *   (cond ? thenExpr : elseExpr)
 *   kid1: cond
 *   kid2: thenExpr
 *   kid3: elseExpr
 * CoalesceExpr, OrExpr, AndExpr, BitOrExpr, BitXorExpr,
 * BitAndExpr, StrictEqExpr, EqExpr, StrictNeExpr, NeExpr, LtExpr, LeExpr,
 * GtExpr, GeExpr, InstanceOfExpr, InExpr, LshExpr, RshExpr, UrshExpr, AddExpr,
 * SubExpr, MulExpr, DivExpr, ModExpr, PowExpr (ListNode)
 *   head: list of N subexpressions
 *         All of these operators are left-associative except Pow which is
 *         right-associative, but still forms a list (see comments in
 *         ParseNode::appendOrCreateList).
 *   count: N >= 2
 * PosExpr, NegExpr, VoidExpr, NotExpr, BitNotExpr, TypeOfNameExpr,
 * TypeOfExpr (UnaryNode)
 *   kid: unary expr
 * PreIncrementExpr, PostIncrementExpr, PreDecrementExpr,
 * PostDecrementExpr (UnaryNode)
 *   kid: member expr
 * NewExpr (BinaryNode)
 *   left: ctor expression on the left of the '('
 *   right: Arguments
 * DeleteNameExpr, DeletePropExpr, DeleteElemExpr, DeleteExpr (UnaryNode)
 *   kid: expression that's evaluated, then the overall delete evaluates to
 *        true; can't be a kind for a more-specific ParseNodeKind::Delete*
 *        unless constant folding (or a similar parse tree manipulation) has
 *        occurred
 *          * DeleteNameExpr: Name expr
 *          * DeletePropExpr: Dot expr
 *          * DeleteElemExpr: Elem expr
 *          * DeleteOptionalChainExpr: Member expr
 *          * DeleteExpr: Member expr
 * DeleteOptionalChainExpr (UnaryNode)
 *   kid: expression that's evaluated, then the overall delete evaluates to
 *        true; If constant folding occurs, Elem expr may become Dot expr.
 *        OptionalElemExpr does not get folded into OptionalDot.
 * OptionalChain (UnaryNode)
 *   kid: expression that is evaluated as a chain. An Optional chain contains
 *        one or more optional nodes. It's first node (kid) is always an
 *        optional node, for example: an OptionalElemExpr, OptionalDotExpr, or
 *        OptionalCall.  An OptionalChain will shortcircuit and return
 *        Undefined without evaluating the rest of the expression if any of the
 *        optional nodes it contains are nullish. An optionalChain also can
 *        contain nodes such as DotExpr, ElemExpr, NameExpr CallExpr, etc.
 *        These are evaluated normally.
 *          * OptionalDotExpr: Dot expr with jump
 *          * OptionalElemExpr: Elem expr with jump
 *          * OptionalCallExpr: Call expr with jump
 *          * DotExpr: Dot expr without jump
 *          * ElemExpr: Elem expr without jump
 *          * CallExpr: Call expr without jump
 * PropertyNameExpr (NameNode)
 *   atom: property name being accessed
 *   privateNameKind: kind of the name if private
 * DotExpr (PropertyAccess)
 *   left: MEMBER expr to left of '.'
 *   right: PropertyName to right of '.'
 * OptionalDotExpr (OptionalPropertyAccess)
 *   left: MEMBER expr to left of '.', short circuits back to OptionalChain
 *        if nullish.
 *   right: PropertyName to right of '.'
 * ElemExpr (PropertyByValue)
 *   left: MEMBER expr to left of '['
 *   right: expr between '[' and ']'
 * OptionalElemExpr (OptionalPropertyByValue)
 *   left: MEMBER expr to left of '[', short circuits back to OptionalChain
 *         if nullish.
 *   right: expr between '[' and ']'
 * CallExpr (BinaryNode)
 *   left: callee expression on the left of the '('
 *   right: Arguments
 * OptionalCallExpr (BinaryNode)
 *   left: callee expression on the left of the '(', short circuits back to
 *         OptionalChain if nullish.
 *   right: Arguments
 * Arguments (ListNode)
 *   head: list of arg1, arg2, ... argN
 *   count: N >= 0
 * ArrayExpr (ListNode)
 *   head: list of N array element expressions
 *         holes ([,,]) are represented by Elision nodes,
 *         spread elements ([...X]) are represented by Spread nodes
 *   count: N >= 0
 * ObjectExpr (ListNode)
 *   head: list of N nodes, each item is one of:
 *           * MutateProto
 *           * PropertyDefinition
 *           * Shorthand
 *           * Spread
 *   count: N >= 0
 * PropertyDefinition (PropertyDefinition)
 *   key-value pair in object initializer or destructuring lhs
 *   left: property id
 *   right: value
 * Shorthand (BinaryNode)
 *   Same fields as PropertyDefinition. This is used for object literal
 *   properties using shorthand ({x}).
 * ComputedName (UnaryNode)
 *   ES6 ComputedPropertyName.
 *   kid: the AssignmentExpression inside the square brackets
 * Name (NameNode)
 *   atom: name, or object atom
 * StringExpr (NameNode)
 *   atom: string
 * TemplateStringListExpr (ListNode)
 *   head: list of alternating expr and template strings
 *           TemplateString [, expression, TemplateString]+
 *         there's at least one expression.  If the template literal contains
 *         no ${}-delimited expression, it's parsed as a single TemplateString
 * TemplateStringExpr (NameNode)
 *   atom: template string atom
 * TaggedTemplateExpr (BinaryNode)
 *   left: tag expression
 *   right: Arguments, with the first being the call site object, then
 *          arg1, arg2, ... argN
 * CallSiteObj (CallSiteNode)
 *   head:  an Array of raw TemplateString, then corresponding cooked
 *          TemplateString nodes
 *            Array [, cooked TemplateString]+
 *          where the Array is
 *            [raw TemplateString]+
 * RegExpExpr (RegExpLiteral)
 *   regexp: RegExp model object
 * NumberExpr (NumericLiteral)
 *   value: double value of numeric literal
 * BigIntExpr (BigIntLiteral)
 *   stencil: script compilation struct that has |bigIntData| vector
 *   index: index into the script compilation's |bigIntData| vector
 * TrueExpr, FalseExpr (BooleanLiteral)
 * NullExpr (NullLiteral)
 * RawUndefinedExpr (RawUndefinedLiteral)
 *
 * ThisExpr (UnaryNode)
 *   kid: '.this' Name if function `this`, else nullptr
 * SuperBase (UnaryNode)
 *   kid: '.this' Name
 * SuperCallExpr (BinaryNode)
 *   left: SuperBase
 *   right: Arguments
 * SetThis (BinaryNode)
 *   left: '.this' Name
 *   right: SuperCall
 *
 * LexicalScope (LexicalScopeNode)
 *   scopeBindings: scope bindings
 *   scopeBody: scope body
 * Generator (NullaryNode)
 * InitialYield (UnaryNode)
 *   kid: generator object
 * YieldExpr, YieldStarExpr, AwaitExpr (UnaryNode)
 *   kid: expr or null
 */

// FIXME: Remove `*Type` (bug 1489008)
#define FOR_EACH_PARSENODE_SUBCLASS(MACRO)                                   \
  MACRO(BinaryNode, BinaryNodeType, asBinary)                                \
  MACRO(AssignmentNode, AssignmentNodeType, asAssignment)                    \
  MACRO(CaseClause, CaseClauseType, asCaseClause)                            \
  MACRO(ClassMethod, ClassMethodType, asClassMethod)                         \
  MACRO(ClassField, ClassFieldType, asClassField)                            \
  MACRO(StaticClassBlock, StaticClassBlockType, asStaticClassBlock)          \
  MACRO(PropertyDefinition, PropertyDefinitionType, asPropertyDefinition)    \
  MACRO(ClassNames, ClassNamesType, asClassNames)                            \
  MACRO(ForNode, ForNodeType, asFor)                                         \
  MACRO(PropertyAccess, PropertyAccessType, asPropertyAccess)                \
  MACRO(OptionalPropertyAccess, OptionalPropertyAccessType,                  \
        asOptionalPropertyAccess)                                            \
  MACRO(PropertyByValue, PropertyByValueType, asPropertyByValue)             \
  MACRO(OptionalPropertyByValue, OptionalPropertyByValueType,                \
        asOptionalPropertyByValue)                                           \
  MACRO(PrivateMemberAccess, PrivateMemberAccessType, asPrivateMemberAccess) \
  MACRO(OptionalPrivateMemberAccess, OptionalPrivateMemberAccessType,        \
        asOptionalPrivateMemberAccess)                                       \
  MACRO(SwitchStatement, SwitchStatementType, asSwitchStatement)             \
                                                                             \
  MACRO(FunctionNode, FunctionNodeType, asFunction)                          \
  MACRO(ModuleNode, ModuleNodeType, asModule)                                \
                                                                             \
  MACRO(LexicalScopeNode, LexicalScopeNodeType, asLexicalScope)              \
  MACRO(ClassBodyScopeNode, ClassBodyScopeNodeType, asClassBodyScope)        \
                                                                             \
  MACRO(ListNode, ListNodeType, asList)                                      \
  MACRO(CallSiteNode, CallSiteNodeType, asCallSite)                          \
  MACRO(CallNode, CallNodeType, asCallNode)                                  \
  MACRO(CallNode, OptionalCallNodeType, asOptionalCallNode)                  \
                                                                             \
  MACRO(LoopControlStatement, LoopControlStatementType,                      \
        asLoopControlStatement)                                              \
  MACRO(BreakStatement, BreakStatementType, asBreakStatement)                \
  MACRO(ContinueStatement, ContinueStatementType, asContinueStatement)       \
                                                                             \
  MACRO(NameNode, NameNodeType, asName)                                      \
  MACRO(LabeledStatement, LabeledStatementType, asLabeledStatement)          \
                                                                             \
  MACRO(NullaryNode, NullaryNodeType, asNullary)                             \
  MACRO(BooleanLiteral, BooleanLiteralType, asBooleanLiteral)                \
  MACRO(DebuggerStatement, DebuggerStatementType, asDebuggerStatement)       \
  MACRO(NullLiteral, NullLiteralType, asNullLiteral)                         \
  MACRO(RawUndefinedLiteral, RawUndefinedLiteralType, asRawUndefinedLiteral) \
                                                                             \
  MACRO(NumericLiteral, NumericLiteralType, asNumericLiteral)                \
  MACRO(BigIntLiteral, BigIntLiteralType, asBigIntLiteral)                   \
                                                                             \
  MACRO(RegExpLiteral, RegExpLiteralType, asRegExpLiteral)                   \
                                                                             \
  MACRO(TernaryNode, TernaryNodeType, asTernary)                             \
  MACRO(ClassNode, ClassNodeType, asClass)                                   \
  MACRO(ConditionalExpression, ConditionalExpressionType,                    \
        asConditionalExpression)                                             \
  MACRO(TryNode, TryNodeType, asTry)                                         \
                                                                             \
  MACRO(UnaryNode, UnaryNodeType, asUnary)                                   \
  MACRO(ThisLiteral, ThisLiteralType, asThisLiteral)

#define DECLARE_CLASS(typeName, longTypeName, asMethodName) class typeName;
FOR_EACH_PARSENODE_SUBCLASS(DECLARE_CLASS)
#undef DECLARE_CLASS

enum class AccessorType { None, Getter, Setter };

static inline bool IsConstructorKind(FunctionSyntaxKind kind) {
  return kind == FunctionSyntaxKind::ClassConstructor ||
         kind == FunctionSyntaxKind::DerivedClassConstructor;
}

static inline bool IsMethodDefinitionKind(FunctionSyntaxKind kind) {
  return IsConstructorKind(kind) || kind == FunctionSyntaxKind::Method ||
         kind == FunctionSyntaxKind::FieldInitializer ||
         kind == FunctionSyntaxKind::Getter ||
         kind == FunctionSyntaxKind::Setter;
}

// To help diagnose sporadic crashes in the frontend, a few assertions are
// enabled in early beta builds. (Most are not; those still use MOZ_ASSERT.)
// See bug 1547561.
#if defined(EARLY_BETA_OR_EARLIER)
#  define JS_PARSE_NODE_ASSERT MOZ_RELEASE_ASSERT
#else
#  define JS_PARSE_NODE_ASSERT MOZ_ASSERT
#endif

class ParseNode {
  const ParseNodeKind pn_type; /* ParseNodeKind::PNK_* type */

  bool pn_parens : 1;       /* this expr was enclosed in parens */
  bool pn_rhs_anon_fun : 1; /* this expr is anonymous function or class that
                             * is a direct RHS of ParseNodeKind::Assign or
                             * ParseNodeKind::PropertyDefinition of property,
                             * that needs SetFunctionName. */

 protected:
  // Used by ComputedName to indicate if the ComputedName is a
  // a synthetic construct. This allows us to avoid needing to
  // compute ToString on uncommon property values such as BigInt.
  // Instead we parse as though they were computed names.
  //
  // We need this bit to distinguish a synthetic computed name like
  // this however to undo this transformation in Reflect.parse and
  // name guessing.
  bool pn_synthetic_computed : 1;

  ParseNode(const ParseNode& other) = delete;
  void operator=(const ParseNode& other) = delete;

 public:
  explicit ParseNode(ParseNodeKind kind)
      : pn_type(kind),
        pn_parens(false),
        pn_rhs_anon_fun(false),
        pn_synthetic_computed(false),
        pn_pos(0, 0),
        pn_next(nullptr) {
    JS_PARSE_NODE_ASSERT(ParseNodeKind::Start <= kind);
    JS_PARSE_NODE_ASSERT(kind < ParseNodeKind::Limit);
  }

  ParseNode(ParseNodeKind kind, const TokenPos& pos)
      : pn_type(kind),
        pn_parens(false),
        pn_rhs_anon_fun(false),
        pn_synthetic_computed(false),
        pn_pos(pos),
        pn_next(nullptr) {
    JS_PARSE_NODE_ASSERT(ParseNodeKind::Start <= kind);
    JS_PARSE_NODE_ASSERT(kind < ParseNodeKind::Limit);
  }

  ParseNodeKind getKind() const {
    JS_PARSE_NODE_ASSERT(ParseNodeKind::Start <= pn_type);
    JS_PARSE_NODE_ASSERT(pn_type < ParseNodeKind::Limit);
    return pn_type;
  }
  bool isKind(ParseNodeKind kind) const { return getKind() == kind; }

 protected:
  size_t getKindAsIndex() const {
    return size_t(getKind()) - size_t(ParseNodeKind::Start);
  }

  // Used to implement test() on a few ParseNodes efficiently.
  // (This enum doesn't fully reflect the ParseNode class hierarchy,
  // so don't use it for anything else.)
  enum class TypeCode : uint8_t {
    Nullary,
    Unary,
    Binary,
    Ternary,
    List,
    Name,
    Other
  };

  // typeCodeTable[getKindAsIndex()] is the type code of a ParseNode of kind
  // pnk.
  static const TypeCode typeCodeTable[];

 private:
#ifdef DEBUG
  static const size_t sizeTable[];
#endif

 public:
  TypeCode typeCode() const { return typeCodeTable[getKindAsIndex()]; }

  bool isBinaryOperation() const {
    ParseNodeKind kind = getKind();
    return ParseNodeKind::BinOpFirst <= kind &&
           kind <= ParseNodeKind::BinOpLast;
  }
  inline bool isName(TaggedParserAtomIndex name) const;

  /* Boolean attributes. */
  bool isInParens() const { return pn_parens; }
  bool isLikelyIIFE() const { return isInParens(); }
  void setInParens(bool enabled) { pn_parens = enabled; }

  bool isDirectRHSAnonFunction() const { return pn_rhs_anon_fun; }
  void setDirectRHSAnonFunction(bool enabled) { pn_rhs_anon_fun = enabled; }

  TokenPos pn_pos;    /* two 16-bit pairs here, for 64 bits */
  ParseNode* pn_next; /* intrinsic link in parent PN_LIST */

 public:
  /*
   * If |left| is a list of the given kind/left-associative op, append
   * |right| to it and return |left|.  Otherwise return a [left, right] list.
   */
  static ParseNode* appendOrCreateList(ParseNodeKind kind, ParseNode* left,
                                       ParseNode* right,
                                       FullParseHandler* handler,
                                       ParseContext* pc);

  /* True if pn is a parsenode representing a literal constant. */
  bool isLiteral() const {
    return isKind(ParseNodeKind::NumberExpr) ||
           isKind(ParseNodeKind::BigIntExpr) ||
           isKind(ParseNodeKind::StringExpr) ||
           isKind(ParseNodeKind::TrueExpr) ||
           isKind(ParseNodeKind::FalseExpr) ||
           isKind(ParseNodeKind::NullExpr) ||
           isKind(ParseNodeKind::RawUndefinedExpr);
  }

  // True iff this is a for-in/of loop variable declaration (var/let/const).
  inline bool isForLoopDeclaration() const;

  inline bool isConstant();

  template <class NodeType>
  inline bool is() const {
    return NodeType::test(*this);
  }

  /* Casting operations. */
  template <class NodeType>
  inline NodeType& as() {
    MOZ_ASSERT(NodeType::test(*this));
    return *static_cast<NodeType*>(this);
  }

  template <class NodeType>
  inline const NodeType& as() const {
    MOZ_ASSERT(NodeType::test(*this));
    return *static_cast<const NodeType*>(this);
  }

#ifdef DEBUG
  // Debugger-friendly stderr printer.
  void dump();
  void dump(ParserBase* parser);
  void dump(ParserBase* parser, GenericPrinter& out);
  void dump(ParserBase* parser, GenericPrinter& out, int indent);

  // The size of this node, in bytes.
  size_t size() const { return sizeTable[getKindAsIndex()]; }
#endif
};

// Remove a ParseNode, **pnp, from a parse tree, putting another ParseNode,
// *pn, in its place.
//
// pnp points to a ParseNode pointer. This must be the only pointer that points
// to the parse node being replaced. The replacement, *pn, is unchanged except
// for its pn_next pointer; updating that is necessary if *pn's new parent is a
// list node.
inline void ReplaceNode(ParseNode** pnp, ParseNode* pn) {
  pn->pn_next = (*pnp)->pn_next;
  *pnp = pn;
}

class NullaryNode : public ParseNode {
 public:
  NullaryNode(ParseNodeKind kind, const TokenPos& pos) : ParseNode(kind, pos) {
    MOZ_ASSERT(is<NullaryNode>());
  }

  static bool test(const ParseNode& node) {
    return node.typeCode() == TypeCode::Nullary;
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::Nullary; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    return true;
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif
};

class NameNode : public ParseNode {
  TaggedParserAtomIndex atom_; /* lexical name or label atom */
  PrivateNameKind privateNameKind_ = PrivateNameKind::None;

 public:
  NameNode(ParseNodeKind kind, TaggedParserAtomIndex atom, const TokenPos& pos)
      : ParseNode(kind, pos), atom_(atom) {
    MOZ_ASSERT(atom);
    MOZ_ASSERT(is<NameNode>());
  }

  static bool test(const ParseNode& node) {
    return node.typeCode() == TypeCode::Name;
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::Name; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    return true;
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  TaggedParserAtomIndex atom() const { return atom_; }

  TaggedParserAtomIndex name() const {
    MOZ_ASSERT(isKind(ParseNodeKind::Name) ||
               isKind(ParseNodeKind::PrivateName));
    return atom_;
  }

  void setAtom(TaggedParserAtomIndex atom) { atom_ = atom; }

  void setPrivateNameKind(PrivateNameKind privateNameKind) {
    privateNameKind_ = privateNameKind;
  }

  PrivateNameKind privateNameKind() { return privateNameKind_; }
};

inline bool ParseNode::isName(TaggedParserAtomIndex name) const {
  return getKind() == ParseNodeKind::Name && as<NameNode>().name() == name;
}

class UnaryNode : public ParseNode {
  ParseNode* kid_;

 public:
  UnaryNode(ParseNodeKind kind, const TokenPos& pos, ParseNode* kid)
      : ParseNode(kind, pos), kid_(kid) {
    MOZ_ASSERT(is<UnaryNode>());
  }

  static bool test(const ParseNode& node) {
    return node.typeCode() == TypeCode::Unary;
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::Unary; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    if (kid_) {
      if (!visitor.visit(kid_)) {
        return false;
      }
    }
    return true;
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  ParseNode* kid() const { return kid_; }

  /*
   * Non-null if this is a statement node which could be a member of a
   * Directive Prologue: an expression statement consisting of a single
   * string literal.
   *
   * This considers only the node and its children, not its context. After
   * parsing, check the node's prologue flag to see if it is indeed part of
   * a directive prologue.
   *
   * Note that a Directive Prologue can contain statements that cannot
   * themselves be directives (string literals that include escape sequences
   * or escaped newlines, say). This member function returns true for such
   * nodes; we use it to determine the extent of the prologue.
   */
  TaggedParserAtomIndex isStringExprStatement() const {
    if (isKind(ParseNodeKind::ExpressionStmt)) {
      if (kid()->isKind(ParseNodeKind::StringExpr) && !kid()->isInParens()) {
        return kid()->as<NameNode>().atom();
      }
    }
    return TaggedParserAtomIndex::null();
  }

  // Methods used by FoldConstants.cpp.
  ParseNode** unsafeKidReference() { return &kid_; }

  void setSyntheticComputedName() { pn_synthetic_computed = true; }
  bool isSyntheticComputedName() {
    MOZ_ASSERT(isKind(ParseNodeKind::ComputedName));
    return pn_synthetic_computed;
  }
};

class BinaryNode : public ParseNode {
  ParseNode* left_;
  ParseNode* right_;

 public:
  BinaryNode(ParseNodeKind kind, const TokenPos& pos, ParseNode* left,
             ParseNode* right)
      : ParseNode(kind, pos), left_(left), right_(right) {
    MOZ_ASSERT(is<BinaryNode>());
  }

  BinaryNode(ParseNodeKind kind, ParseNode* left, ParseNode* right)
      : ParseNode(kind, TokenPos::box(left->pn_pos, right->pn_pos)),
        left_(left),
        right_(right) {
    MOZ_ASSERT(is<BinaryNode>());
  }

  static bool test(const ParseNode& node) {
    return node.typeCode() == TypeCode::Binary;
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::Binary; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    if (left_) {
      if (!visitor.visit(left_)) {
        return false;
      }
    }
    if (right_) {
      if (!visitor.visit(right_)) {
        return false;
      }
    }
    return true;
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  ParseNode* left() const { return left_; }

  ParseNode* right() const { return right_; }

  // Methods used by FoldConstants.cpp.
  // callers are responsible for keeping the list consistent.
  ParseNode** unsafeLeftReference() { return &left_; }

  ParseNode** unsafeRightReference() { return &right_; }
};

class AssignmentNode : public BinaryNode {
 public:
  AssignmentNode(ParseNodeKind kind, ParseNode* left, ParseNode* right)
      : BinaryNode(kind, TokenPos(left->pn_pos.begin, right->pn_pos.end), left,
                   right) {}

  static bool test(const ParseNode& node) {
    ParseNodeKind kind = node.getKind();
    bool match = ParseNodeKind::AssignmentStart <= kind &&
                 kind <= ParseNodeKind::AssignmentLast;
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    return match;
  }
};

class ForNode : public BinaryNode {
  unsigned iflags_; /* JSITER_* flags */

 public:
  ForNode(const TokenPos& pos, ParseNode* forHead, ParseNode* body,
          unsigned iflags)
      : BinaryNode(ParseNodeKind::ForStmt, pos, forHead, body),
        iflags_(iflags) {
    MOZ_ASSERT(forHead->isKind(ParseNodeKind::ForIn) ||
               forHead->isKind(ParseNodeKind::ForOf) ||
               forHead->isKind(ParseNodeKind::ForHead));
  }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::ForStmt);
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    return match;
  }

  TernaryNode* head() const { return &left()->as<TernaryNode>(); }

  ParseNode* body() const { return right(); }

  unsigned iflags() const { return iflags_; }
};

class TernaryNode : public ParseNode {
  ParseNode* kid1_; /* condition, discriminant, etc. */
  ParseNode* kid2_; /* then-part, case list, etc. */
  ParseNode* kid3_; /* else-part, default case, etc. */

 public:
  TernaryNode(ParseNodeKind kind, ParseNode* kid1, ParseNode* kid2,
              ParseNode* kid3)
      : TernaryNode(kind, kid1, kid2, kid3,
                    TokenPos((kid1   ? kid1
                              : kid2 ? kid2
                                     : kid3)
                                 ->pn_pos.begin,
                             (kid3   ? kid3
                              : kid2 ? kid2
                                     : kid1)
                                 ->pn_pos.end)) {}

  TernaryNode(ParseNodeKind kind, ParseNode* kid1, ParseNode* kid2,
              ParseNode* kid3, const TokenPos& pos)
      : ParseNode(kind, pos), kid1_(kid1), kid2_(kid2), kid3_(kid3) {
    MOZ_ASSERT(is<TernaryNode>());
  }

  static bool test(const ParseNode& node) {
    return node.typeCode() == TypeCode::Ternary;
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::Ternary; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    if (kid1_) {
      if (!visitor.visit(kid1_)) {
        return false;
      }
    }
    if (kid2_) {
      if (!visitor.visit(kid2_)) {
        return false;
      }
    }
    if (kid3_) {
      if (!visitor.visit(kid3_)) {
        return false;
      }
    }
    return true;
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  ParseNode* kid1() const { return kid1_; }

  ParseNode* kid2() const { return kid2_; }

  ParseNode* kid3() const { return kid3_; }

  // Methods used by FoldConstants.cpp.
  ParseNode** unsafeKid1Reference() { return &kid1_; }

  ParseNode** unsafeKid2Reference() { return &kid2_; }

  ParseNode** unsafeKid3Reference() { return &kid3_; }
};

class ListNode : public ParseNode {
  ParseNode* head_;  /* first node in list */
  ParseNode** tail_; /* ptr to last node's pn_next in list */
  uint32_t count_;   /* number of nodes in list */
  uint32_t xflags;

 private:
  // xflags bits.

  // Statement list has top-level function statements.
  static constexpr uint32_t hasTopLevelFunctionDeclarationsBit = Bit(0);

  // Array/Object/Class initializer has non-constants.
  //   * array has holes
  //   * array has spread node
  //   * array has element which is known not to be constant
  //   * array has no element
  //   * object/class has __proto__
  //   * object/class has property which is known not to be constant
  //   * object/class shorthand property
  //   * object/class spread property
  //   * object/class has method
  //   * object/class has computed property
  static constexpr uint32_t hasNonConstInitializerBit = Bit(1);

  // Flag set by the emitter after emitting top-level function statements.
  static constexpr uint32_t emittedTopLevelFunctionDeclarationsBit = Bit(2);

 public:
  ListNode(ParseNodeKind kind, const TokenPos& pos) : ParseNode(kind, pos) {
    makeEmpty();
    MOZ_ASSERT(is<ListNode>());
  }

  ListNode(ParseNodeKind kind, ParseNode* kid)
      : ParseNode(kind, kid->pn_pos),
        head_(kid),
        tail_(&kid->pn_next),
        count_(1),
        xflags(0) {
    if (kid->pn_pos.begin < pn_pos.begin) {
      pn_pos.begin = kid->pn_pos.begin;
    }
    pn_pos.end = kid->pn_pos.end;

    MOZ_ASSERT(is<ListNode>());
  }

  static bool test(const ParseNode& node) {
    return node.typeCode() == TypeCode::List;
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::List; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    ParseNode** listp = &head_;
    for (; *listp; listp = &(*listp)->pn_next) {
      // Don't use PN*& because we want to check if it changed, so we can use
      // ReplaceNode
      ParseNode* pn = *listp;
      if (!visitor.visit(pn)) {
        return false;
      }
      if (pn != *listp) {
        ReplaceNode(listp, pn);
      }
    }
    unsafeReplaceTail(listp);
    return true;
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  ParseNode* head() const { return head_; }

  ParseNode** tail() const { return tail_; }

  uint32_t count() const { return count_; }

  bool empty() const { return count() == 0; }

  void checkConsistency() const
#ifndef DEBUG
  {
  }
#endif
  ;

  [[nodiscard]] bool hasTopLevelFunctionDeclarations() const {
    MOZ_ASSERT(isKind(ParseNodeKind::StatementList));
    return xflags & hasTopLevelFunctionDeclarationsBit;
  }

  [[nodiscard]] bool emittedTopLevelFunctionDeclarations() const {
    MOZ_ASSERT(isKind(ParseNodeKind::StatementList));
    MOZ_ASSERT(hasTopLevelFunctionDeclarations());
    return xflags & emittedTopLevelFunctionDeclarationsBit;
  }

  [[nodiscard]] bool hasNonConstInitializer() const {
    MOZ_ASSERT(isKind(ParseNodeKind::ArrayExpr) ||
               isKind(ParseNodeKind::ObjectExpr));
    return xflags & hasNonConstInitializerBit;
  }

  void setHasTopLevelFunctionDeclarations() {
    MOZ_ASSERT(isKind(ParseNodeKind::StatementList));
    xflags |= hasTopLevelFunctionDeclarationsBit;
  }

  void setEmittedTopLevelFunctionDeclarations() {
    MOZ_ASSERT(isKind(ParseNodeKind::StatementList));
    MOZ_ASSERT(hasTopLevelFunctionDeclarations());
    xflags |= emittedTopLevelFunctionDeclarationsBit;
  }

  void setHasNonConstInitializer() {
    MOZ_ASSERT(isKind(ParseNodeKind::ArrayExpr) ||
               isKind(ParseNodeKind::ObjectExpr));
    xflags |= hasNonConstInitializerBit;
  }

  void unsetHasNonConstInitializer() {
    MOZ_ASSERT(isKind(ParseNodeKind::ArrayExpr) ||
               isKind(ParseNodeKind::ObjectExpr));
    xflags &= ~hasNonConstInitializerBit;
  }

  /*
   * Compute a pointer to the last element in a singly-linked list. NB: list
   * must be non-empty -- this is asserted!
   */
  ParseNode* last() const {
    MOZ_ASSERT(!empty());
    //
    // ParseNode                      ParseNode
    // +-----+---------+-----+        +-----+---------+-----+
    // | ... | pn_next | ... | +-...->| ... | pn_next | ... |
    // +-----+---------+-----+ |      +-----+---------+-----+
    // ^       |               |      ^     ^
    // |       +---------------+      |     |
    // |                              |     tail()
    // |                              |
    // head()                         last()
    //
    return (ParseNode*)(uintptr_t(tail()) - offsetof(ParseNode, pn_next));
  }

  void replaceLast(ParseNode* node) {
    MOZ_ASSERT(!empty());
    pn_pos.end = node->pn_pos.end;

    ParseNode* item = head();
    ParseNode* lastNode = last();
    MOZ_ASSERT(item);
    if (item == lastNode) {
      head_ = node;
    } else {
      while (item->pn_next != lastNode) {
        MOZ_ASSERT(item->pn_next);
        item = item->pn_next;
      }
      item->pn_next = node;
    }
    tail_ = &node->pn_next;
  }

  void makeEmpty() {
    head_ = nullptr;
    tail_ = &head_;
    count_ = 0;
    xflags = 0;
  }

  void append(ParseNode* item) {
    MOZ_ASSERT(item->pn_pos.begin >= pn_pos.begin);
    pn_pos.end = item->pn_pos.end;
    *tail_ = item;
    tail_ = &item->pn_next;
    count_++;
  }

  void prepend(ParseNode* item) {
    item->pn_next = head_;
    head_ = item;
    if (tail_ == &head_) {
      tail_ = &item->pn_next;
    }
    count_++;
  }

  void prependAndUpdatePos(ParseNode* item) {
    prepend(item);
    pn_pos.begin = item->pn_pos.begin;
  }

  // Methods used by FoldConstants.cpp.
  // Caller is responsible for keeping the list consistent.
  ParseNode** unsafeHeadReference() { return &head_; }

  void unsafeReplaceTail(ParseNode** newTail) {
    tail_ = newTail;
    checkConsistency();
  }

  void unsafeDecrementCount() {
    MOZ_ASSERT(count() > 1);
    count_--;
  }

 private:
  // Classes to iterate over ListNode contents:
  //
  // Usage:
  //   ListNode* list;
  //   for (ParseNode* item : list->contents()) {
  //     // item is ParseNode* typed.
  //   }
  class iterator {
   private:
    ParseNode* node_;

    friend class ListNode;
    explicit iterator(ParseNode* node) : node_(node) {}

   public:
    // Implement std::iterator_traits.
    using iterator_category = std::input_iterator_tag;
    using value_type = ParseNode*;
    using difference_type = ptrdiff_t;
    using pointer = ParseNode**;
    using reference = ParseNode*&;

    bool operator==(const iterator& other) const {
      return node_ == other.node_;
    }

    bool operator!=(const iterator& other) const { return !(*this == other); }

    iterator& operator++() {
      node_ = node_->pn_next;
      return *this;
    }

    ParseNode* operator*() { return node_; }

    const ParseNode* operator*() const { return node_; }
  };

  class range {
   private:
    ParseNode* begin_;
    ParseNode* end_;

    friend class ListNode;
    range(ParseNode* begin, ParseNode* end) : begin_(begin), end_(end) {}

   public:
    iterator begin() { return iterator(begin_); }

    iterator end() { return iterator(end_); }

    const iterator begin() const { return iterator(begin_); }

    const iterator end() const { return iterator(end_); }

    const iterator cbegin() const { return begin(); }

    const iterator cend() const { return end(); }
  };

#ifdef DEBUG
  [[nodiscard]] bool contains(ParseNode* target) const {
    MOZ_ASSERT(target);
    for (ParseNode* node : contents()) {
      if (target == node) {
        return true;
      }
    }
    return false;
  }
#endif

 public:
  range contents() { return range(head(), nullptr); }

  const range contents() const { return range(head(), nullptr); }

  range contentsFrom(ParseNode* begin) {
    MOZ_ASSERT_IF(begin, contains(begin));
    return range(begin, nullptr);
  }

  const range contentsFrom(ParseNode* begin) const {
    MOZ_ASSERT_IF(begin, contains(begin));
    return range(begin, nullptr);
  }

  range contentsTo(ParseNode* end) {
    MOZ_ASSERT_IF(end, contains(end));
    return range(head(), end);
  }

  const range contentsTo(ParseNode* end) const {
    MOZ_ASSERT_IF(end, contains(end));
    return range(head(), end);
  }
};

inline bool ParseNode::isForLoopDeclaration() const {
  if (isKind(ParseNodeKind::VarStmt) || isKind(ParseNodeKind::LetDecl) ||
      isKind(ParseNodeKind::ConstDecl)) {
    MOZ_ASSERT(!as<ListNode>().empty());
    return true;
  }

  return false;
}

class FunctionNode : public ParseNode {
  FunctionBox* funbox_;
  ParseNode* body_;
  FunctionSyntaxKind syntaxKind_;

 public:
  FunctionNode(FunctionSyntaxKind syntaxKind, const TokenPos& pos)
      : ParseNode(ParseNodeKind::Function, pos),
        funbox_(nullptr),
        body_(nullptr),
        syntaxKind_(syntaxKind) {
    MOZ_ASSERT(!body_);
    MOZ_ASSERT(!funbox_);
    MOZ_ASSERT(is<FunctionNode>());
  }

  static bool test(const ParseNode& node) {
    return node.isKind(ParseNodeKind::Function);
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::Other; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    // Note: body is null for lazily-parsed functions.
    if (body_) {
      if (!visitor.visit(body_)) {
        return false;
      }
    }
    return true;
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  FunctionBox* funbox() const { return funbox_; }

  ListNode* body() const { return body_ ? &body_->as<ListNode>() : nullptr; }

  void setFunbox(FunctionBox* funbox) { funbox_ = funbox; }

  void setBody(ListNode* body) { body_ = body; }

  FunctionSyntaxKind syntaxKind() const { return syntaxKind_; }

  bool functionIsHoisted() const {
    return syntaxKind() == FunctionSyntaxKind::Statement;
  }
};

class ModuleNode : public ParseNode {
  ParseNode* body_;

 public:
  explicit ModuleNode(const TokenPos& pos)
      : ParseNode(ParseNodeKind::Module, pos), body_(nullptr) {
    MOZ_ASSERT(!body_);
    MOZ_ASSERT(is<ModuleNode>());
  }

  static bool test(const ParseNode& node) {
    return node.isKind(ParseNodeKind::Module);
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::Other; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    return visitor.visit(body_);
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  ListNode* body() const { return &body_->as<ListNode>(); }

  void setBody(ListNode* body) { body_ = body; }
};

class NumericLiteral : public ParseNode {
  double value_;              /* aligned numeric literal value */
  DecimalPoint decimalPoint_; /* Whether the number has a decimal point */

 public:
  NumericLiteral(double value, DecimalPoint decimalPoint, const TokenPos& pos)
      : ParseNode(ParseNodeKind::NumberExpr, pos),
        value_(value),
        decimalPoint_(decimalPoint) {}

  static bool test(const ParseNode& node) {
    return node.isKind(ParseNodeKind::NumberExpr);
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::Other; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    return true;
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  double value() const { return value_; }

  DecimalPoint decimalPoint() const { return decimalPoint_; }

  void setValue(double v) { value_ = v; }

  void setDecimalPoint(DecimalPoint d) { decimalPoint_ = d; }

  // Return the decimal string representation of this numeric literal.
  TaggedParserAtomIndex toAtom(JSContext* cx,
                               ParserAtomsTable& parserAtoms) const;
};

class BigIntLiteral : public ParseNode {
  BigIntIndex index_;
  bool isZero_;

 public:
  BigIntLiteral(BigIntIndex index, bool isZero, const TokenPos& pos)
      : ParseNode(ParseNodeKind::BigIntExpr, pos),
        index_(index),
        isZero_(isZero) {}

  static bool test(const ParseNode& node) {
    return node.isKind(ParseNodeKind::BigIntExpr);
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::Other; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    return true;
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  BigIntIndex index() { return index_; }

  bool isZero() const { return isZero_; }
};

template <ParseNodeKind NodeKind, typename ScopeType>
class BaseScopeNode : public ParseNode {
  using ParserData = typename ScopeType::ParserData;
  ParserData* bindings;
  ParseNode* body;
  ScopeKind kind_;

 public:
  BaseScopeNode(ParserData* bindings, ParseNode* body,
                ScopeKind kind = ScopeKind::Lexical)
      : ParseNode(NodeKind, body->pn_pos),
        bindings(bindings),
        body(body),
        kind_(kind) {}

  static bool test(const ParseNode& node) { return node.isKind(NodeKind); }

  static constexpr TypeCode classTypeCode() { return TypeCode::Other; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    return visitor.visit(body);
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  ParserData* scopeBindings() const {
    MOZ_ASSERT(!isEmptyScope());
    return bindings;
  }

  ParseNode* scopeBody() const { return body; }

  void setScopeBody(ParseNode* body) { this->body = body; }

  bool isEmptyScope() const { return !bindings; }

  ScopeKind kind() const { return kind_; }
};

class LexicalScopeNode
    : public BaseScopeNode<ParseNodeKind::LexicalScope, LexicalScope> {
 public:
  LexicalScopeNode(LexicalScope::ParserData* bindings, ParseNode* body,
                   ScopeKind kind = ScopeKind::Lexical)
      : BaseScopeNode(bindings, body, kind) {}
};

class ClassBodyScopeNode
    : public BaseScopeNode<ParseNodeKind::ClassBodyScope, ClassBodyScope> {
 public:
  ClassBodyScopeNode(ClassBodyScope::ParserData* bindings, ListNode* memberList)
      : BaseScopeNode(bindings, memberList, ScopeKind::ClassBody) {
    MOZ_ASSERT(memberList->isKind(ParseNodeKind::ClassMemberList));
  }

  ListNode* memberList() const {
    ListNode* list = &scopeBody()->as<ListNode>();
    MOZ_ASSERT(list->isKind(ParseNodeKind::ClassMemberList));
    return list;
  }
};

class LabeledStatement : public NameNode {
  ParseNode* statement_;

 public:
  LabeledStatement(TaggedParserAtomIndex label, ParseNode* stmt, uint32_t begin)
      : NameNode(ParseNodeKind::LabelStmt, label,
                 TokenPos(begin, stmt->pn_pos.end)),
        statement_(stmt) {}

  TaggedParserAtomIndex label() const { return atom(); }

  ParseNode* statement() const { return statement_; }

  static bool test(const ParseNode& node) {
    return node.isKind(ParseNodeKind::LabelStmt);
  }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    if (statement_) {
      if (!visitor.visit(statement_)) {
        return false;
      }
    }
    return true;
  }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif
};

// Inside a switch statement, a CaseClause is a case-label and the subsequent
// statements. The same node type is used for DefaultClauses. The only
// difference is that their caseExpression() is null.
class CaseClause : public BinaryNode {
 public:
  CaseClause(ParseNode* expr, ParseNode* stmts, uint32_t begin)
      : BinaryNode(ParseNodeKind::Case, TokenPos(begin, stmts->pn_pos.end),
                   expr, stmts) {}

  ParseNode* caseExpression() const { return left(); }

  bool isDefault() const { return !caseExpression(); }

  ListNode* statementList() const { return &right()->as<ListNode>(); }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::Case);
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    return match;
  }
};

class LoopControlStatement : public ParseNode {
  TaggedParserAtomIndex label_; /* target of break/continue statement */

 protected:
  LoopControlStatement(ParseNodeKind kind, TaggedParserAtomIndex label,
                       const TokenPos& pos)
      : ParseNode(kind, pos), label_(label) {
    MOZ_ASSERT(kind == ParseNodeKind::BreakStmt ||
               kind == ParseNodeKind::ContinueStmt);
    MOZ_ASSERT(is<LoopControlStatement>());
  }

 public:
  /* Label associated with this break/continue statement, if any. */
  TaggedParserAtomIndex label() const { return label_; }

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  static bool test(const ParseNode& node) {
    return node.isKind(ParseNodeKind::BreakStmt) ||
           node.isKind(ParseNodeKind::ContinueStmt);
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::Other; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    return true;
  }
};

class BreakStatement : public LoopControlStatement {
 public:
  BreakStatement(TaggedParserAtomIndex label, const TokenPos& pos)
      : LoopControlStatement(ParseNodeKind::BreakStmt, label, pos) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::BreakStmt);
    MOZ_ASSERT_IF(match, node.is<LoopControlStatement>());
    return match;
  }
};

class ContinueStatement : public LoopControlStatement {
 public:
  ContinueStatement(TaggedParserAtomIndex label, const TokenPos& pos)
      : LoopControlStatement(ParseNodeKind::ContinueStmt, label, pos) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::ContinueStmt);
    MOZ_ASSERT_IF(match, node.is<LoopControlStatement>());
    return match;
  }
};

class DebuggerStatement : public NullaryNode {
 public:
  explicit DebuggerStatement(const TokenPos& pos)
      : NullaryNode(ParseNodeKind::DebuggerStmt, pos) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::DebuggerStmt);
    MOZ_ASSERT_IF(match, node.is<NullaryNode>());
    return match;
  }
};

class ConditionalExpression : public TernaryNode {
 public:
  ConditionalExpression(ParseNode* condition, ParseNode* thenExpr,
                        ParseNode* elseExpr)
      : TernaryNode(ParseNodeKind::ConditionalExpr, condition, thenExpr,
                    elseExpr,
                    TokenPos(condition->pn_pos.begin, elseExpr->pn_pos.end)) {
    MOZ_ASSERT(condition);
    MOZ_ASSERT(thenExpr);
    MOZ_ASSERT(elseExpr);
  }

  ParseNode& condition() const { return *kid1(); }

  ParseNode& thenExpression() const { return *kid2(); }

  ParseNode& elseExpression() const { return *kid3(); }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::ConditionalExpr);
    MOZ_ASSERT_IF(match, node.is<TernaryNode>());
    return match;
  }
};

class TryNode : public TernaryNode {
 public:
  TryNode(uint32_t begin, ParseNode* body, LexicalScopeNode* catchScope,
          ParseNode* finallyBlock)
      : TernaryNode(
            ParseNodeKind::TryStmt, body, catchScope, finallyBlock,
            TokenPos(begin,
                     (finallyBlock ? finallyBlock : catchScope)->pn_pos.end)) {
    MOZ_ASSERT(body);
    MOZ_ASSERT(catchScope || finallyBlock);
  }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::TryStmt);
    MOZ_ASSERT_IF(match, node.is<TernaryNode>());
    return match;
  }

  ParseNode* body() const { return kid1(); }

  LexicalScopeNode* catchScope() const {
    return kid2() ? &kid2()->as<LexicalScopeNode>() : nullptr;
  }

  ParseNode* finallyBlock() const { return kid3(); }
};

class ThisLiteral : public UnaryNode {
 public:
  ThisLiteral(const TokenPos& pos, ParseNode* thisName)
      : UnaryNode(ParseNodeKind::ThisExpr, pos, thisName) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::ThisExpr);
    MOZ_ASSERT_IF(match, node.is<UnaryNode>());
    return match;
  }
};

class NullLiteral : public NullaryNode {
 public:
  explicit NullLiteral(const TokenPos& pos)
      : NullaryNode(ParseNodeKind::NullExpr, pos) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::NullExpr);
    MOZ_ASSERT_IF(match, node.is<NullaryNode>());
    return match;
  }
};

// This is only used internally, currently just for tagged templates and the
// initial value of fields without initializers. It represents the value
// 'undefined' (aka `void 0`), like NullLiteral represents the value 'null'.
class RawUndefinedLiteral : public NullaryNode {
 public:
  explicit RawUndefinedLiteral(const TokenPos& pos)
      : NullaryNode(ParseNodeKind::RawUndefinedExpr, pos) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::RawUndefinedExpr);
    MOZ_ASSERT_IF(match, node.is<NullaryNode>());
    return match;
  }
};

class BooleanLiteral : public NullaryNode {
 public:
  BooleanLiteral(bool b, const TokenPos& pos)
      : NullaryNode(b ? ParseNodeKind::TrueExpr : ParseNodeKind::FalseExpr,
                    pos) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::TrueExpr) ||
                 node.isKind(ParseNodeKind::FalseExpr);
    MOZ_ASSERT_IF(match, node.is<NullaryNode>());
    return match;
  }
};

class RegExpLiteral : public ParseNode {
  RegExpIndex index_;

 public:
  RegExpLiteral(RegExpIndex dataIndex, const TokenPos& pos)
      : ParseNode(ParseNodeKind::RegExpExpr, pos), index_(dataIndex) {}

  // Create a RegExp object of this RegExp literal.
  RegExpObject* create(JSContext* cx, ParserAtomsTable& parserAtoms,
                       CompilationAtomCache& atomCache,
                       ExtensibleCompilationStencil& stencil) const;

#ifdef DEBUG
  void dumpImpl(ParserBase* parser, GenericPrinter& out, int indent);
#endif

  static bool test(const ParseNode& node) {
    return node.isKind(ParseNodeKind::RegExpExpr);
  }

  static constexpr TypeCode classTypeCode() { return TypeCode::Other; }

  template <typename Visitor>
  bool accept(Visitor& visitor) {
    return true;
  }

  RegExpIndex index() { return index_; }
};

class PropertyAccessBase : public BinaryNode {
 public:
  /*
   * PropertyAccess nodes can have any expression/'super' as left-hand
   * side, but the name must be a ParseNodeKind::PropertyName node.
   */
  PropertyAccessBase(ParseNodeKind kind, ParseNode* lhs, NameNode* name,
                     uint32_t begin, uint32_t end)
      : BinaryNode(kind, TokenPos(begin, end), lhs, name) {
    MOZ_ASSERT(lhs);
    MOZ_ASSERT(name);
  }

  ParseNode& expression() const { return *left(); }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::DotExpr) ||
                 node.isKind(ParseNodeKind::OptionalDotExpr);
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    MOZ_ASSERT_IF(match, node.as<BinaryNode>().right()->isKind(
                             ParseNodeKind::PropertyNameExpr));
    return match;
  }

  NameNode& key() const { return right()->as<NameNode>(); }

  // Method used by BytecodeEmitter::emitPropLHS for optimization.
  // Those methods allow expression to temporarily be nullptr for
  // optimization purpose.
  ParseNode* maybeExpression() const { return left(); }

  void setExpression(ParseNode* pn) { *unsafeLeftReference() = pn; }

  TaggedParserAtomIndex name() const { return right()->as<NameNode>().atom(); }
};

class PropertyAccess : public PropertyAccessBase {
 public:
  PropertyAccess(ParseNode* lhs, NameNode* name, uint32_t begin, uint32_t end)
      : PropertyAccessBase(ParseNodeKind::DotExpr, lhs, name, begin, end) {
    MOZ_ASSERT(lhs);
    MOZ_ASSERT(name);
  }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::DotExpr);
    MOZ_ASSERT_IF(match, node.is<PropertyAccessBase>());
    return match;
  }

  bool isSuper() const {
    // ParseNodeKind::SuperBase cannot result from any expression syntax.
    return expression().isKind(ParseNodeKind::SuperBase);
  }
};

class OptionalPropertyAccess : public PropertyAccessBase {
 public:
  OptionalPropertyAccess(ParseNode* lhs, NameNode* name, uint32_t begin,
                         uint32_t end)
      : PropertyAccessBase(ParseNodeKind::OptionalDotExpr, lhs, name, begin,
                           end) {
    MOZ_ASSERT(lhs);
    MOZ_ASSERT(name);
  }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::OptionalDotExpr);
    MOZ_ASSERT_IF(match, node.is<PropertyAccessBase>());
    return match;
  }
};

class PropertyByValueBase : public BinaryNode {
 public:
  PropertyByValueBase(ParseNodeKind kind, ParseNode* lhs, ParseNode* propExpr,
                      uint32_t begin, uint32_t end)
      : BinaryNode(kind, TokenPos(begin, end), lhs, propExpr) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::ElemExpr) ||
                 node.isKind(ParseNodeKind::OptionalElemExpr);
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    return match;
  }

  ParseNode& expression() const { return *left(); }

  ParseNode& key() const { return *right(); }
};

class PropertyByValue : public PropertyByValueBase {
 public:
  PropertyByValue(ParseNode* lhs, ParseNode* propExpr, uint32_t begin,
                  uint32_t end)
      : PropertyByValueBase(ParseNodeKind::ElemExpr, lhs, propExpr, begin,
                            end) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::ElemExpr);
    MOZ_ASSERT_IF(match, node.is<PropertyByValueBase>());
    return match;
  }

  bool isSuper() const { return left()->isKind(ParseNodeKind::SuperBase); }
};

class OptionalPropertyByValue : public PropertyByValueBase {
 public:
  OptionalPropertyByValue(ParseNode* lhs, ParseNode* propExpr, uint32_t begin,
                          uint32_t end)
      : PropertyByValueBase(ParseNodeKind::OptionalElemExpr, lhs, propExpr,
                            begin, end) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::OptionalElemExpr);
    MOZ_ASSERT_IF(match, node.is<PropertyByValueBase>());
    return match;
  }
};

class PrivateMemberAccessBase : public BinaryNode {
 public:
  PrivateMemberAccessBase(ParseNodeKind kind, ParseNode* lhs, NameNode* name,
                          uint32_t begin, uint32_t end)
      : BinaryNode(kind, TokenPos(begin, end), lhs, name) {
    MOZ_ASSERT(name->isKind(ParseNodeKind::PrivateName));
  }

  ParseNode& expression() const { return *left(); }

  NameNode& privateName() const {
    NameNode& name = right()->as<NameNode>();
    MOZ_ASSERT(name.isKind(ParseNodeKind::PrivateName));
    return name;
  }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::PrivateMemberExpr) ||
                 node.isKind(ParseNodeKind::OptionalPrivateMemberExpr);
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    MOZ_ASSERT_IF(match, node.as<BinaryNode>().right()->isKind(
                             ParseNodeKind::PrivateName));
    return match;
  }
};

class PrivateMemberAccess : public PrivateMemberAccessBase {
 public:
  PrivateMemberAccess(ParseNode* lhs, NameNode* name, uint32_t begin,
                      uint32_t end)
      : PrivateMemberAccessBase(ParseNodeKind::PrivateMemberExpr, lhs, name,
                                begin, end) {}

  static bool test(const ParseNode& node) {
    return node.isKind(ParseNodeKind::PrivateMemberExpr);
  }
};

class OptionalPrivateMemberAccess : public PrivateMemberAccessBase {
 public:
  OptionalPrivateMemberAccess(ParseNode* lhs, NameNode* name, uint32_t begin,
                              uint32_t end)
      : PrivateMemberAccessBase(ParseNodeKind::OptionalPrivateMemberExpr, lhs,
                                name, begin, end) {}

  static bool test(const ParseNode& node) {
    return node.isKind(ParseNodeKind::OptionalPrivateMemberExpr);
  }
};

/*
 * A CallSiteNode represents the implicit call site object argument in a
 * TaggedTemplate.
 */
class CallSiteNode : public ListNode {
 public:
  explicit CallSiteNode(uint32_t begin)
      : ListNode(ParseNodeKind::CallSiteObj, TokenPos(begin, begin + 1)) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::CallSiteObj);
    MOZ_ASSERT_IF(match, node.is<ListNode>());
    return match;
  }

  ListNode* rawNodes() const {
    MOZ_ASSERT(head());
    return &head()->as<ListNode>();
  }
};

class CallNode : public BinaryNode {
  const JSOp callOp_;

 public:
  CallNode(ParseNodeKind kind, JSOp callOp, ParseNode* left, ParseNode* right)
      : CallNode(kind, callOp, TokenPos(left->pn_pos.begin, right->pn_pos.end),
                 left, right) {}

  CallNode(ParseNodeKind kind, JSOp callOp, TokenPos pos, ParseNode* left,
           ParseNode* right)
      : BinaryNode(kind, pos, left, right), callOp_(callOp) {
    MOZ_ASSERT(is<CallNode>());
  }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::CallExpr) ||
                 node.isKind(ParseNodeKind::SuperCallExpr) ||
                 node.isKind(ParseNodeKind::OptionalCallExpr) ||
                 node.isKind(ParseNodeKind::TaggedTemplateExpr) ||
                 node.isKind(ParseNodeKind::CallImportExpr) ||
                 node.isKind(ParseNodeKind::NewExpr);
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    return match;
  }

  JSOp callOp() { return callOp_; }
};

class ClassMethod : public BinaryNode {
  bool isStatic_;
  AccessorType accessorType_;
  FunctionNode* initializerIfPrivate_;

 public:
  /*
   * Method definitions often keep a name and function body that overlap,
   * so explicitly define the beginning and end here.
   */
  ClassMethod(ParseNodeKind kind, ParseNode* name, ParseNode* body,
              AccessorType accessorType, bool isStatic,
              FunctionNode* initializerIfPrivate)
      : BinaryNode(kind, TokenPos(name->pn_pos.begin, body->pn_pos.end), name,
                   body),
        isStatic_(isStatic),
        accessorType_(accessorType),
        initializerIfPrivate_(initializerIfPrivate) {
    MOZ_ASSERT(kind == ParseNodeKind::DefaultConstructor ||
               kind == ParseNodeKind::ClassMethod);
  }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::DefaultConstructor) ||
                 node.isKind(ParseNodeKind::ClassMethod);
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    return match;
  }

  ParseNode& name() const { return *left(); }

  FunctionNode& method() const { return right()->as<FunctionNode>(); }

  bool isStatic() const { return isStatic_; }

  AccessorType accessorType() const { return accessorType_; }

  FunctionNode* initializerIfPrivate() const { return initializerIfPrivate_; }
};

class ClassField : public BinaryNode {
  bool isStatic_;

 public:
  ClassField(ParseNode* name, ParseNode* initializer, bool isStatic)
      : BinaryNode(ParseNodeKind::ClassField, initializer->pn_pos, name,
                   initializer),
        isStatic_(isStatic) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::ClassField);
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    return match;
  }

  ParseNode& name() const { return *left(); }

  FunctionNode* initializer() const { return &right()->as<FunctionNode>(); }

  bool isStatic() const { return isStatic_; }
};

// Hold onto the function generated for a class static block like
//
// class A {
//  static { /* this static block */ }
// }
//
class StaticClassBlock : public UnaryNode {
 public:
  explicit StaticClassBlock(FunctionNode* function)
      : UnaryNode(ParseNodeKind::StaticClassBlock, function->pn_pos, function) {
  }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::StaticClassBlock);
    MOZ_ASSERT_IF(match, node.is<UnaryNode>());
    return match;
  }
  FunctionNode* function() const { return &kid()->as<FunctionNode>(); }
};

class PropertyDefinition : public BinaryNode {
  AccessorType accessorType_;

 public:
  PropertyDefinition(ParseNode* name, ParseNode* value,
                     AccessorType accessorType)
      : BinaryNode(ParseNodeKind::PropertyDefinition,
                   TokenPos(name->pn_pos.begin, value->pn_pos.end), name,
                   value),
        accessorType_(accessorType) {}

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::PropertyDefinition);
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    return match;
  }

  AccessorType accessorType() { return accessorType_; }
};

class SwitchStatement : public BinaryNode {
  bool hasDefault_; /* only for ParseNodeKind::Switch */

 public:
  SwitchStatement(uint32_t begin, ParseNode* discriminant,
                  LexicalScopeNode* lexicalForCaseList, bool hasDefault)
      : BinaryNode(ParseNodeKind::SwitchStmt,
                   TokenPos(begin, lexicalForCaseList->pn_pos.end),
                   discriminant, lexicalForCaseList),
        hasDefault_(hasDefault) {
#ifdef DEBUG
    ListNode* cases = &lexicalForCaseList->scopeBody()->as<ListNode>();
    MOZ_ASSERT(cases->isKind(ParseNodeKind::StatementList));
    bool found = false;
    for (ParseNode* item : cases->contents()) {
      CaseClause* caseNode = &item->as<CaseClause>();
      if (caseNode->isDefault()) {
        found = true;
        break;
      }
    }
    MOZ_ASSERT(found == hasDefault);
#endif
  }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::SwitchStmt);
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    return match;
  }

  ParseNode& discriminant() const { return *left(); }

  LexicalScopeNode& lexicalForCaseList() const {
    return right()->as<LexicalScopeNode>();
  }

  bool hasDefault() const { return hasDefault_; }
};

class ClassNames : public BinaryNode {
 public:
  ClassNames(ParseNode* outerBinding, ParseNode* innerBinding,
             const TokenPos& pos)
      : BinaryNode(ParseNodeKind::ClassNames, pos, outerBinding, innerBinding) {
    MOZ_ASSERT_IF(outerBinding, outerBinding->isKind(ParseNodeKind::Name));
    MOZ_ASSERT(innerBinding->isKind(ParseNodeKind::Name));
    MOZ_ASSERT_IF(outerBinding, innerBinding->as<NameNode>().atom() ==
                                    outerBinding->as<NameNode>().atom());
  }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::ClassNames);
    MOZ_ASSERT_IF(match, node.is<BinaryNode>());
    return match;
  }

  /*
   * Classes require two definitions: The first "outer" binding binds the
   * class into the scope in which it was declared. the outer binding is a
   * mutable lexial binding. The second "inner" binding binds the class by
   * name inside a block in which the methods are evaulated. It is immutable,
   * giving the methods access to the static members of the class even if
   * the outer binding has been overwritten.
   */
  NameNode* outerBinding() const {
    if (ParseNode* binding = left()) {
      return &binding->as<NameNode>();
    }
    return nullptr;
  }

  NameNode* innerBinding() const { return &right()->as<NameNode>(); }
};

class ClassNode : public TernaryNode {
 private:
  LexicalScopeNode* innerScope() const {
    return &kid3()->as<LexicalScopeNode>();
  }

  ClassBodyScopeNode* bodyScope() const {
    return &innerScope()->scopeBody()->as<ClassBodyScopeNode>();
  }

 public:
  ClassNode(ParseNode* names, ParseNode* heritage,
            LexicalScopeNode* memberBlock, const TokenPos& pos)
      : TernaryNode(ParseNodeKind::ClassDecl, names, heritage, memberBlock,
                    pos) {
    MOZ_ASSERT(innerScope()->scopeBody()->is<ClassBodyScopeNode>());
    MOZ_ASSERT_IF(names, names->is<ClassNames>());
  }

  static bool test(const ParseNode& node) {
    bool match = node.isKind(ParseNodeKind::ClassDecl);
    MOZ_ASSERT_IF(match, node.is<TernaryNode>());
    return match;
  }

  ClassNames* names() const {
    return kid1() ? &kid1()->as<ClassNames>() : nullptr;
  }

  ParseNode* heritage() const { return kid2(); }

  ListNode* memberList() const { return bodyScope()->memberList(); }

  LexicalScopeNode* scopeBindings() const {
    LexicalScopeNode* scope = innerScope();
    return scope->isEmptyScope() ? nullptr : scope;
  }

  ClassBodyScopeNode* bodyScopeBindings() const {
    ClassBodyScopeNode* scope = bodyScope();
    return scope->isEmptyScope() ? nullptr : scope;
  }
};

#ifdef DEBUG
void DumpParseTree(ParserBase* parser, ParseNode* pn, GenericPrinter& out,
                   int indent = 0);
#endif

class ParseNodeAllocator {
 public:
  explicit ParseNodeAllocator(JSContext* cx, LifoAlloc& alloc)
      : cx(cx), alloc(alloc) {}

  void* allocNode(size_t size);

 private:
  JSContext* cx;
  LifoAlloc& alloc;
};

inline bool ParseNode::isConstant() {
  switch (pn_type) {
    case ParseNodeKind::NumberExpr:
    case ParseNodeKind::StringExpr:
    case ParseNodeKind::TemplateStringExpr:
    case ParseNodeKind::NullExpr:
    case ParseNodeKind::RawUndefinedExpr:
    case ParseNodeKind::FalseExpr:
    case ParseNodeKind::TrueExpr:
      return true;
    case ParseNodeKind::ArrayExpr:
    case ParseNodeKind::ObjectExpr:
      return !as<ListNode>().hasNonConstInitializer();
    default:
      return false;
  }
}

static inline ParseNode* FunctionFormalParametersList(ParseNode* fn,
                                                      unsigned* numFormals) {
  MOZ_ASSERT(fn->isKind(ParseNodeKind::Function));
  ListNode* argsBody = fn->as<FunctionNode>().body();
  MOZ_ASSERT(argsBody->isKind(ParseNodeKind::ParamsBody));
  *numFormals = argsBody->count();
  if (*numFormals > 0 && argsBody->last()->is<LexicalScopeNode>() &&
      argsBody->last()->as<LexicalScopeNode>().scopeBody()->isKind(
          ParseNodeKind::StatementList)) {
    (*numFormals)--;
  }
  return argsBody->head();
}

bool IsAnonymousFunctionDefinition(ParseNode* pn);

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_ParseNode_h */
