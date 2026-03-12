/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_FullParseHandler_h
#define frontend_FullParseHandler_h

#include "mozilla/Maybe.h"   // mozilla::Maybe
#include "mozilla/Result.h"  // mozilla::Result, mozilla::UnusedZero
#include "mozilla/Try.h"     // MOZ_TRY*

#include <cstddef>  // std::nullptr_t
#include <string.h>

#include "jstypes.h"

#include "frontend/CompilationStencil.h"  // CompilationState
#include "frontend/FunctionSyntaxKind.h"  // FunctionSyntaxKind
#include "frontend/NameAnalysisTypes.h"   // PrivateNameKind
#include "frontend/ParseNode.h"
#include "frontend/Parser-macros.h"  // MOZ_TRY_VAR_OR_RETURN
#include "frontend/ParserAtom.h"     // TaggedParserAtomIndex
#include "frontend/SharedContext.h"
#include "frontend/Stencil.h"

template <>
struct mozilla::detail::UnusedZero<js::frontend::ParseNode*> {
  static const bool value = true;
};

#define DEFINE_UNUSED_ZERO(typeName)                            \
  template <>                                                   \
  struct mozilla::detail::UnusedZero<js::frontend::typeName*> { \
    static const bool value = true;                             \
  };
FOR_EACH_PARSENODE_SUBCLASS(DEFINE_UNUSED_ZERO)
#undef DEFINE_UNUSED_ZERO

namespace js {
namespace frontend {

class TokenStreamAnyChars;

// Parse handler used when generating a full parse tree for all code which the
// parser encounters.
class FullParseHandler {
  ParseNodeAllocator allocator;

  ParseNode* allocParseNode(size_t size) {
    return static_cast<ParseNode*>(allocator.allocNode(size));
  }

  // If this is a full parse to construct the bytecode for a function that
  // was previously lazily parsed, we still don't want to full parse the
  // inner functions. These members are used for this functionality:
  //
  // - reuseGCThings if ture it means that the following fields are valid.
  // - gcThingsData holds an incomplete stencil-like copy of inner functions as
  //   well as atoms.
  // - scriptData and scriptExtra_ hold information necessary to locate inner
  //   functions to skip over each.
  // - lazyInnerFunctionIndex is used as we skip over inner functions
  //   (see skipLazyInnerFunction),
  // - lazyClosedOverBindingIndex is used to synchronize binding computation
  //   with the scope traversal.
  //   (see propagateFreeNamesAndMarkClosedOverBindings),
  const CompilationSyntaxParseCache& previousParseCache_;

  size_t lazyInnerFunctionIndex;
  size_t lazyClosedOverBindingIndex;

  bool reuseGCThings;

  /* new_ methods for creating parse nodes. These report OOM on context. */
  JS_DECLARE_NEW_METHODS(new_, allocParseNode, inline)

 public:
  using NodeError = ParseNodeError;

  using Node = ParseNode*;
  using NodeResult = ParseNodeResult;
  using NodeErrorResult = mozilla::GenericErrorResult<NodeError>;

#define DECLARE_TYPE(typeName)      \
  using typeName##Type = typeName*; \
  using typeName##Result = mozilla::Result<typeName*, NodeError>;
  FOR_EACH_PARSENODE_SUBCLASS(DECLARE_TYPE)
#undef DECLARE_TYPE

  template <class T, typename... Args>
  inline mozilla::Result<T*, NodeError> newResult(Args&&... args) {
    auto* node = new_<T>(std::forward<Args>(args)...);
    if (!node) {
      return mozilla::Result<T*, NodeError>(NodeError());
    }
    return node;
  }

  using NullNode = std::nullptr_t;

  bool isPropertyOrPrivateMemberAccess(Node node) {
    return node->isKind(ParseNodeKind::DotExpr) ||
           node->isKind(ParseNodeKind::ElemExpr) ||
           node->isKind(ParseNodeKind::PrivateMemberExpr) ||
           node->isKind(ParseNodeKind::ArgumentsLength);
  }

  bool isOptionalPropertyOrPrivateMemberAccess(Node node) {
    return node->isKind(ParseNodeKind::OptionalDotExpr) ||
           node->isKind(ParseNodeKind::OptionalElemExpr) ||
           node->isKind(ParseNodeKind::PrivateMemberExpr);
  }

  bool isFunctionCall(Node node) {
    // Note: super() is a special form, *not* a function call.
    return node->isKind(ParseNodeKind::CallExpr);
  }

  static bool isUnparenthesizedDestructuringPattern(Node node) {
    return !node->isInParens() && (node->isKind(ParseNodeKind::ObjectExpr) ||
                                   node->isKind(ParseNodeKind::ArrayExpr));
  }

  static bool isParenthesizedDestructuringPattern(Node node) {
    // Technically this isn't a destructuring pattern at all -- the grammar
    // doesn't treat it as such.  But we need to know when this happens to
    // consider it a SyntaxError rather than an invalid-left-hand-side
    // ReferenceError.
    return node->isInParens() && (node->isKind(ParseNodeKind::ObjectExpr) ||
                                  node->isKind(ParseNodeKind::ArrayExpr));
  }

  FullParseHandler(FrontendContext* fc, CompilationState& compilationState)
      : allocator(fc, compilationState.parserAllocScope.alloc()),
        previousParseCache_(compilationState.previousParseCache),
        lazyInnerFunctionIndex(0),
        lazyClosedOverBindingIndex(0),
        reuseGCThings(compilationState.input.isDelazifying()) {}

  static NullNode null() { return NullNode(); }
  static constexpr NodeErrorResult errorResult() {
    return NodeErrorResult(NodeError());
  }

#define DECLARE_AS(typeName)                      \
  static typeName##Type as##typeName(Node node) { \
    return &node->as<typeName>();                 \
  }
  FOR_EACH_PARSENODE_SUBCLASS(DECLARE_AS)
#undef DECLARE_AS

  NameNodeResult newName(TaggedParserAtomIndex name, const TokenPos& pos) {
    return newResult<NameNode>(ParseNodeKind::Name, name, pos);
  }

  UnaryNodeResult newComputedName(Node expr, uint32_t begin, uint32_t end) {
    TokenPos pos(begin, end);
    return newResult<UnaryNode>(ParseNodeKind::ComputedName, pos, expr);
  }

