/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_SyntaxParseHandler_h
#define frontend_SyntaxParseHandler_h

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"   // mozilla::Maybe
#include "mozilla/Result.h"  // mozilla::Result, mozilla::UnusedZero

#include <string.h>

#include "jstypes.h"

#include "frontend/CompilationStencil.h"  // CompilationState
#include "frontend/FunctionSyntaxKind.h"  // FunctionSyntaxKind
#include "frontend/NameAnalysisTypes.h"   // PrivateNameKind
#include "frontend/ParseNode.h"
#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex
#include "frontend/TokenStream.h"

namespace js {
namespace frontend {
enum SyntaxParseHandlerNode {
  NodeFailure = 0,
  NodeGeneric,
  NodeGetProp,
  NodeStringExprStatement,
  NodeReturn,
  NodeBreak,
  NodeThrow,
  NodeEmptyStatement,

  NodeVarDeclaration,
  NodeLexicalDeclaration,

  // A non-arrow function expression with block body, from bog-standard
  // ECMAScript.
  NodeFunctionExpression,

  NodeFunctionArrow,
  NodeFunctionStatement,

  // This is needed for proper assignment-target handling.  ES6 formally
  // requires function calls *not* pass IsValidSimpleAssignmentTarget,
  // but at last check there were still sites with |f() = 5| and similar
  // in code not actually executed (or at least not executed enough to be
  // noticed).
  NodeFunctionCall,

  NodeOptionalFunctionCall,

  // Node representing normal names which don't require any special
  // casing.
  NodeName,

  // Nodes representing the names "arguments", "length" and "eval".
  NodeArgumentsName,
  NodeLengthName,
  NodeEvalName,

  // Node representing the "async" name, which may actually be a
  // contextual keyword.
  NodePotentialAsyncKeyword,

  // Node representing private names.
  NodePrivateName,

  NodeDottedProperty,
  NodeOptionalDottedProperty,
  NodeElement,
  NodeOptionalElement,
  // A distinct node for [PrivateName], to make detecting delete this.#x
  // detectable in syntax parse
  NodePrivateMemberAccess,
  NodeOptionalPrivateMemberAccess,

  // Node representing the compound Arguments.length expression;
  // Used only for property access, not assignment.
  NodeArgumentsLength,

  // Destructuring target patterns can't be parenthesized: |([a]) = [3];|
  // must be a syntax error.  (We can't use NodeGeneric instead of these
  // because that would trigger invalid-left-hand-side ReferenceError
  // semantics when SyntaxError semantics are desired.)
  NodeParenthesizedArray,
  NodeParenthesizedObject,

  // In rare cases a parenthesized |node| doesn't have the same semantics
  // as |node|.  Each such node has a special Node value, and we use a
  // different Node value to represent the parenthesized form.  See also
  // is{Unp,P}arenthesized*(Node), parenthesize(Node), and the various
  // functions that deal in NodeUnparenthesized* below.

  // Valuable for recognizing potential destructuring patterns.
  NodeUnparenthesizedArray,
  NodeUnparenthesizedObject,

  // The directive prologue at the start of a FunctionBody or ScriptBody
  // is the longest sequence (possibly empty) of string literal
  // expression statements at the start of a function.  Thus we need this
  // to treat |"use strict";| as a possible Use Strict Directive and
  // |("use strict");| as a useless statement.
  NodeUnparenthesizedString,

  // For destructuring patterns an assignment element with
  // an initializer expression is not allowed be parenthesized.
  // i.e. |{x = 1} = obj|
  NodeUnparenthesizedAssignment,

  // This node is necessary to determine if the base operand in an
  // exponentiation operation is an unparenthesized unary expression.
  // We want to reject |-2 ** 3|, but still need to allow |(-2) ** 3|.
  NodeUnparenthesizedUnary,

  // This node is necessary to determine if the LHS of a property access is
  // super related.
  NodeSuperBase
};

}  // namespace frontend
}  // namespace js

template <>
struct mozilla::detail::UnusedZero<js::frontend::SyntaxParseHandlerNode> {
  static const bool value = true;
};