  UnaryNodeResult newSyntheticComputedName(Node expr, uint32_t begin,
                                           uint32_t end) {
    TokenPos pos(begin, end);
    UnaryNode* node;
    MOZ_TRY_VAR(node,
                newResult<UnaryNode>(ParseNodeKind::ComputedName, pos, expr));
    node->setSyntheticComputedName();
    return node;
  }

  NameNodeResult newObjectLiteralPropertyName(TaggedParserAtomIndex atom,
                                              const TokenPos& pos) {
    return newResult<NameNode>(ParseNodeKind::ObjectPropertyName, atom, pos);
  }

  NameNodeResult newPrivateName(TaggedParserAtomIndex atom,
                                const TokenPos& pos) {
    return newResult<NameNode>(ParseNodeKind::PrivateName, atom, pos);
  }

  NumericLiteralResult newNumber(double value, DecimalPoint decimalPoint,
                                 const TokenPos& pos) {
    return newResult<NumericLiteral>(value, decimalPoint, pos);
  }

  BigIntLiteralResult newBigInt(BigIntIndex index, const TokenPos& pos) {
    return newResult<BigIntLiteral>(index, pos);
  }

  BooleanLiteralResult newBooleanLiteral(bool cond, const TokenPos& pos) {
    return newResult<BooleanLiteral>(cond, pos);
  }

  NameNodeResult newStringLiteral(TaggedParserAtomIndex atom,
                                  const TokenPos& pos) {
    return newResult<NameNode>(ParseNodeKind::StringExpr, atom, pos);
  }

  NameNodeResult newTemplateStringLiteral(TaggedParserAtomIndex atom,
                                          const TokenPos& pos) {
    return newResult<NameNode>(ParseNodeKind::TemplateStringExpr, atom, pos);
  }

  CallSiteNodeResult newCallSiteObject(uint32_t begin) {
    CallSiteNode* callSiteObj;
    MOZ_TRY_VAR(callSiteObj, newResult<CallSiteNode>(begin));

    ListNode* rawNodes;
    MOZ_TRY_VAR(rawNodes, newArrayLiteral(callSiteObj->pn_pos.begin));

    addArrayElement(callSiteObj, rawNodes);

    return callSiteObj;
  }

  void addToCallSiteObject(CallSiteNodeType callSiteObj, Node rawNode,
                           Node cookedNode) {
    MOZ_ASSERT(callSiteObj->isKind(ParseNodeKind::CallSiteObj));
    MOZ_ASSERT(rawNode->isKind(ParseNodeKind::TemplateStringExpr));
    MOZ_ASSERT(cookedNode->isKind(ParseNodeKind::TemplateStringExpr) ||
               cookedNode->isKind(ParseNodeKind::RawUndefinedExpr));

    addArrayElement(callSiteObj, cookedNode);
    addArrayElement(callSiteObj->rawNodes(), rawNode);

    /*
     * We don't know when the last noSubstTemplate will come in, and we
     * don't want to deal with this outside this method
     */
    setEndPosition(callSiteObj, callSiteObj->rawNodes());
  }

  ThisLiteralResult newThisLiteral(const TokenPos& pos, Node thisName) {
    return newResult<ThisLiteral>(pos, thisName);
  }

  NullLiteralResult newNullLiteral(const TokenPos& pos) {
    return newResult<NullLiteral>(pos);
  }

  RawUndefinedLiteralResult newRawUndefinedLiteral(const TokenPos& pos) {
    return newResult<RawUndefinedLiteral>(pos);
  }

  RegExpLiteralResult newRegExp(RegExpIndex index, const TokenPos& pos) {
    return newResult<RegExpLiteral>(index, pos);
  }

  ConditionalExpressionResult newConditional(Node cond, Node thenExpr,
                                             Node elseExpr) {
    return newResult<ConditionalExpression>(cond, thenExpr, elseExpr);
  }

  UnaryNodeResult newDelete(uint32_t begin, Node expr) {
    if (expr->isKind(ParseNodeKind::Name)) {
      return newUnary(ParseNodeKind::DeleteNameExpr, begin, expr);
    }

    if (expr->isKind(ParseNodeKind::DotExpr)) {
      return newUnary(ParseNodeKind::DeletePropExpr, begin, expr);
    }

    if (expr->isKind(ParseNodeKind::ElemExpr)) {
      return newUnary(ParseNodeKind::DeleteElemExpr, begin, expr);
    }

    if (expr->isKind(ParseNodeKind::OptionalChain)) {
      Node kid = expr->as<UnaryNode>().kid();
      // Handle property deletion explicitly. OptionalCall is handled
      // via DeleteExpr.
      if (kid->isKind(ParseNodeKind::DotExpr) ||
          kid->isKind(ParseNodeKind::OptionalDotExpr) ||
          kid->isKind(ParseNodeKind::ElemExpr) ||
          kid->isKind(ParseNodeKind::OptionalElemExpr)) {
        return newUnary(ParseNodeKind::DeleteOptionalChainExpr, begin, kid);
      }
    }

    return newUnary(ParseNodeKind::DeleteExpr, begin, expr);
  }

  UnaryNodeResult newTypeof(uint32_t begin, Node kid) {
    ParseNodeKind pnk = kid->isKind(ParseNodeKind::Name)
                            ? ParseNodeKind::TypeOfNameExpr
                            : ParseNodeKind::TypeOfExpr;
    return newUnary(pnk, begin, kid);
  }

  UnaryNodeResult newUnary(ParseNodeKind kind, uint32_t begin, Node kid) {
    TokenPos pos(begin, kid->pn_pos.end);
    return newResult<UnaryNode>(kind, pos, kid);
  }

  UnaryNodeResult newUpdate(ParseNodeKind kind, uint32_t begin, Node kid) {
    TokenPos pos(begin, kid->pn_pos.end);
    return newResult<UnaryNode>(kind, pos, kid);
  }

  UnaryNodeResult newSpread(uint32_t begin, Node kid) {
    TokenPos pos(begin, kid->pn_pos.end);
    return newResult<UnaryNode>(ParseNodeKind::Spread, pos, kid);
  }

 private:
  BinaryNodeResult newBinary(ParseNodeKind kind, Node left, Node right) {
    TokenPos pos(left->pn_pos.begin, right->pn_pos.end);
    return newResult<BinaryNode>(kind, pos, left, right);
  }

 public:
  NodeResult appendOrCreateList(ParseNodeKind kind, Node left, Node right,
                                ParseContext* pc) {
    return ParseNode::appendOrCreateList(kind, left, right, this, pc);
  }

  // Expressions

  ListNodeResult newArrayLiteral(uint32_t begin) {
    return newResult<ListNode>(ParseNodeKind::ArrayExpr,
                               TokenPos(begin, begin + 1));
  }

  [[nodiscard]] bool addElision(ListNodeType literal, const TokenPos& pos) {
    MOZ_ASSERT(literal->isKind(ParseNodeKind::ArrayExpr));

    NullaryNode* elision;
    MOZ_TRY_VAR_OR_RETURN(
        elision, newResult<NullaryNode>(ParseNodeKind::Elision, pos), false);
    addList(/* list = */ literal, /* kid = */ elision);
    literal->setHasNonConstInitializer();
    return true;
  }

  [[nodiscard]] bool addSpreadElement(ListNodeType literal, uint32_t begin,
                                      Node inner) {
    MOZ_ASSERT(literal->isKind(ParseNodeKind::ArrayExpr));

    UnaryNodeType spread;
    MOZ_TRY_VAR_OR_RETURN(spread, newSpread(begin, inner), false);
    addList(/* list = */ literal, /* kid = */ spread);
    literal->setHasNonConstInitializer();
    return true;
  }

  void addArrayElement(ListNodeType literal, Node element) {
    MOZ_ASSERT(literal->isKind(ParseNodeKind::ArrayExpr) ||
               literal->isKind(ParseNodeKind::CallSiteObj));
    if (!element->isConstant()) {
      literal->setHasNonConstInitializer();
    }
    addList(/* list = */ literal, /* kid = */ element);
  }

  CallNodeResult newCall(Node callee, ListNodeType args, JSOp callOp) {
    return newResult<CallNode>(ParseNodeKind::CallExpr, callOp, callee, args);
  }

  CallNodeResult newOptionalCall(Node callee, ListNodeType args, JSOp callOp) {
    return newResult<CallNode>(ParseNodeKind::OptionalCallExpr, callOp, callee,
                               args);
  }

  ListNodeResult newArguments(const TokenPos& pos) {
    return newResult<ListNode>(ParseNodeKind::Arguments, pos);
  }

  CallNodeResult newSuperCall(Node callee, ListNodeType args, bool isSpread) {
    return newResult<CallNode>(
        ParseNodeKind::SuperCallExpr,
        isSpread ? JSOp::SpreadSuperCall : JSOp::SuperCall, callee, args);
  }

  CallNodeResult newTaggedTemplate(Node tag, ListNodeType args, JSOp callOp) {
    return newResult<CallNode>(ParseNodeKind::TaggedTemplateExpr, callOp, tag,
                               args);
  }

  ListNodeResult newObjectLiteral(uint32_t begin) {
    return newResult<ListNode>(ParseNodeKind::ObjectExpr,
                               TokenPos(begin, begin + 1));
  }

  ClassNodeResult newClass(Node name, Node heritage,
                           LexicalScopeNodeType memberBlock,
#ifdef ENABLE_DECORATORS
                           ListNodeType decorators,
                           FunctionNodeType addInitializerFunction,
#endif
                           const TokenPos& pos) {
    return newResult<ClassNode>(name, heritage, memberBlock,
#ifdef ENABLE_DECORATORS
                                decorators, addInitializerFunction,
#endif
                                pos);
  }
  ListNodeResult newClassMemberList(uint32_t begin) {
    return newResult<ListNode>(ParseNodeKind::ClassMemberList,
                               TokenPos(begin, begin + 1));
  }
  ClassNamesResult newClassNames(Node outer, Node inner, const TokenPos& pos) {
    return newResult<ClassNames>(outer, inner, pos);
  }
  NewTargetNodeResult newNewTarget(NullaryNodeType newHolder,
                                   NullaryNodeType targetHolder,
                                   NameNodeType newTargetName) {
    return newResult<NewTargetNode>(newHolder, targetHolder, newTargetName);
  }
  NullaryNodeResult newPosHolder(const TokenPos& pos) {
    return newResult<NullaryNode>(ParseNodeKind::PosHolder, pos);
  }
  UnaryNodeResult newSuperBase(Node thisName, const TokenPos& pos) {
    return newResult<UnaryNode>(ParseNodeKind::SuperBase, pos, thisName);
  }
  [[nodiscard]] bool addPrototypeMutation(ListNodeType literal, uint32_t begin,
                                          Node expr) {
    MOZ_ASSERT(literal->isKind(ParseNodeKind::ObjectExpr));

    // Object literals with mutated [[Prototype]] are non-constant so that
    // singleton objects will have Object.prototype as their [[Prototype]].
    literal->setHasNonConstInitializer();

    UnaryNode* mutation;
    MOZ_TRY_VAR_OR_RETURN(
        mutation, newUnary(ParseNodeKind::MutateProto, begin, expr), false);
    addList(/* list = */ literal, /* kid = */ mutation);
    return true;
  }

  BinaryNodeResult newPropertyDefinition(Node key, Node val) {
    MOZ_ASSERT(isUsableAsObjectPropertyName(key));
    checkAndSetIsDirectRHSAnonFunction(val);
    return newResult<PropertyDefinition>(key, val, AccessorType::None);
  }

  void addPropertyDefinition(ListNodeType literal, BinaryNodeType propdef) {
    MOZ_ASSERT(literal->isKind(ParseNodeKind::ObjectExpr));
    MOZ_ASSERT(propdef->isKind(ParseNodeKind::PropertyDefinition));

    if (!propdef->right()->isConstant()) {
      literal->setHasNonConstInitializer();
    }

    addList(/* list = */ literal, /* kid = */ propdef);
  }

  [[nodiscard]] bool addPropertyDefinition(ListNodeType literal, Node key,
                                           Node val) {
    BinaryNode* propdef;
    MOZ_TRY_VAR_OR_RETURN(propdef, newPropertyDefinition(key, val), false);
    addPropertyDefinition(literal, propdef);
    return true;
  }