namespace js {
namespace frontend {

// Parse handler used when processing the syntax in a block of code, to generate
// the minimal information which is required to detect syntax errors and allow
// bytecode to be emitted for outer functions.
//
// When parsing, we start at the top level with a full parse, and when possible
// only check the syntax for inner functions, so that they can be lazily parsed
// into bytecode when/if they first run. Checking the syntax of a function is
// several times faster than doing a full parse/emit, and lazy parsing improves
// both performance and memory usage significantly when pages contain large
// amounts of code that never executes (which happens often).
class SyntaxParseHandler {
  // Remember the last encountered name or string literal during syntax parses.
  TaggedParserAtomIndex lastAtom;
  TokenPos lastStringPos;

 public:
  struct NodeError {};

  using Node = SyntaxParseHandlerNode;

  using NodeResult = mozilla::Result<Node, NodeError>;
  using NodeErrorResult = mozilla::GenericErrorResult<NodeError>;

#define DECLARE_TYPE(typeName) \
  using typeName##Type = Node; \
  using typeName##Result = mozilla::Result<Node, NodeError>;
  FOR_EACH_PARSENODE_SUBCLASS(DECLARE_TYPE)
#undef DECLARE_TYPE

  using NullNode = Node;

  bool isNonArrowFunctionExpression(Node node) const {
    return node == NodeFunctionExpression;
  }

  bool isPropertyOrPrivateMemberAccess(Node node) {
    return node == NodeDottedProperty || node == NodeElement ||
           node == NodePrivateMemberAccess || node == NodeArgumentsLength;
  }

  bool isOptionalPropertyOrPrivateMemberAccess(Node node) {
    return node == NodeOptionalDottedProperty || node == NodeOptionalElement ||
           node == NodeOptionalPrivateMemberAccess;
  }

  bool isFunctionCall(Node node) {
    // Note: super() is a special form, *not* a function call.
    return node == NodeFunctionCall;
  }

  static bool isUnparenthesizedDestructuringPattern(Node node) {
    return node == NodeUnparenthesizedArray ||
           node == NodeUnparenthesizedObject;
  }

  static bool isParenthesizedDestructuringPattern(Node node) {
    // Technically this isn't a destructuring target at all -- the grammar
    // doesn't treat it as such.  But we need to know when this happens to
    // consider it a SyntaxError rather than an invalid-left-hand-side
    // ReferenceError.
    return node == NodeParenthesizedArray || node == NodeParenthesizedObject;
  }

 public:
  SyntaxParseHandler(FrontendContext* fc, CompilationState& compilationState) {
    MOZ_ASSERT(!compilationState.input.isDelazifying());
  }

  static NullNode null() { return NodeFailure; }
  static constexpr NodeErrorResult errorResult() {
    return NodeErrorResult(NodeError());
  }

#define DECLARE_AS(typeName) \
  static typeName##Type as##typeName(Node node) { return node; }
  FOR_EACH_PARSENODE_SUBCLASS(DECLARE_AS)
#undef DECLARE_AS

  NameNodeResult newName(TaggedParserAtomIndex name, const TokenPos& pos) {
    lastAtom = name;
    if (name == TaggedParserAtomIndex::WellKnown::arguments()) {
      return NodeArgumentsName;
    }
    if (pos.begin + strlen("async") == pos.end &&
        name == TaggedParserAtomIndex::WellKnown::async()) {
      return NodePotentialAsyncKeyword;
    }
    if (name == TaggedParserAtomIndex::WellKnown::eval()) {
      return NodeEvalName;
    }
    return NodeName;
  }

  UnaryNodeResult newComputedName(Node expr, uint32_t start, uint32_t end) {
    return NodeGeneric;
  }

  UnaryNodeResult newSyntheticComputedName(Node expr, uint32_t start,
                                           uint32_t end) {
    return NodeGeneric;
  }

  NameNodeResult newObjectLiteralPropertyName(TaggedParserAtomIndex atom,
                                              const TokenPos& pos) {
    return NodeName;
  }

  NameNodeResult newPrivateName(TaggedParserAtomIndex atom,
                                const TokenPos& pos) {
    return NodePrivateName;
  }

  NumericLiteralResult newNumber(double value, DecimalPoint decimalPoint,
                                 const TokenPos& pos) {
    return NodeGeneric;
  }

  BigIntLiteralResult newBigInt() { return NodeGeneric; }

  BooleanLiteralResult newBooleanLiteral(bool cond, const TokenPos& pos) {
    return NodeGeneric;
  }

  NameNodeResult newStringLiteral(TaggedParserAtomIndex atom,
                                  const TokenPos& pos) {
    lastAtom = atom;
    lastStringPos = pos;
    return NodeUnparenthesizedString;
  }