  [[nodiscard]] bool addShorthand(ListNodeType literal, NameNodeType name,
                                  NameNodeType expr) {
    MOZ_ASSERT(literal->isKind(ParseNodeKind::ObjectExpr));
    MOZ_ASSERT(name->isKind(ParseNodeKind::ObjectPropertyName));
    MOZ_ASSERT(expr->isKind(ParseNodeKind::Name));
    MOZ_ASSERT(name->atom() == expr->atom());

    literal->setHasNonConstInitializer();
    BinaryNode* propdef;
    MOZ_TRY_VAR_OR_RETURN(
        propdef, newBinary(ParseNodeKind::Shorthand, name, expr), false);
    addList(/* list = */ literal, /* kid = */ propdef);
    return true;
  }

  [[nodiscard]] bool addSpreadProperty(ListNodeType literal, uint32_t begin,
                                       Node inner) {
    MOZ_ASSERT(literal->isKind(ParseNodeKind::ObjectExpr));

    literal->setHasNonConstInitializer();
    ParseNode* spread;
    MOZ_TRY_VAR_OR_RETURN(spread, newSpread(begin, inner), false);
    addList(/* list = */ literal, /* kid = */ spread);
    return true;
  }

  [[nodiscard]] bool addObjectMethodDefinition(ListNodeType literal, Node key,
                                               FunctionNodeType funNode,
                                               AccessorType atype) {
    literal->setHasNonConstInitializer();

    checkAndSetIsDirectRHSAnonFunction(funNode);

    ParseNode* propdef;
    MOZ_TRY_VAR_OR_RETURN(
        propdef, newObjectMethodOrPropertyDefinition(key, funNode, atype),
        false);
    addList(/* list = */ literal, /* kid = */ propdef);
    return true;
  }

  [[nodiscard]] ClassMethodResult newDefaultClassConstructor(
      Node key, FunctionNodeType funNode) {
    MOZ_ASSERT(isUsableAsObjectPropertyName(key));

    checkAndSetIsDirectRHSAnonFunction(funNode);

    return newResult<ClassMethod>(
        ParseNodeKind::DefaultConstructor, key, funNode, AccessorType::None,
        /* isStatic = */ false, /* initializeIfPrivate = */ nullptr
#ifdef ENABLE_DECORATORS
        ,
        /* decorators = */ nullptr
#endif
    );
  }

  [[nodiscard]] ClassMethodResult newClassMethodDefinition(
      Node key, FunctionNodeType funNode, AccessorType atype, bool isStatic,
      mozilla::Maybe<FunctionNodeType> initializerIfPrivate
#ifdef ENABLE_DECORATORS
      ,
      ListNodeType decorators
#endif
  ) {
    MOZ_ASSERT(isUsableAsObjectPropertyName(key));

    checkAndSetIsDirectRHSAnonFunction(funNode);

    if (initializerIfPrivate.isSome()) {
      return newResult<ClassMethod>(ParseNodeKind::ClassMethod, key, funNode,
                                    atype, isStatic,
                                    initializerIfPrivate.value()
#ifdef ENABLE_DECORATORS
                                        ,
                                    decorators
#endif
      );
    }
    return newResult<ClassMethod>(ParseNodeKind::ClassMethod, key, funNode,
                                  atype, isStatic,
                                  /* initializeIfPrivate = */ nullptr
#ifdef ENABLE_DECORATORS
                                  ,
                                  decorators
#endif
    );
  }

  [[nodiscard]] ClassFieldResult newClassFieldDefinition(
      Node name, FunctionNodeType initializer, bool isStatic
#ifdef ENABLE_DECORATORS
      ,
      ListNodeType decorators, ClassMethodType accessorGetterNode,
      ClassMethodType accessorSetterNode
#endif
  ) {
    MOZ_ASSERT(isUsableAsObjectPropertyName(name));

    return newResult<ClassField>(name, initializer, isStatic
#if ENABLE_DECORATORS
                                 ,
                                 decorators, accessorGetterNode,
                                 accessorSetterNode
#endif
    );
  }

  [[nodiscard]] StaticClassBlockResult newStaticClassBlock(
      FunctionNodeType block) {
    return newResult<StaticClassBlock>(block);
  }

  [[nodiscard]] bool addClassMemberDefinition(ListNodeType memberList,
                                              Node member) {
    MOZ_ASSERT(memberList->isKind(ParseNodeKind::ClassMemberList));
    // Constructors can be surrounded by LexicalScopes.
    MOZ_ASSERT(member->isKind(ParseNodeKind::DefaultConstructor) ||
               member->isKind(ParseNodeKind::ClassMethod) ||
               member->isKind(ParseNodeKind::ClassField) ||
               member->isKind(ParseNodeKind::StaticClassBlock) ||
               (member->isKind(ParseNodeKind::LexicalScope) &&
                member->as<LexicalScopeNode>().scopeBody()->is<ClassMethod>()));

    addList(/* list = */ memberList, /* kid = */ member);
    return true;
  }

  UnaryNodeResult newInitialYieldExpression(uint32_t begin, Node gen) {
    TokenPos pos(begin, begin + 1);
    return newResult<UnaryNode>(ParseNodeKind::InitialYield, pos, gen);
  }

  UnaryNodeResult newYieldExpression(uint32_t begin, Node value) {
    TokenPos pos(begin, value ? value->pn_pos.end : begin + 1);
    return newResult<UnaryNode>(ParseNodeKind::YieldExpr, pos, value);
  }

  UnaryNodeResult newYieldStarExpression(uint32_t begin, Node value) {
    TokenPos pos(begin, value->pn_pos.end);
    return newResult<UnaryNode>(ParseNodeKind::YieldStarExpr, pos, value);
  }

  UnaryNodeResult newAwaitExpression(uint32_t begin, Node value) {
    TokenPos pos(begin, value ? value->pn_pos.end : begin + 1);
    return newResult<UnaryNode>(ParseNodeKind::AwaitExpr, pos, value);
  }

  UnaryNodeResult newOptionalChain(uint32_t begin, Node value) {
    TokenPos pos(begin, value->pn_pos.end);
    return newResult<UnaryNode>(ParseNodeKind::OptionalChain, pos, value);
  }

  // Statements

  ListNodeResult newStatementList(const TokenPos& pos) {
    return newResult<ListNode>(ParseNodeKind::StatementList, pos);
  }

  [[nodiscard]] bool isFunctionStmt(Node stmt) {
    while (stmt->isKind(ParseNodeKind::LabelStmt)) {
      stmt = stmt->as<LabeledStatement>().statement();
    }
    return stmt->is<FunctionNode>();
  }

  void addStatementToList(ListNodeType list, Node stmt) {
    MOZ_ASSERT(list->isKind(ParseNodeKind::StatementList));

    addList(/* list = */ list, /* kid = */ stmt);

    if (isFunctionStmt(stmt)) {
      // Notify the emitter that the block contains body-level function
      // definitions that should be processed before the rest of nodes.
      list->setHasTopLevelFunctionDeclarations();
    }
  }

  void setListEndPosition(ListNodeType list, const TokenPos& pos) {
    MOZ_ASSERT(list->isKind(ParseNodeKind::StatementList));
    list->pn_pos.end = pos.end;
  }

  void addCaseStatementToList(ListNodeType list, CaseClauseType caseClause) {
    MOZ_ASSERT(list->isKind(ParseNodeKind::StatementList));

    addList(/* list = */ list, /* kid = */ caseClause);

    if (caseClause->statementList()->hasTopLevelFunctionDeclarations()) {
      list->setHasTopLevelFunctionDeclarations();
    }
  }

  [[nodiscard]] bool prependInitialYield(ListNodeType stmtList, Node genName) {
    MOZ_ASSERT(stmtList->isKind(ParseNodeKind::StatementList));

    TokenPos yieldPos(stmtList->pn_pos.begin, stmtList->pn_pos.begin + 1);
    NullaryNode* makeGen;
    MOZ_TRY_VAR_OR_RETURN(
        makeGen, newResult<NullaryNode>(ParseNodeKind::Generator, yieldPos),
        false);

    ParseNode* genInit;
    MOZ_TRY_VAR_OR_RETURN(
        genInit,
        newAssignment(ParseNodeKind::AssignExpr, /* lhs = */ genName,
                      /* rhs = */ makeGen),
        false);

    UnaryNode* initialYield;
    MOZ_TRY_VAR_OR_RETURN(initialYield,
                          newInitialYieldExpression(yieldPos.begin, genInit),
                          false);

    stmtList->prepend(initialYield);
    return true;
  }

  BinaryNodeResult newSetThis(Node thisName, Node value) {
    return newBinary(ParseNodeKind::SetThis, thisName, value);
  }

  NullaryNodeResult newEmptyStatement(const TokenPos& pos) {
    return newResult<NullaryNode>(ParseNodeKind::EmptyStmt, pos);
  }

  BinaryNodeResult newImportAttribute(Node keyNode, Node valueNode) {
    return newBinary(ParseNodeKind::ImportAttribute, keyNode, valueNode);
  }

  BinaryNodeResult newModuleRequest(Node moduleSpec, Node importAttributeList,
                                    const TokenPos& pos) {
    return newResult<BinaryNode>(ParseNodeKind::ImportModuleRequest, pos,
                                 moduleSpec, importAttributeList);
  }

  BinaryNodeResult newImportDeclaration(Node importSpecSet, Node moduleRequest,
                                        const TokenPos& pos) {
    return newResult<BinaryNode>(ParseNodeKind::ImportDecl, pos, importSpecSet,
                                 moduleRequest);
  }

  BinaryNodeResult newImportSpec(Node importNameNode, Node bindingName) {
    return newBinary(ParseNodeKind::ImportSpec, importNameNode, bindingName);
  }

  UnaryNodeResult newImportNamespaceSpec(uint32_t begin, Node bindingName) {
    return newUnary(ParseNodeKind::ImportNamespaceSpec, begin, bindingName);
  }

  UnaryNodeResult newExportDeclaration(Node kid, const TokenPos& pos) {
    return newResult<UnaryNode>(ParseNodeKind::ExportStmt, pos, kid);
  }

  BinaryNodeResult newExportFromDeclaration(uint32_t begin, Node exportSpecSet,
                                            Node moduleRequest) {
    BinaryNode* decl;
    MOZ_TRY_VAR(decl, newResult<BinaryNode>(ParseNodeKind::ExportFromStmt,
                                            exportSpecSet, moduleRequest));
    decl->pn_pos.begin = begin;
    return decl;
  }

  BinaryNodeResult newExportDefaultDeclaration(Node kid, Node maybeBinding,
                                               const TokenPos& pos) {
    if (maybeBinding) {
      MOZ_ASSERT(maybeBinding->isKind(ParseNodeKind::Name));
      MOZ_ASSERT(!maybeBinding->isInParens());

      checkAndSetIsDirectRHSAnonFunction(kid);
    }

    return newResult<BinaryNode>(ParseNodeKind::ExportDefaultStmt, pos, kid,
                                 maybeBinding);
  }

  BinaryNodeResult newExportSpec(Node bindingName, Node exportName) {
    return newBinary(ParseNodeKind::ExportSpec, bindingName, exportName);
  }

  UnaryNodeResult newExportNamespaceSpec(uint32_t begin, Node exportName) {
    return newUnary(ParseNodeKind::ExportNamespaceSpec, begin, exportName);
  }

  NullaryNodeResult newExportBatchSpec(const TokenPos& pos) {
    return newResult<NullaryNode>(ParseNodeKind::ExportBatchSpecStmt, pos);
  }

  BinaryNodeResult newImportMeta(NullaryNodeType importHolder,
                                 NullaryNodeType metaHolder) {
    return newResult<BinaryNode>(ParseNodeKind::ImportMetaExpr, importHolder,
                                 metaHolder);
  }

  BinaryNodeResult newCallImport(NullaryNodeType importHolder, Node singleArg) {
    return newResult<BinaryNode>(ParseNodeKind::CallImportExpr, importHolder,
                                 singleArg);
  }

  BinaryNodeResult newCallImportSpec(Node specifierArg, Node optionalArg) {
    return newResult<BinaryNode>(ParseNodeKind::CallImportSpec, specifierArg,
                                 optionalArg);
  }

  UnaryNodeResult newExprStatement(Node expr, uint32_t end) {
    MOZ_ASSERT(expr->pn_pos.end <= end);
    return newResult<UnaryNode>(ParseNodeKind::ExpressionStmt,
                                TokenPos(expr->pn_pos.begin, end), expr);
  }