  NameNodeResult newTemplateStringLiteral(TaggedParserAtomIndex atom,
                                          const TokenPos& pos) {
    return NodeGeneric;
  }

  CallSiteNodeResult newCallSiteObject(uint32_t begin) { return NodeGeneric; }

  void addToCallSiteObject(CallSiteNodeType callSiteObj, Node rawNode,
                           Node cookedNode) {}

  ThisLiteralResult newThisLiteral(const TokenPos& pos, Node thisName) {
    return NodeGeneric;
  }
  NullLiteralResult newNullLiteral(const TokenPos& pos) { return NodeGeneric; }
  RawUndefinedLiteralResult newRawUndefinedLiteral(const TokenPos& pos) {
    return NodeGeneric;
  }

  RegExpLiteralResult newRegExp(Node reobj, const TokenPos& pos) {
    return NodeGeneric;
  }

  ConditionalExpressionResult newConditional(Node cond, Node thenExpr,
                                             Node elseExpr) {
    return NodeGeneric;
  }

  UnaryNodeResult newDelete(uint32_t begin, Node expr) {
    return NodeUnparenthesizedUnary;
  }

  UnaryNodeResult newTypeof(uint32_t begin, Node kid) {
    return NodeUnparenthesizedUnary;
  }

  UnaryNodeResult newUnary(ParseNodeKind kind, uint32_t begin, Node kid) {
    return NodeUnparenthesizedUnary;
  }

  UnaryNodeResult newUpdate(ParseNodeKind kind, uint32_t begin, Node kid) {
    return NodeGeneric;
  }

  UnaryNodeResult newSpread(uint32_t begin, Node kid) { return NodeGeneric; }

  NodeResult appendOrCreateList(ParseNodeKind kind, Node left, Node right,
                                ParseContext* pc) {
    return NodeGeneric;
  }

  // Expressions

  ListNodeResult newArrayLiteral(uint32_t begin) {
    return NodeUnparenthesizedArray;
  }
  [[nodiscard]] bool addElision(ListNodeType literal, const TokenPos& pos) {
    return true;
  }
  [[nodiscard]] bool addSpreadElement(ListNodeType literal, uint32_t begin,
                                      Node inner) {
    return true;
  }
  void addArrayElement(ListNodeType literal, Node element) {}

  ListNodeResult newArguments(const TokenPos& pos) { return NodeGeneric; }
  CallNodeResult newCall(Node callee, ListNodeType args, JSOp callOp) {
    return NodeFunctionCall;
  }

  CallNodeResult newOptionalCall(Node callee, ListNodeType args, JSOp callOp) {
    return NodeOptionalFunctionCall;
  }

  CallNodeResult newSuperCall(Node callee, ListNodeType args, bool isSpread) {
    return NodeGeneric;
  }
  CallNodeResult newTaggedTemplate(Node tag, ListNodeType args, JSOp callOp) {
    return NodeGeneric;
  }

  ListNodeResult newObjectLiteral(uint32_t begin) {
    return NodeUnparenthesizedObject;
  }

  ListNodeResult newClassMemberList(uint32_t begin) { return NodeGeneric; }
  ClassNamesResult newClassNames(Node outer, Node inner, const TokenPos& pos) {
    return NodeGeneric;
  }
  ClassNodeResult newClass(Node name, Node heritage, Node methodBlock,
#ifdef ENABLE_DECORATORS
                           ListNodeType decorators,
                           FunctionNodeType addInitializerFunction,
#endif
                           const TokenPos& pos) {
    return NodeGeneric;
  }

  LexicalScopeNodeResult newLexicalScope(Node body) {
    return NodeLexicalDeclaration;
  }

  ClassBodyScopeNodeResult newClassBodyScope(Node body) {
    return NodeLexicalDeclaration;
  }

  NewTargetNodeResult newNewTarget(NullaryNodeType newHolder,
                                   NullaryNodeType targetHolder,
                                   NameNodeType newTargetName) {
    return NodeGeneric;
  }
  NullaryNodeResult newPosHolder(const TokenPos& pos) { return NodeGeneric; }
  UnaryNodeResult newSuperBase(Node thisName, const TokenPos& pos) {
    return NodeSuperBase;
  }

  [[nodiscard]] bool addPrototypeMutation(ListNodeType literal, uint32_t begin,
                                          Node expr) {
    return true;
  }
  BinaryNodeResult newPropertyDefinition(Node key, Node val) {
    return NodeGeneric;
  }
  void addPropertyDefinition(ListNodeType literal, BinaryNodeType propdef) {}
  [[nodiscard]] bool addPropertyDefinition(ListNodeType literal, Node key,
                                           Node expr) {
    return true;
  }
  [[nodiscard]] bool addShorthand(ListNodeType literal, NameNodeType name,
                                  NameNodeType expr) {
    return true;
  }
  [[nodiscard]] bool addSpreadProperty(ListNodeType literal, uint32_t begin,
                                       Node inner) {
    return true;
  }
  [[nodiscard]] bool addObjectMethodDefinition(ListNodeType literal, Node key,
                                               FunctionNodeType funNode,
                                               AccessorType atype) {
    return true;
  }
  [[nodiscard]] NodeResult newDefaultClassConstructor(
      Node key, FunctionNodeType funNode) {
    return NodeGeneric;
  }
  [[nodiscard]] NodeResult newClassMethodDefinition(
      Node key, FunctionNodeType funNode, AccessorType atype, bool isStatic,
      mozilla::Maybe<FunctionNodeType> initializerIfPrivate
#ifdef ENABLE_DECORATORS
      ,
      ListNodeType decorators
#endif
  ) {
    return NodeGeneric;
  }
  [[nodiscard]] NodeResult newClassFieldDefinition(
      Node name, FunctionNodeType initializer, bool isStatic
#ifdef ENABLE_DECORATORS
      ,
      ListNodeType decorators, ClassMethodType accessorGetterNode,
      ClassMethodType accessorSetterNode
#endif
  ) {
    return NodeGeneric;
  }

  [[nodiscard]] NodeResult newStaticClassBlock(FunctionNodeType block) {
    return NodeGeneric;
  }

  [[nodiscard]] bool addClassMemberDefinition(ListNodeType memberList,
                                              Node member) {
    return true;
  }
  UnaryNodeResult newYieldExpression(uint32_t begin, Node value) {
    return NodeGeneric;
  }
  UnaryNodeResult newYieldStarExpression(uint32_t begin, Node value) {
    return NodeGeneric;
  }
  UnaryNodeResult newAwaitExpression(uint32_t begin, Node value) {
    return NodeUnparenthesizedUnary;
  }
  UnaryNodeResult newOptionalChain(uint32_t begin, Node value) {
    return NodeGeneric;
  }

  // Statements

  ListNodeResult newStatementList(const TokenPos& pos) { return NodeGeneric; }
  void addStatementToList(ListNodeType list, Node stmt) {}
  void setListEndPosition(ListNodeType list, const TokenPos& pos) {}
  void addCaseStatementToList(ListNodeType list, CaseClauseType caseClause) {}
  [[nodiscard]] bool prependInitialYield(ListNodeType stmtList, Node genName) {
    return true;
  }
  NullaryNodeResult newEmptyStatement(const TokenPos& pos) {
    return NodeEmptyStatement;
  }

  BinaryNodeResult newImportAttribute(Node keyNode, Node valueNode) {
    return NodeGeneric;
  }
  BinaryNodeResult newModuleRequest(Node moduleSpec, Node importAttributeList,
                                    const TokenPos& pos) {
    return NodeGeneric;
  }
  BinaryNodeResult newImportDeclaration(Node importSpecSet, Node moduleRequest,
                                        const TokenPos& pos) {
    return NodeGeneric;
  }
  BinaryNodeResult newImportSpec(Node importNameNode, Node bindingName) {
    return NodeGeneric;
  }
  UnaryNodeResult newImportNamespaceSpec(uint32_t begin, Node bindingName) {
    return NodeGeneric;
  }
  UnaryNodeResult newExportDeclaration(Node kid, const TokenPos& pos) {
    return NodeGeneric;
  }
  BinaryNodeResult newExportFromDeclaration(uint32_t begin, Node exportSpecSet,
                                            Node moduleRequest) {
    return NodeGeneric;
  }
  BinaryNodeResult newExportDefaultDeclaration(Node kid, Node maybeBinding,
                                               const TokenPos& pos) {
    return NodeGeneric;
  }
  BinaryNodeResult newExportSpec(Node bindingName, Node exportName) {
    return NodeGeneric;
  }
  UnaryNodeResult newExportNamespaceSpec(uint32_t begin, Node exportName) {
    return NodeGeneric;
  }
  NullaryNodeResult newExportBatchSpec(const TokenPos& pos) {
    return NodeGeneric;
  }
  BinaryNodeResult newImportMeta(NullaryNodeType importHolder,
                                 NullaryNodeType metaHolder) {
    return NodeGeneric;
  }
  BinaryNodeResult newCallImport(NullaryNodeType importHolder, Node singleArg) {
    return NodeGeneric;
  }
  BinaryNodeResult newCallImportSpec(Node specifierArg, Node optionalArg) {
    return NodeGeneric;
  }