  TernaryNodeResult newIfStatement(uint32_t begin, Node cond, Node thenBranch,
                                   Node elseBranch) {
    TernaryNode* node;
    MOZ_TRY_VAR(node, newResult<TernaryNode>(ParseNodeKind::IfStmt, cond,
                                             thenBranch, elseBranch));
    node->pn_pos.begin = begin;
    return node;
  }

  BinaryNodeResult newDoWhileStatement(Node body, Node cond,
                                       const TokenPos& pos) {
    return newResult<BinaryNode>(ParseNodeKind::DoWhileStmt, pos, body, cond);
  }

  BinaryNodeResult newWhileStatement(uint32_t begin, Node cond, Node body) {
    TokenPos pos(begin, body->pn_pos.end);
    return newResult<BinaryNode>(ParseNodeKind::WhileStmt, pos, cond, body);
  }

  ForNodeResult newForStatement(uint32_t begin, TernaryNodeType forHead,
                                Node body, unsigned iflags) {
    return newResult<ForNode>(TokenPos(begin, body->pn_pos.end), forHead, body,
                              iflags);
  }

  TernaryNodeResult newForHead(Node init, Node test, Node update,
                               const TokenPos& pos) {
    return newResult<TernaryNode>(ParseNodeKind::ForHead, init, test, update,
                                  pos);
  }

  TernaryNodeResult newForInOrOfHead(ParseNodeKind kind, Node target,
                                     Node iteratedExpr, const TokenPos& pos) {
    MOZ_ASSERT(kind == ParseNodeKind::ForIn || kind == ParseNodeKind::ForOf);
    return newResult<TernaryNode>(kind, target, nullptr, iteratedExpr, pos);
  }

  SwitchStatementResult newSwitchStatement(
      uint32_t begin, Node discriminant,
      LexicalScopeNodeType lexicalForCaseList, bool hasDefault) {
    return newResult<SwitchStatement>(begin, discriminant, lexicalForCaseList,
                                      hasDefault);
  }

  CaseClauseResult newCaseOrDefault(uint32_t begin, Node expr, Node body) {
    return newResult<CaseClause>(expr, body, begin);
  }

  ContinueStatementResult newContinueStatement(TaggedParserAtomIndex label,
                                               const TokenPos& pos) {
    return newResult<ContinueStatement>(label, pos);
  }

  BreakStatementResult newBreakStatement(TaggedParserAtomIndex label,
                                         const TokenPos& pos) {
    return newResult<BreakStatement>(label, pos);
  }

  UnaryNodeResult newReturnStatement(Node expr, const TokenPos& pos) {
    MOZ_ASSERT_IF(expr, pos.encloses(expr->pn_pos));
    return newResult<UnaryNode>(ParseNodeKind::ReturnStmt, pos, expr);
  }

  UnaryNodeResult newExpressionBody(Node expr) {
    return newResult<UnaryNode>(ParseNodeKind::ReturnStmt, expr->pn_pos, expr);
  }

  BinaryNodeResult newWithStatement(uint32_t begin, Node expr, Node body) {
    return newResult<BinaryNode>(ParseNodeKind::WithStmt,
                                 TokenPos(begin, body->pn_pos.end), expr, body);
  }

  LabeledStatementResult newLabeledStatement(TaggedParserAtomIndex label,
                                             Node stmt, uint32_t begin) {
    return newResult<LabeledStatement>(label, stmt, begin);
  }

  UnaryNodeResult newThrowStatement(Node expr, const TokenPos& pos) {
    MOZ_ASSERT(pos.encloses(expr->pn_pos));
    return newResult<UnaryNode>(ParseNodeKind::ThrowStmt, pos, expr);
  }

  TernaryNodeResult newTryStatement(uint32_t begin, Node body,
                                    LexicalScopeNodeType catchScope,
                                    Node finallyBlock) {
    return newResult<TryNode>(begin, body, catchScope, finallyBlock);
  }

  DebuggerStatementResult newDebuggerStatement(const TokenPos& pos) {
    return newResult<DebuggerStatement>(pos);
  }

  NameNodeResult newPropertyName(TaggedParserAtomIndex name,
                                 const TokenPos& pos) {
    return newResult<NameNode>(ParseNodeKind::PropertyNameExpr, name, pos);
  }

  PropertyAccessResult newPropertyAccess(Node expr, NameNodeType key) {
    return newResult<PropertyAccess>(expr, key, expr->pn_pos.begin,
                                     key->pn_pos.end);
  }

  ArgumentsLengthResult newArgumentsLength(Node expr, NameNodeType key) {
    return newResult<ArgumentsLength>(expr, key, expr->pn_pos.begin,
                                      key->pn_pos.end);
  }

  PropertyByValueResult newPropertyByValue(Node lhs, Node index, uint32_t end) {
    return newResult<PropertyByValue>(lhs, index, lhs->pn_pos.begin, end);
  }

  OptionalPropertyAccessResult newOptionalPropertyAccess(Node expr,
                                                         NameNodeType key) {
    return newResult<OptionalPropertyAccess>(expr, key, expr->pn_pos.begin,
                                             key->pn_pos.end);
  }

  OptionalPropertyByValueResult newOptionalPropertyByValue(Node lhs, Node index,
                                                           uint32_t end) {
    return newResult<OptionalPropertyByValue>(lhs, index, lhs->pn_pos.begin,
                                              end);
  }

  PrivateMemberAccessResult newPrivateMemberAccess(Node lhs,
                                                   NameNodeType privateName,
                                                   uint32_t end) {
    return newResult<PrivateMemberAccess>(lhs, privateName, lhs->pn_pos.begin,
                                          end);
  }

  OptionalPrivateMemberAccessResult newOptionalPrivateMemberAccess(
      Node lhs, NameNodeType privateName, uint32_t end) {
    return newResult<OptionalPrivateMemberAccess>(lhs, privateName,
                                                  lhs->pn_pos.begin, end);
  }

  bool setupCatchScope(LexicalScopeNodeType lexicalScope, Node catchName,
                       Node catchBody) {
    BinaryNode* catchClause;
    if (catchName) {
      MOZ_TRY_VAR_OR_RETURN(
          catchClause,
          newResult<BinaryNode>(ParseNodeKind::Catch, catchName, catchBody),
          false);
    } else {
      MOZ_TRY_VAR_OR_RETURN(
          catchClause,
          newResult<BinaryNode>(ParseNodeKind::Catch, catchBody->pn_pos,
                                catchName, catchBody),
          false);
    }
    lexicalScope->setScopeBody(catchClause);
    return true;
  }

  [[nodiscard]] inline bool setLastFunctionFormalParameterDefault(
      FunctionNodeType funNode, Node defaultValue);

  void checkAndSetIsDirectRHSAnonFunction(Node pn) {
    if (IsAnonymousFunctionDefinition(pn)) {
      pn->setDirectRHSAnonFunction(true);
    }
  }

  ParamsBodyNodeResult newParamsBody(const TokenPos& pos) {
    return newResult<ParamsBodyNode>(pos);
  }

  FunctionNodeResult newFunction(FunctionSyntaxKind syntaxKind,
                                 const TokenPos& pos) {
    return newResult<FunctionNode>(syntaxKind, pos);
  }

  BinaryNodeResult newObjectMethodOrPropertyDefinition(Node key, Node value,
                                                       AccessorType atype) {
    MOZ_ASSERT(isUsableAsObjectPropertyName(key));

    return newResult<PropertyDefinition>(key, value, atype);
  }

  void setFunctionFormalParametersAndBody(FunctionNodeType funNode,
                                          ParamsBodyNodeType paramsBody) {
    funNode->setBody(paramsBody);
  }
  void setFunctionBox(FunctionNodeType funNode, FunctionBox* funbox) {
    funNode->setFunbox(funbox);
    funbox->functionNode = funNode;
  }
  void addFunctionFormalParameter(FunctionNodeType funNode, Node argpn) {
    addList(/* list = */ funNode->body(), /* kid = */ argpn);
  }
  void setFunctionBody(FunctionNodeType funNode, LexicalScopeNodeType body) {
    addList(/* list = */ funNode->body(), /* kid = */ body);
  }

  ModuleNodeResult newModule(const TokenPos& pos) {
    return newResult<ModuleNode>(pos);
  }

  LexicalScopeNodeResult newLexicalScope(LexicalScope::ParserData* bindings,
                                         Node body,
                                         ScopeKind kind = ScopeKind::Lexical) {
    return newResult<LexicalScopeNode>(bindings, body, kind);
  }

  ClassBodyScopeNodeResult newClassBodyScope(
      ClassBodyScope::ParserData* bindings, ListNodeType body) {
    return newResult<ClassBodyScopeNode>(bindings, body);
  }

  CallNodeResult newNewExpression(uint32_t begin, Node ctor, ListNodeType args,
                                  bool isSpread) {
    return newResult<CallNode>(ParseNodeKind::NewExpr,
                               isSpread ? JSOp::SpreadNew : JSOp::New,
                               TokenPos(begin, args->pn_pos.end), ctor, args);
  }

  AssignmentNodeResult newAssignment(ParseNodeKind kind, Node lhs, Node rhs) {
    if ((kind == ParseNodeKind::AssignExpr ||
         kind == ParseNodeKind::CoalesceAssignExpr ||
         kind == ParseNodeKind::OrAssignExpr ||
         kind == ParseNodeKind::AndAssignExpr) &&
        lhs->isKind(ParseNodeKind::Name) && !lhs->isInParens()) {
      checkAndSetIsDirectRHSAnonFunction(rhs);
    }

    return newResult<AssignmentNode>(kind, lhs, rhs);
  }

  BinaryNodeResult newInitExpr(Node lhs, Node rhs) {
    TokenPos pos(lhs->pn_pos.begin, rhs->pn_pos.end);
    return newResult<BinaryNode>(ParseNodeKind::InitExpr, pos, lhs, rhs);
  }

  bool isUnparenthesizedAssignment(Node node) {
    if ((node->isKind(ParseNodeKind::AssignExpr)) && !node->isInParens()) {
      return true;
    }

    return false;
  }

  bool isUnparenthesizedUnaryExpression(Node node) {
    if (!node->isInParens()) {
      ParseNodeKind kind = node->getKind();
      return kind == ParseNodeKind::VoidExpr ||
             kind == ParseNodeKind::NotExpr ||
             kind == ParseNodeKind::BitNotExpr ||
             kind == ParseNodeKind::PosExpr || kind == ParseNodeKind::NegExpr ||
             kind == ParseNodeKind::AwaitExpr || IsTypeofKind(kind) ||
             IsDeleteKind(kind);
    }
    return false;
  }

  bool isReturnStatement(Node node) {
    return node->isKind(ParseNodeKind::ReturnStmt);
  }

  bool isStatementPermittedAfterReturnStatement(Node node) {
    ParseNodeKind kind = node->getKind();
    return kind == ParseNodeKind::Function || kind == ParseNodeKind::VarStmt ||
           kind == ParseNodeKind::BreakStmt ||
           kind == ParseNodeKind::ThrowStmt || kind == ParseNodeKind::EmptyStmt;
  }

  bool isSuperBase(Node node) { return node->isKind(ParseNodeKind::SuperBase); }

  bool isUsableAsObjectPropertyName(Node node) {
    return node->isKind(ParseNodeKind::NumberExpr) ||
           node->isKind(ParseNodeKind::BigIntExpr) ||
           node->isKind(ParseNodeKind::ObjectPropertyName) ||
           node->isKind(ParseNodeKind::StringExpr) ||
           node->isKind(ParseNodeKind::ComputedName) ||
           node->isKind(ParseNodeKind::PrivateName);
  }

  AssignmentNodeResult finishInitializerAssignment(NameNodeType nameNode,
                                                   Node init) {
    MOZ_ASSERT(nameNode->isKind(ParseNodeKind::Name));
    MOZ_ASSERT(!nameNode->isInParens());

    checkAndSetIsDirectRHSAnonFunction(init);

    return newAssignment(ParseNodeKind::AssignExpr, nameNode, init);
  }

  void setBeginPosition(Node pn, Node oth) {
    setBeginPosition(pn, oth->pn_pos.begin);
  }
  void setBeginPosition(Node pn, uint32_t begin) {
    pn->pn_pos.begin = begin;
    MOZ_ASSERT(pn->pn_pos.begin <= pn->pn_pos.end);
  }