  BinaryNodeResult newSetThis(Node thisName, Node value) { return value; }

  UnaryNodeResult newExprStatement(Node expr, uint32_t end) {
    return expr == NodeUnparenthesizedString ? NodeStringExprStatement
                                             : NodeGeneric;
  }

  TernaryNodeResult newIfStatement(uint32_t begin, Node cond, Node thenBranch,
                                   Node elseBranch) {
    return NodeGeneric;
  }
  BinaryNodeResult newDoWhileStatement(Node body, Node cond,
                                       const TokenPos& pos) {
    return NodeGeneric;
  }
  BinaryNodeResult newWhileStatement(uint32_t begin, Node cond, Node body) {
    return NodeGeneric;
  }
  SwitchStatementResult newSwitchStatement(
      uint32_t begin, Node discriminant,
      LexicalScopeNodeType lexicalForCaseList, bool hasDefault) {
    return NodeGeneric;
  }
  CaseClauseResult newCaseOrDefault(uint32_t begin, Node expr, Node body) {
    return NodeGeneric;
  }
  ContinueStatementResult newContinueStatement(TaggedParserAtomIndex label,
                                               const TokenPos& pos) {
    return NodeGeneric;
  }
  BreakStatementResult newBreakStatement(TaggedParserAtomIndex label,
                                         const TokenPos& pos) {
    return NodeBreak;
  }
  UnaryNodeResult newReturnStatement(Node expr, const TokenPos& pos) {
    return NodeReturn;
  }
  UnaryNodeResult newExpressionBody(Node expr) { return NodeReturn; }
  BinaryNodeResult newWithStatement(uint32_t begin, Node expr, Node body) {
    return NodeGeneric;
  }

  LabeledStatementResult newLabeledStatement(TaggedParserAtomIndex label,
                                             Node stmt, uint32_t begin) {
    return NodeGeneric;
  }

  UnaryNodeResult newThrowStatement(Node expr, const TokenPos& pos) {
    return NodeThrow;
  }
  TernaryNodeResult newTryStatement(uint32_t begin, Node body,
                                    LexicalScopeNodeType catchScope,
                                    Node finallyBlock) {
    return NodeGeneric;
  }
  DebuggerStatementResult newDebuggerStatement(const TokenPos& pos) {
    return NodeGeneric;
  }

  NameNodeResult newPropertyName(TaggedParserAtomIndex name,
                                 const TokenPos& pos) {
    lastAtom = name;
    if (name == TaggedParserAtomIndex::WellKnown::length()) {
      return NodeLengthName;
    }
    return NodeGeneric;
  }

  PropertyAccessResult newPropertyAccess(Node expr, NameNodeType key) {
    return NodeDottedProperty;
  }

  PropertyAccessResult newArgumentsLength(Node expr, NameNodeType key) {
    return NodeArgumentsLength;
  }

  PropertyAccessResult newOptionalPropertyAccess(Node expr, NameNodeType key) {
    return NodeOptionalDottedProperty;
  }

  PropertyByValueResult newPropertyByValue(Node lhs, Node index, uint32_t end) {
    MOZ_ASSERT(!isPrivateName(index));
    return NodeElement;
  }

  PropertyByValueResult newOptionalPropertyByValue(Node lhs, Node index,
                                                   uint32_t end) {
    return NodeOptionalElement;
  }

  PrivateMemberAccessResult newPrivateMemberAccess(Node lhs, Node privateName,
                                                   uint32_t end) {
    return NodePrivateMemberAccess;
  }

  PrivateMemberAccessResult newOptionalPrivateMemberAccess(Node lhs,
                                                           Node privateName,
                                                           uint32_t end) {
    return NodeOptionalPrivateMemberAccess;
  }

  [[nodiscard]] bool setupCatchScope(LexicalScopeNodeType lexicalScope,
                                     Node catchName, Node catchBody) {
    return true;
  }

  [[nodiscard]] bool setLastFunctionFormalParameterDefault(
      FunctionNodeType funNode, Node defaultValue) {
    return true;
  }

  void checkAndSetIsDirectRHSAnonFunction(Node pn) {}

  ParamsBodyNodeResult newParamsBody(const TokenPos& pos) {
    return NodeGeneric;
  }

  FunctionNodeResult newFunction(FunctionSyntaxKind syntaxKind,
                                 const TokenPos& pos) {
    switch (syntaxKind) {
      case FunctionSyntaxKind::Statement:
        return NodeFunctionStatement;
      case FunctionSyntaxKind::Arrow:
        return NodeFunctionArrow;
      default:
        // All non-arrow function expressions are initially presumed to have
        // block body.  This will be overridden later *if* the function
        // expression permissibly has an AssignmentExpression body.
        return NodeFunctionExpression;
    }
  }

  void setFunctionFormalParametersAndBody(FunctionNodeType funNode,
                                          ParamsBodyNodeType paramsBody) {}
  void setFunctionBody(FunctionNodeType funNode, LexicalScopeNodeType body) {}
  void setFunctionBox(FunctionNodeType funNode, FunctionBox* funbox) {}
  void addFunctionFormalParameter(FunctionNodeType funNode, Node argpn) {}

  ForNodeResult newForStatement(uint32_t begin, TernaryNodeType forHead,
                                Node body, unsigned iflags) {
    return NodeGeneric;
  }

  TernaryNodeResult newForHead(Node init, Node test, Node update,
                               const TokenPos& pos) {
    return NodeGeneric;
  }

  TernaryNodeResult newForInOrOfHead(ParseNodeKind kind, Node target,
                                     Node iteratedExpr, const TokenPos& pos) {
    return NodeGeneric;
  }

  AssignmentNodeResult finishInitializerAssignment(NameNodeType nameNode,
                                                   Node init) {
    return NodeUnparenthesizedAssignment;
  }

  void setBeginPosition(Node pn, Node oth) {}
  void setBeginPosition(Node pn, uint32_t begin) {}

  void setEndPosition(Node pn, Node oth) {}
  void setEndPosition(Node pn, uint32_t end) {}

  uint32_t getFunctionNameOffset(Node func, TokenStreamAnyChars& ts) {
    // XXX This offset isn't relevant to the offending function name.  But
    //     we may not *have* that function name around, because of how lazy
    //     parsing works -- the actual name could be outside
    //     |tokenStream.userbuf|'s observed region.  So the current offset
    //     is the best we can do.
    return ts.currentToken().pos.begin;
  }

  ListNodeResult newList(ParseNodeKind kind, const TokenPos& pos) {
    MOZ_ASSERT(kind != ParseNodeKind::VarStmt);
    MOZ_ASSERT(kind != ParseNodeKind::LetDecl);
    MOZ_ASSERT(kind != ParseNodeKind::ConstDecl);
    MOZ_ASSERT(kind != ParseNodeKind::ParamsBody);
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    MOZ_ASSERT(kind != ParseNodeKind::UsingDecl);
    MOZ_ASSERT(kind != ParseNodeKind::AwaitUsingDecl);
#endif
    return NodeGeneric;
  }

  ListNodeResult newList(ParseNodeKind kind, Node kid) {
    return newList(kind, TokenPos());
  }

  DeclarationListNodeResult newDeclarationList(ParseNodeKind kind,
                                               const TokenPos& pos) {
    if (kind == ParseNodeKind::VarStmt) {
      return NodeVarDeclaration;
    }
    MOZ_ASSERT(kind == ParseNodeKind::LetDecl ||
               kind == ParseNodeKind::ConstDecl
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
               || kind == ParseNodeKind::UsingDecl ||
               kind == ParseNodeKind::AwaitUsingDecl
#endif
    );
    return NodeLexicalDeclaration;
  }

  ListNodeResult newCommaExpressionList(Node kid) { return NodeGeneric; }

  void addList(ListNodeType list, Node kid) {
    MOZ_ASSERT(list == NodeGeneric || list == NodeUnparenthesizedArray ||
               list == NodeUnparenthesizedObject ||
               list == NodeVarDeclaration || list == NodeLexicalDeclaration ||
               list == NodeFunctionCall);
  }

  CallNodeResult newNewExpression(uint32_t begin, Node ctor, ListNodeType args,
                                  bool isSpread) {
    return NodeGeneric;
  }