  void setEndPosition(Node pn, Node oth) {
    setEndPosition(pn, oth->pn_pos.end);
  }
  void setEndPosition(Node pn, uint32_t end) {
    pn->pn_pos.end = end;
    MOZ_ASSERT(pn->pn_pos.begin <= pn->pn_pos.end);
  }

  uint32_t getFunctionNameOffset(Node func, TokenStreamAnyChars& ts) {
    return func->pn_pos.begin;
  }

  ListNodeResult newList(ParseNodeKind kind, const TokenPos& pos) {
    auto list = newResult<ListNode>(kind, pos);
    MOZ_ASSERT_IF(list.isOk(), !list.unwrap()->is<DeclarationListNode>());
    MOZ_ASSERT_IF(list.isOk(), !list.unwrap()->is<ParamsBodyNode>());
    return list;
  }

  ListNodeResult newList(ParseNodeKind kind, Node kid) {
    auto list = newResult<ListNode>(kind, kid);
    MOZ_ASSERT_IF(list.isOk(), !list.unwrap()->is<DeclarationListNode>());
    MOZ_ASSERT_IF(list.isOk(), !list.unwrap()->is<ParamsBodyNode>());
    return list;
  }

  DeclarationListNodeResult newDeclarationList(ParseNodeKind kind,
                                               const TokenPos& pos) {
    return newResult<DeclarationListNode>(kind, pos);
  }

  ListNodeResult newCommaExpressionList(Node kid) {
    return newResult<ListNode>(ParseNodeKind::CommaExpr, kid);
  }

  void addList(ListNodeType list, Node kid) { list->append(kid); }

  void setListHasNonConstInitializer(ListNodeType literal) {
    literal->setHasNonConstInitializer();
  }

  // NOTE: This is infallible.
  template <typename NodeType>
  [[nodiscard]] NodeType parenthesize(NodeType node) {
    node->setInParens(true);
    return node;
  }

  // NOTE: This is infallible.
  template <typename NodeType>
  [[nodiscard]] NodeType setLikelyIIFE(NodeType node) {
    return parenthesize(node);
  }

  bool isName(Node node) { return node->isKind(ParseNodeKind::Name); }

  bool isArgumentsName(Node node) {
    return node->isKind(ParseNodeKind::Name) &&
           node->as<NameNode>().atom() ==
               TaggedParserAtomIndex::WellKnown::arguments();
  }

  bool isLengthName(Node node) {
    return node->isKind(ParseNodeKind::PropertyNameExpr) &&
           node->as<NameNode>().atom() ==
               TaggedParserAtomIndex::WellKnown::length();
  }

  bool isEvalName(Node node) {
    return node->isKind(ParseNodeKind::Name) &&
           node->as<NameNode>().atom() ==
               TaggedParserAtomIndex::WellKnown::eval();
  }

  bool isAsyncKeyword(Node node) {
    return node->isKind(ParseNodeKind::Name) &&
           node->pn_pos.begin + strlen("async") == node->pn_pos.end &&
           node->as<NameNode>().atom() ==
               TaggedParserAtomIndex::WellKnown::async();
  }

  bool isArgumentsLength(Node node) {
    return node->isKind(ParseNodeKind::ArgumentsLength);
  }

  bool isPrivateName(Node node) {
    return node->isKind(ParseNodeKind::PrivateName);
  }

  bool isPrivateMemberAccess(Node node) {
    if (node->isKind(ParseNodeKind::OptionalChain)) {
      return isPrivateMemberAccess(node->as<UnaryNode>().kid());
    }
    return node->is<PrivateMemberAccessBase>();
  }

  TaggedParserAtomIndex maybeDottedProperty(Node pn) {
    return pn->is<PropertyAccessBase>() ? pn->as<PropertyAccessBase>().name()
                                        : TaggedParserAtomIndex::null();
  }
  TaggedParserAtomIndex isStringExprStatement(Node pn, TokenPos* pos) {
    if (pn->is<UnaryNode>()) {
      UnaryNode* unary = &pn->as<UnaryNode>();
      if (auto atom = unary->isStringExprStatement()) {
        *pos = unary->kid()->pn_pos;
        return atom;
      }
    }
    return TaggedParserAtomIndex::null();
  }

  bool reuseLazyInnerFunctions() { return reuseGCThings; }
  bool reuseClosedOverBindings() { return reuseGCThings; }
  bool reuseRegexpSyntaxParse() { return reuseGCThings; }
  void nextLazyInnerFunction() { lazyInnerFunctionIndex++; }
  TaggedParserAtomIndex nextLazyClosedOverBinding() {
    // Trailing nullptrs were elided in PerHandlerParser::finishFunction().
    auto closedOverBindings = previousParseCache_.closedOverBindings();
    if (lazyClosedOverBindingIndex >= closedOverBindings.Length()) {
      return TaggedParserAtomIndex::null();
    }

    return closedOverBindings[lazyClosedOverBindingIndex++];
  }
  const ScriptStencil& cachedScriptData() const {
    // lazyInnerFunctionIndex is incremented with nextLazyInnferFunction before
    // reading the content, thus we need -1 to access the element that we just
    // skipped.
    return previousParseCache_.scriptData(lazyInnerFunctionIndex - 1);
  }
  const ScriptStencilExtra& cachedScriptExtra() const {
    // lazyInnerFunctionIndex is incremented with nextLazyInnferFunction before
    // reading the content, thus we need -1 to access the element that we just
    // skipped.
    return previousParseCache_.scriptExtra(lazyInnerFunctionIndex - 1);
  }

  void setPrivateNameKind(Node node, PrivateNameKind kind) {
    MOZ_ASSERT(node->is<NameNode>());
    node->as<NameNode>().setPrivateNameKind(kind);
  }
};

inline bool FullParseHandler::setLastFunctionFormalParameterDefault(
    FunctionNodeType funNode, Node defaultValue) {
  ParamsBodyNode* body = funNode->body();
  ParseNode* arg = body->last();
  ParseNode* pn;
  MOZ_TRY_VAR_OR_RETURN(
      pn, newAssignment(ParseNodeKind::AssignExpr, arg, defaultValue), false);

  body->replaceLast(pn);
  return true;
}

}  // namespace frontend
}  // namespace js

#endif /* frontend_FullParseHandler_h */