  AssignmentNodeResult newAssignment(ParseNodeKind kind, Node lhs, Node rhs) {
    return kind == ParseNodeKind::AssignExpr ? NodeUnparenthesizedAssignment
                                             : NodeGeneric;
  }

  AssignmentNodeResult newInitExpr(Node lhs, Node rhs) { return NodeGeneric; }

  bool isUnparenthesizedAssignment(Node node) {
    return node == NodeUnparenthesizedAssignment;
  }

  bool isUnparenthesizedUnaryExpression(Node node) {
    return node == NodeUnparenthesizedUnary;
  }

  bool isReturnStatement(Node node) { return node == NodeReturn; }

  bool isStatementPermittedAfterReturnStatement(Node pn) {
    return pn == NodeFunctionStatement || isNonArrowFunctionExpression(pn) ||
           pn == NodeVarDeclaration || pn == NodeBreak || pn == NodeThrow ||
           pn == NodeEmptyStatement;
  }

  bool isSuperBase(Node pn) { return pn == NodeSuperBase; }

  void setListHasNonConstInitializer(ListNodeType literal) {}

  // NOTE: This is infallible.
  [[nodiscard]] Node parenthesize(Node node) {
    // A number of nodes have different behavior upon parenthesization, but
    // only in some circumstances.  Convert these nodes to special
    // parenthesized forms.
    if (node == NodeUnparenthesizedArray) {
      return NodeParenthesizedArray;
    }
    if (node == NodeUnparenthesizedObject) {
      return NodeParenthesizedObject;
    }

    // Other nodes need not be recognizable after parenthesization; convert
    // them to a generic node.
    if (node == NodeUnparenthesizedString ||
        node == NodeUnparenthesizedAssignment ||
        node == NodeUnparenthesizedUnary) {
      return NodeGeneric;
    }

    // Convert parenthesized |async| to a normal name node.
    if (node == NodePotentialAsyncKeyword) {
      return NodeName;
    }

    // In all other cases, the parenthesized form of |node| is equivalent
    // to the unparenthesized form: return |node| unchanged.
    return node;
  }

  // NOTE: This is infallible.
  template <typename NodeType>
  [[nodiscard]] NodeType setLikelyIIFE(NodeType node) {
    return node;  // Remain in syntax-parse mode.
  }

  bool isName(Node node) {
    return node == NodeName || node == NodeArgumentsName ||
           node == NodeLengthName || node == NodeEvalName ||
           node == NodePotentialAsyncKeyword;
  }

  bool isArgumentsName(Node node) { return node == NodeArgumentsName; }
  bool isLengthName(Node node) { return node == NodeLengthName; }
  bool isEvalName(Node node) { return node == NodeEvalName; }
  bool isAsyncKeyword(Node node) { return node == NodePotentialAsyncKeyword; }

  bool isArgumentsLength(Node node) { return node == NodeArgumentsLength; }

  bool isPrivateName(Node node) { return node == NodePrivateName; }
  bool isPrivateMemberAccess(Node node) {
    return node == NodePrivateMemberAccess;
  }

  TaggedParserAtomIndex maybeDottedProperty(Node node) {
    // Note: |super.apply(...)| is a special form that calls an "apply"
    // method retrieved from one value, but using a *different* value as
    // |this|.  It's not really eligible for the funapply/funcall
    // optimizations as they're currently implemented (assuming a single
    // value is used for both retrieval and |this|).
    if (node != NodeDottedProperty && node != NodeOptionalDottedProperty &&
        node != NodeArgumentsLength) {
      return TaggedParserAtomIndex::null();
    }
    return lastAtom;
  }

  TaggedParserAtomIndex isStringExprStatement(Node pn, TokenPos* pos) {
    if (pn == NodeStringExprStatement) {
      *pos = lastStringPos;
      return lastAtom;
    }
    return TaggedParserAtomIndex::null();
  }

  bool reuseLazyInnerFunctions() { return false; }
  bool reuseClosedOverBindings() { return false; }
  TaggedParserAtomIndex nextLazyClosedOverBinding() {
    MOZ_CRASH(
        "SyntaxParseHandler::canSkipLazyClosedOverBindings must return false");
  }

  void setPrivateNameKind(Node node, PrivateNameKind kind) {}
};

}  // namespace frontend
}  // namespace js

#endif /* frontend_SyntaxParseHandler_h */
