/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_FullParseHandler_h
#define frontend_FullParseHandler_h

#include "mozilla/Attributes.h"
#include "mozilla/PodOperations.h"

#include <string.h>

#include "frontend/ParseNode.h"
#include "frontend/SharedContext.h"

namespace js {

class RegExpObject;

namespace frontend {

enum class SourceKind {
    // We are parsing from a text source (Parser.h)
    Text,
    // We are parsing from a binary source (BinSource.h)
    Binary,
};

// Parse handler used when generating a full parse tree for all code which the
// parser encounters.
class FullParseHandler
{
    ParseNodeAllocator allocator;

    ParseNode* allocParseNode(size_t size) {
        MOZ_ASSERT(size == sizeof(ParseNode));
        return static_cast<ParseNode*>(allocator.allocNode());
    }

    /*
     * If this is a full parse to construct the bytecode for a function that
     * was previously lazily parsed, that lazy function and the current index
     * into its inner functions. We do not want to reparse the inner functions.
     */
    const Rooted<LazyScript*> lazyOuterFunction_;
    size_t lazyInnerFunctionIndex;
    size_t lazyClosedOverBindingIndex;

    const SourceKind sourceKind_;

  public:
    /* new_ methods for creating parse nodes. These report OOM on context. */
    JS_DECLARE_NEW_METHODS(new_, allocParseNode, inline)

    typedef ParseNode* Node;

    bool isPropertyAccess(ParseNode* node) {
        return node->isKind(ParseNodeKind::Dot) || node->isKind(ParseNodeKind::Elem);
    }

    bool isFunctionCall(ParseNode* node) {
        // Note: super() is a special form, *not* a function call.
        return node->isKind(ParseNodeKind::Call);
    }

    static bool isUnparenthesizedDestructuringPattern(ParseNode* node) {
        return !node->isInParens() && (node->isKind(ParseNodeKind::Object) ||
                                       node->isKind(ParseNodeKind::Array));
    }

    static bool isParenthesizedDestructuringPattern(ParseNode* node) {
        // Technically this isn't a destructuring pattern at all -- the grammar
        // doesn't treat it as such.  But we need to know when this happens to
        // consider it a SyntaxError rather than an invalid-left-hand-side
        // ReferenceError.
        return node->isInParens() && (node->isKind(ParseNodeKind::Object) ||
                                      node->isKind(ParseNodeKind::Array));
    }

    FullParseHandler(JSContext* cx, LifoAlloc& alloc, LazyScript* lazyOuterFunction,
                     SourceKind kind = SourceKind::Text)
      : allocator(cx, alloc),
        lazyOuterFunction_(cx, lazyOuterFunction),
        lazyInnerFunctionIndex(0),
        lazyClosedOverBindingIndex(0),
        sourceKind_(SourceKind::Text)
    {}

    static ParseNode* null() { return nullptr; }

    // The FullParseHandler may be used to create nodes for text sources
    // (from Parser.h) or for binary sources (from BinSource.h). In the latter
    // case, some common assumptions on offsets are incorrect, e.g. in `a + b`,
    // `a`, `b` and `+` may be stored in any order. We use `sourceKind()`
    // to determine whether we need to check these assumptions.
    SourceKind sourceKind() const { return sourceKind_; }

    ParseNode* freeTree(ParseNode* pn) { return allocator.freeTree(pn); }
    void prepareNodeForMutation(ParseNode* pn) { return allocator.prepareNodeForMutation(pn); }

    ParseNode* newName(PropertyName* name, const TokenPos& pos, JSContext* cx)
    {
        return new_<NameNode>(ParseNodeKind::Name, JSOP_GETNAME, name, pos);
    }

    ParseNode* newComputedName(ParseNode* expr, uint32_t begin, uint32_t end) {
        TokenPos pos(begin, end);
        return new_<UnaryNode>(ParseNodeKind::ComputedName, pos, expr);
    }

    ParseNode* newObjectLiteralPropertyName(JSAtom* atom, const TokenPos& pos) {
        return new_<NullaryNode>(ParseNodeKind::ObjectPropertyName, JSOP_NOP, pos, atom);
    }

    ParseNode* newNumber(double value, DecimalPoint decimalPoint, const TokenPos& pos) {
        ParseNode* pn = new_<NullaryNode>(ParseNodeKind::Number, pos);
        if (!pn)
            return nullptr;
        pn->initNumber(value, decimalPoint);
        return pn;
    }

    ParseNode* newBooleanLiteral(bool cond, const TokenPos& pos) {
        return new_<BooleanLiteral>(cond, pos);
    }

    ParseNode* newStringLiteral(JSAtom* atom, const TokenPos& pos) {
        return new_<NullaryNode>(ParseNodeKind::String, JSOP_NOP, pos, atom);
    }

    ParseNode* newTemplateStringLiteral(JSAtom* atom, const TokenPos& pos) {
        return new_<NullaryNode>(ParseNodeKind::TemplateString, JSOP_NOP, pos, atom);
    }

    ParseNode* newCallSiteObject(uint32_t begin) {
        ParseNode* callSite = new_<CallSiteNode>(begin);
        if (!callSite)
            return null();

        Node propExpr = newArrayLiteral(callSite->pn_pos.begin);
        if (!propExpr)
            return null();

        addArrayElement(callSite, propExpr);

        return callSite;
    }

    void addToCallSiteObject(ParseNode* callSiteObj, ParseNode* rawNode, ParseNode* cookedNode) {
        MOZ_ASSERT(callSiteObj->isKind(ParseNodeKind::CallSiteObj));

        addArrayElement(callSiteObj, cookedNode);
        addArrayElement(callSiteObj->pn_head, rawNode);

        /*
         * We don't know when the last noSubstTemplate will come in, and we
         * don't want to deal with this outside this method
         */
        setEndPosition(callSiteObj, callSiteObj->pn_head);
    }

    ParseNode* newThisLiteral(const TokenPos& pos, ParseNode* thisName) {
        return new_<ThisLiteral>(pos, thisName);
    }

    ParseNode* newNullLiteral(const TokenPos& pos) {
        return new_<NullLiteral>(pos);
    }

    ParseNode* newRawUndefinedLiteral(const TokenPos& pos) {
        return new_<RawUndefinedLiteral>(pos);
    }

    // The Boxer object here is any object that can allocate ObjectBoxes.
    // Specifically, a Boxer has a .newObjectBox(T) method that accepts a
    // Rooted<RegExpObject*> argument and returns an ObjectBox*.
    template <class Boxer>
    ParseNode* newRegExp(RegExpObject* reobj, const TokenPos& pos, Boxer& boxer) {
        ObjectBox* objbox = boxer.newObjectBox(reobj);
        if (!objbox)
            return null();
        return new_<RegExpLiteral>(objbox, pos);
    }

    ParseNode* newConditional(ParseNode* cond, ParseNode* thenExpr, ParseNode* elseExpr) {
        return new_<ConditionalExpression>(cond, thenExpr, elseExpr);
    }

    ParseNode* newDelete(uint32_t begin, ParseNode* expr) {
        if (expr->isKind(ParseNodeKind::Name)) {
            expr->setOp(JSOP_DELNAME);
            return newUnary(ParseNodeKind::DeleteName, begin, expr);
        }

        if (expr->isKind(ParseNodeKind::Dot))
            return newUnary(ParseNodeKind::DeleteProp, begin, expr);

        if (expr->isKind(ParseNodeKind::Elem))
            return newUnary(ParseNodeKind::DeleteElem, begin, expr);

        return newUnary(ParseNodeKind::DeleteExpr, begin, expr);
    }

    ParseNode* newTypeof(uint32_t begin, ParseNode* kid) {
        ParseNodeKind pnk = kid->isKind(ParseNodeKind::Name)
                            ? ParseNodeKind::TypeOfName
                            : ParseNodeKind::TypeOfExpr;
        return newUnary(pnk, begin, kid);
    }

    ParseNode* newUnary(ParseNodeKind kind, uint32_t begin, ParseNode* kid) {
        TokenPos pos(begin, kid->pn_pos.end);
        return new_<UnaryNode>(kind, pos, kid);
    }

    ParseNode* newUpdate(ParseNodeKind kind, uint32_t begin, ParseNode* kid) {
        TokenPos pos(begin, kid->pn_pos.end);
        return new_<UnaryNode>(kind, pos, kid);
    }

    ParseNode* newSpread(uint32_t begin, ParseNode* kid) {
        TokenPos pos(begin, kid->pn_pos.end);
        return new_<UnaryNode>(ParseNodeKind::Spread, pos, kid);
    }

  private:
    ParseNode* newBinary(ParseNodeKind kind, ParseNode* left, ParseNode* right,
                         JSOp op = JSOP_NOP)
    {
        TokenPos pos(left->pn_pos.begin, right->pn_pos.end);
        return new_<BinaryNode>(kind, op, pos, left, right);
    }

  public:
    ParseNode* appendOrCreateList(ParseNodeKind kind, ParseNode* left, ParseNode* right,
                                  ParseContext* pc)
    {
        return ParseNode::appendOrCreateList(kind, left, right, this, pc);
    }

    // Expressions

    ParseNode* newArrayLiteral(uint32_t begin) {
        return new_<ListNode>(ParseNodeKind::Array, TokenPos(begin, begin + 1));
    }

    MOZ_MUST_USE bool addElision(ParseNode* literal, const TokenPos& pos) {
        MOZ_ASSERT(literal->isKind(ParseNodeKind::Array));
        MOZ_ASSERT(literal->isArity(PN_LIST));

        ParseNode* elision = new_<NullaryNode>(ParseNodeKind::Elision, pos);
        if (!elision)
            return false;
        addList(/* list = */ literal, /* child = */ elision);
        literal->pn_xflags |= PNX_ARRAYHOLESPREAD | PNX_NONCONST;
        return true;
    }

    MOZ_MUST_USE bool addSpreadElement(ParseNode* literal, uint32_t begin, ParseNode* inner) {
        MOZ_ASSERT(literal->isKind(ParseNodeKind::Array));
        MOZ_ASSERT(literal->isArity(PN_LIST));

        ParseNode* spread = newSpread(begin, inner);
        if (!spread)
            return false;
        addList(/* list = */ literal, /* child = */ spread);
        literal->pn_xflags |= PNX_ARRAYHOLESPREAD | PNX_NONCONST;
        return true;
    }

    void addArrayElement(ParseNode* literal, ParseNode* element) {
        MOZ_ASSERT(literal->isArity(PN_LIST));

        if (!element->isConstant())
            literal->pn_xflags |= PNX_NONCONST;
        addList(/* list = */ literal, /* child = */ element);
    }

    ParseNode* newCall(const TokenPos& pos) {
        return new_<ListNode>(ParseNodeKind::Call, JSOP_CALL, pos);
    }

    ParseNode* newSuperCall(ParseNode* callee) {
        return new_<ListNode>(ParseNodeKind::SuperCall, JSOP_SUPERCALL, callee);
    }

    ParseNode* newTaggedTemplate(const TokenPos& pos) {
        return new_<ListNode>(ParseNodeKind::TaggedTemplate, JSOP_CALL, pos);
    }

    ParseNode* newObjectLiteral(uint32_t begin) {
        return new_<ListNode>(ParseNodeKind::Object, TokenPos(begin, begin + 1));
    }

    ParseNode* newClass(ParseNode* name, ParseNode* heritage, ParseNode* methodBlock,
                        const TokenPos& pos)
    {
        return new_<ClassNode>(name, heritage, methodBlock, pos);
    }
    ParseNode* newClassMethodList(uint32_t begin) {
        return new_<ListNode>(ParseNodeKind::ClassMethodList, TokenPos(begin, begin + 1));
    }
    ParseNode* newClassNames(ParseNode* outer, ParseNode* inner, const TokenPos& pos) {
        return new_<ClassNames>(outer, inner, pos);
    }
    ParseNode* newNewTarget(ParseNode* newHolder, ParseNode* targetHolder) {
        return new_<BinaryNode>(ParseNodeKind::NewTarget, JSOP_NOP, newHolder, targetHolder);
    }
    ParseNode* newPosHolder(const TokenPos& pos) {
        return new_<NullaryNode>(ParseNodeKind::PosHolder, pos);
    }
    ParseNode* newSuperBase(ParseNode* thisName, const TokenPos& pos) {
        return new_<UnaryNode>(ParseNodeKind::SuperBase, pos, thisName);
    }
    ParseNode* newCatchBlock(ParseNode* catchName, ParseNode* catchGuard, ParseNode* catchBody) {
        return new_<TernaryNode>(ParseNodeKind::Catch, catchName, catchGuard, catchBody);
    }
    MOZ_MUST_USE bool addPrototypeMutation(ParseNode* literal, uint32_t begin, ParseNode* expr) {
        MOZ_ASSERT(literal->isKind(ParseNodeKind::Object));
        MOZ_ASSERT(literal->isArity(PN_LIST));

        // Object literals with mutated [[Prototype]] are non-constant so that
        // singleton objects will have Object.prototype as their [[Prototype]].
        setListFlag(literal, PNX_NONCONST);

        ParseNode* mutation = newUnary(ParseNodeKind::MutateProto, begin, expr);
        if (!mutation)
            return false;
        addList(/* list = */ literal, /* child = */ mutation);
        return true;
    }

    MOZ_MUST_USE bool addPropertyDefinition(ParseNode* literal, ParseNode* key, ParseNode* val) {
        MOZ_ASSERT(literal->isKind(ParseNodeKind::Object));
        MOZ_ASSERT(literal->isArity(PN_LIST));
        MOZ_ASSERT(isUsableAsObjectPropertyName(key));

        ParseNode* propdef = newBinary(ParseNodeKind::Colon, key, val, JSOP_INITPROP);
        if (!propdef)
            return false;
        addList(/* list = */ literal, /* child = */ propdef);
        return true;
    }

    MOZ_MUST_USE bool addShorthand(ParseNode* literal, ParseNode* name, ParseNode* expr) {
        MOZ_ASSERT(literal->isKind(ParseNodeKind::Object));
        MOZ_ASSERT(literal->isArity(PN_LIST));
        MOZ_ASSERT(name->isKind(ParseNodeKind::ObjectPropertyName));
        MOZ_ASSERT(expr->isKind(ParseNodeKind::Name));
        MOZ_ASSERT(name->pn_atom == expr->pn_atom);

        setListFlag(literal, PNX_NONCONST);
        ParseNode* propdef = newBinary(ParseNodeKind::Shorthand, name, expr, JSOP_INITPROP);
        if (!propdef)
            return false;
        addList(/* list = */ literal, /* child = */ propdef);
        return true;
    }

    MOZ_MUST_USE bool addSpreadProperty(ParseNode* literal, uint32_t begin, ParseNode* inner) {
        MOZ_ASSERT(literal->isKind(ParseNodeKind::Object));
        MOZ_ASSERT(literal->isArity(PN_LIST));

        setListFlag(literal, PNX_NONCONST);
        ParseNode* spread = newSpread(begin, inner);
        if (!spread)
            return false;
        addList(/* list = */ literal, /* child = */ spread);
        return true;
    }

    MOZ_MUST_USE bool addObjectMethodDefinition(ParseNode* literal, ParseNode* key, ParseNode* fn,
                                                AccessorType atype)
    {
        MOZ_ASSERT(literal->isArity(PN_LIST));
        literal->pn_xflags |= PNX_NONCONST;

        ParseNode* propdef = newObjectMethodOrPropertyDefinition(key, fn, atype);
        if (!propdef)
            return false;

        addList(/* list = */ literal, /* child = */ propdef);
        return true;
    }

    MOZ_MUST_USE bool addClassMethodDefinition(ParseNode* methodList, ParseNode* key, ParseNode* fn,
                                               AccessorType atype, bool isStatic)
    {
        MOZ_ASSERT(methodList->isKind(ParseNodeKind::ClassMethodList));
        MOZ_ASSERT(isUsableAsObjectPropertyName(key));

        ParseNode* classMethod = new_<ClassMethod>(key, fn, AccessorTypeToJSOp(atype), isStatic);
        if (!classMethod)
            return false;
        addList(/* list = */ methodList, /* child = */ classMethod);
        return true;
    }

    ParseNode* newInitialYieldExpression(uint32_t begin, ParseNode* gen) {
        TokenPos pos(begin, begin + 1);
        return new_<UnaryNode>(ParseNodeKind::InitialYield, pos, gen);
    }

    ParseNode* newYieldExpression(uint32_t begin, ParseNode* value) {
        TokenPos pos(begin, value ? value->pn_pos.end : begin + 1);
        return new_<UnaryNode>(ParseNodeKind::Yield, pos, value);
    }

    ParseNode* newYieldStarExpression(uint32_t begin, ParseNode* value) {
        TokenPos pos(begin, value->pn_pos.end);
        return new_<UnaryNode>(ParseNodeKind::YieldStar, pos, value);
    }

    ParseNode* newAwaitExpression(uint32_t begin, ParseNode* value) {
        TokenPos pos(begin, value ? value->pn_pos.end : begin + 1);
        return new_<UnaryNode>(ParseNodeKind::Await, pos, value);
    }

    // Statements

    ParseNode* newStatementList(const TokenPos& pos) {
        return new_<ListNode>(ParseNodeKind::StatementList, pos);
    }

    MOZ_MUST_USE bool isFunctionStmt(ParseNode* stmt) {
        while (stmt->isKind(ParseNodeKind::Label))
            stmt = stmt->as<LabeledStatement>().statement();
        return stmt->isKind(ParseNodeKind::Function);
    }

    void addStatementToList(ParseNode* list, ParseNode* stmt) {
        MOZ_ASSERT(list->isKind(ParseNodeKind::StatementList));

        addList(/* list = */ list, /* child = */ stmt);

        if (isFunctionStmt(stmt)) {
            // PNX_FUNCDEFS notifies the emitter that the block contains
            // body-level function definitions that should be processed
            // before the rest of nodes.
            list->pn_xflags |= PNX_FUNCDEFS;
        }
    }

    void setListEndPosition(ParseNode* list, const TokenPos& pos) {
        MOZ_ASSERT(list->isKind(ParseNodeKind::StatementList));
        list->pn_pos.end = pos.end;
    }

    void addCaseStatementToList(ParseNode* list, ParseNode* casepn) {
        MOZ_ASSERT(list->isKind(ParseNodeKind::StatementList));
        MOZ_ASSERT(casepn->isKind(ParseNodeKind::Case));
        MOZ_ASSERT(casepn->pn_right->isKind(ParseNodeKind::StatementList));

        addList(/* list = */ list, /* child = */ casepn);

        if (casepn->pn_right->pn_xflags & PNX_FUNCDEFS)
            list->pn_xflags |= PNX_FUNCDEFS;
    }

    MOZ_MUST_USE inline bool addCatchBlock(ParseNode* catchList, ParseNode* lexicalScope,
                              ParseNode* catchName, ParseNode* catchGuard,
                              ParseNode* catchBody);

    MOZ_MUST_USE bool prependInitialYield(ParseNode* stmtList, ParseNode* genName) {
        MOZ_ASSERT(stmtList->isKind(ParseNodeKind::StatementList));
        MOZ_ASSERT(stmtList->isArity(PN_LIST));

        TokenPos yieldPos(stmtList->pn_pos.begin, stmtList->pn_pos.begin + 1);
        ParseNode* makeGen = new_<NullaryNode>(ParseNodeKind::Generator, yieldPos);
        if (!makeGen)
            return false;

        MOZ_ASSERT(genName->getOp() == JSOP_GETNAME);
        genName->setOp(JSOP_SETNAME);
        ParseNode* genInit = newAssignment(ParseNodeKind::Assign, /* lhs = */ genName,
                                           /* rhs = */ makeGen);
        if (!genInit)
            return false;

        ParseNode* initialYield = newInitialYieldExpression(yieldPos.begin, genInit);
        if (!initialYield)
            return false;

        stmtList->prepend(initialYield);
        return true;
    }

    ParseNode* newSetThis(ParseNode* thisName, ParseNode* val) {
        MOZ_ASSERT(thisName->getOp() == JSOP_GETNAME);
        thisName->setOp(JSOP_SETNAME);
        return newBinary(ParseNodeKind::SetThis, thisName, val);
    }

    ParseNode* newEmptyStatement(const TokenPos& pos) {
        return new_<NullaryNode>(ParseNodeKind::EmptyStatement, pos);
    }

    ParseNode* newImportDeclaration(ParseNode* importSpecSet,
                                    ParseNode* moduleSpec, const TokenPos& pos)
    {
        ParseNode* pn = new_<BinaryNode>(ParseNodeKind::Import, JSOP_NOP, pos,
                                         importSpecSet, moduleSpec);
        if (!pn)
            return null();
        return pn;
    }

    ParseNode* newImportSpec(ParseNode* importNameNode, ParseNode* bindingName) {
        return newBinary(ParseNodeKind::ImportSpec, importNameNode, bindingName);
    }

    ParseNode* newExportDeclaration(ParseNode* kid, const TokenPos& pos) {
        return new_<UnaryNode>(ParseNodeKind::Export, pos, kid);
    }

    ParseNode* newExportFromDeclaration(uint32_t begin, ParseNode* exportSpecSet,
                                        ParseNode* moduleSpec)
    {
        ParseNode* pn = new_<BinaryNode>(ParseNodeKind::ExportFrom, JSOP_NOP, exportSpecSet,
                                         moduleSpec);
        if (!pn)
            return null();
        pn->pn_pos.begin = begin;
        return pn;
    }

    ParseNode* newExportDefaultDeclaration(ParseNode* kid, ParseNode* maybeBinding,
                                           const TokenPos& pos) {
        return new_<BinaryNode>(ParseNodeKind::ExportDefault, JSOP_NOP, pos, kid, maybeBinding);
    }

    ParseNode* newExportSpec(ParseNode* bindingName, ParseNode* exportName) {
        return newBinary(ParseNodeKind::ExportSpec, bindingName, exportName);
    }

    ParseNode* newExportBatchSpec(const TokenPos& pos) {
        return new_<NullaryNode>(ParseNodeKind::ExportBatchSpec, JSOP_NOP, pos);
    }

    ParseNode* newExprStatement(ParseNode* expr, uint32_t end) {
        MOZ_ASSERT(expr->pn_pos.end <= end);
        return new_<UnaryNode>(ParseNodeKind::ExpressionStatement,
                               TokenPos(expr->pn_pos.begin, end), expr);
    }

    ParseNode* newIfStatement(uint32_t begin, ParseNode* cond, ParseNode* thenBranch,
                              ParseNode* elseBranch)
    {
        ParseNode* pn = new_<TernaryNode>(ParseNodeKind::If, cond, thenBranch, elseBranch);
        if (!pn)
            return null();
        pn->pn_pos.begin = begin;
        return pn;
    }

    ParseNode* newDoWhileStatement(ParseNode* body, ParseNode* cond, const TokenPos& pos) {
        return new_<BinaryNode>(ParseNodeKind::DoWhile, JSOP_NOP, pos, body, cond);
    }

    ParseNode* newWhileStatement(uint32_t begin, ParseNode* cond, ParseNode* body) {
        TokenPos pos(begin, body->pn_pos.end);
        return new_<BinaryNode>(ParseNodeKind::While, JSOP_NOP, pos, cond, body);
    }

    ParseNode* newForStatement(uint32_t begin, ParseNode* forHead, ParseNode* body,
                               unsigned iflags)
    {
        /* A FOR node is binary, left is loop control and right is the body. */
        JSOp op = forHead->isKind(ParseNodeKind::ForIn) ? JSOP_ITER : JSOP_NOP;
        BinaryNode* pn = new_<BinaryNode>(ParseNodeKind::For, op,
                                          TokenPos(begin, body->pn_pos.end),
                                          forHead, body);
        if (!pn)
            return null();
        pn->pn_iflags = iflags;
        return pn;
    }

    ParseNode* newForHead(ParseNode* init, ParseNode* test, ParseNode* update,
                          const TokenPos& pos)
    {
        return new_<TernaryNode>(ParseNodeKind::ForHead, init, test, update, pos);
    }

    ParseNode* newForInOrOfHead(ParseNodeKind kind, ParseNode* target, ParseNode* iteratedExpr,
                                const TokenPos& pos)
    {
        MOZ_ASSERT(kind == ParseNodeKind::ForIn || kind == ParseNodeKind::ForOf);
        return new_<TernaryNode>(kind, target, nullptr, iteratedExpr, pos);
    }

    ParseNode* newSwitchStatement(uint32_t begin, ParseNode* discriminant, ParseNode* caseList) {
        TokenPos pos(begin, caseList->pn_pos.end);
        return new_<BinaryNode>(ParseNodeKind::Switch, JSOP_NOP, pos, discriminant, caseList);
    }

    ParseNode* newCaseOrDefault(uint32_t begin, ParseNode* expr, ParseNode* body) {
        return new_<CaseClause>(expr, body, begin);
    }

    ParseNode* newContinueStatement(PropertyName* label, const TokenPos& pos) {
        return new_<ContinueStatement>(label, pos);
    }

    ParseNode* newBreakStatement(PropertyName* label, const TokenPos& pos) {
        return new_<BreakStatement>(label, pos);
    }

    ParseNode* newReturnStatement(ParseNode* expr, const TokenPos& pos) {
        MOZ_ASSERT_IF(expr, pos.encloses(expr->pn_pos));
        return new_<UnaryNode>(ParseNodeKind::Return, pos, expr);
    }

    ParseNode* newExpressionBody(ParseNode* expr) {
        return new_<UnaryNode>(ParseNodeKind::Return, expr->pn_pos, expr);
    }

    ParseNode* newWithStatement(uint32_t begin, ParseNode* expr, ParseNode* body) {
        return new_<BinaryNode>(ParseNodeKind::With, JSOP_NOP, TokenPos(begin, body->pn_pos.end),
                                expr, body);
    }

    ParseNode* newLabeledStatement(PropertyName* label, ParseNode* stmt, uint32_t begin) {
        return new_<LabeledStatement>(label, stmt, begin);
    }

    ParseNode* newThrowStatement(ParseNode* expr, const TokenPos& pos) {
        MOZ_ASSERT(pos.encloses(expr->pn_pos));
        return new_<UnaryNode>(ParseNodeKind::Throw, pos, expr);
    }

    ParseNode* newTryStatement(uint32_t begin, ParseNode* body, ParseNode* catchScope,
                               ParseNode* finallyBlock) {
        TokenPos pos(begin, (finallyBlock ? finallyBlock : catchScope)->pn_pos.end);
        return new_<TernaryNode>(ParseNodeKind::Try, body, catchScope, finallyBlock, pos);
    }

    ParseNode* newDebuggerStatement(const TokenPos& pos) {
        return new_<DebuggerStatement>(pos);
    }

    ParseNode* newPropertyAccess(ParseNode* expr, PropertyName* key, uint32_t end) {
        return new_<PropertyAccess>(expr, key, expr->pn_pos.begin, end);
    }

    ParseNode* newPropertyByValue(ParseNode* lhs, ParseNode* index, uint32_t end) {
        return new_<PropertyByValue>(lhs, index, lhs->pn_pos.begin, end);
    }

    bool setupCatchScope(ParseNode* lexicalScope, ParseNode* catchName, ParseNode* catchBody) {
        ParseNode* catchpn;
        if (catchName) {
            catchpn = new_<BinaryNode>(ParseNodeKind::Catch, JSOP_NOP, catchName, catchBody);
        } else {
            catchpn = new_<BinaryNode>(ParseNodeKind::Catch, JSOP_NOP, catchBody->pn_pos,
                                       catchName, catchBody);
        }
        if (!catchpn)
            return false;
        lexicalScope->setScopeBody(catchpn);
        return true;
    }

    inline MOZ_MUST_USE bool setLastFunctionFormalParameterDefault(ParseNode* funcpn,
                                                                   ParseNode* pn);

    void checkAndSetIsDirectRHSAnonFunction(ParseNode* pn) {
        if (IsAnonymousFunctionDefinition(pn))
            pn->setDirectRHSAnonFunction(true);
    }

    ParseNode* newFunctionStatement(const TokenPos& pos) {
        return new_<CodeNode>(ParseNodeKind::Function, JSOP_NOP, pos);
    }

    ParseNode* newFunctionExpression(const TokenPos& pos) {
        return new_<CodeNode>(ParseNodeKind::Function, JSOP_LAMBDA, pos);
    }

    ParseNode* newArrowFunction(const TokenPos& pos) {
        return new_<CodeNode>(ParseNodeKind::Function, JSOP_LAMBDA_ARROW, pos);
    }

    bool isExpressionClosure(ParseNode* node) const {
        return node->isKind(ParseNodeKind::Function) &&
               node->pn_funbox->isExprBody() &&
               !node->pn_funbox->isArrow();
    }

    void noteExpressionClosure(Node* funcNode) const {
        // No need to do anything: |funcNode->pn_funbox| modifications
        // performed elsewhere in the relevant code path will assure
        // |isExpressionClosure| above tests true on |*funcNode|.
    }

    ParseNode* newObjectMethodOrPropertyDefinition(ParseNode* key, ParseNode* fn, AccessorType atype) {
        MOZ_ASSERT(isUsableAsObjectPropertyName(key));

        return newBinary(ParseNodeKind::Colon, key, fn, AccessorTypeToJSOp(atype));
    }

    bool setComprehensionLambdaBody(ParseNode* pn, ParseNode* body) {
        MOZ_ASSERT(body->isKind(ParseNodeKind::StatementList));
        ParseNode* paramsBody = newList(ParseNodeKind::ParamsBody, body);
        if (!paramsBody)
            return false;
        setFunctionFormalParametersAndBody(pn, paramsBody);
        return true;
    }
    void setFunctionFormalParametersAndBody(ParseNode* funcNode, ParseNode* kid) {
        MOZ_ASSERT_IF(kid, kid->isKind(ParseNodeKind::ParamsBody));
        funcNode->pn_body = kid;
    }
    void setFunctionBox(ParseNode* pn, FunctionBox* funbox) {
        MOZ_ASSERT(pn->isKind(ParseNodeKind::Function));
        pn->pn_funbox = funbox;
        funbox->functionNode = pn;
    }
    void addFunctionFormalParameter(ParseNode* pn, ParseNode* argpn) {
        addList(/* list = */ pn->pn_body, /* child = */ argpn);
    }
    void setFunctionBody(ParseNode* fn, ParseNode* body) {
        MOZ_ASSERT(fn->pn_body->isKind(ParseNodeKind::ParamsBody));
        addList(/* list = */ fn->pn_body, /* child = */ body);
    }

    ParseNode* newModule(const TokenPos& pos) {
        return new_<CodeNode>(ParseNodeKind::Module, JSOP_NOP, pos);
    }

    ParseNode* newLexicalScope(LexicalScope::Data* bindings, ParseNode* body) {
        return new_<LexicalScopeNode>(bindings, body);
    }

    Node newNewExpression(uint32_t begin, ParseNode* ctor) {
        ParseNode* newExpr = new_<ListNode>(ParseNodeKind::New, JSOP_NEW, TokenPos(begin, begin + 1));
        if (!newExpr)
            return nullptr;

        addList(/* list = */ newExpr, /* child = */ ctor);
        return newExpr;
    }

    ParseNode* newAssignment(ParseNodeKind kind, ParseNode* lhs, ParseNode* rhs) {
        return newBinary(kind, lhs, rhs);
    }

    bool isUnparenthesizedAssignment(Node node) {
        if (node->isKind(ParseNodeKind::Assign) && !node->isInParens()) {
            // ParseNodeKind::Assign is also (mis)used for things like
            // |var name = expr;|. But this method is only called on actual
            // expressions, so we can just assert the node's op is the one used
            // for plain assignment.
            MOZ_ASSERT(node->isOp(JSOP_NOP));
            return true;
        }

        return false;
    }

    bool isUnparenthesizedUnaryExpression(ParseNode* node) {
        if (!node->isInParens()) {
            ParseNodeKind kind = node->getKind();
            return kind == ParseNodeKind::Void ||
                   kind == ParseNodeKind::Not ||
                   kind == ParseNodeKind::BitNot ||
                   kind == ParseNodeKind::Pos ||
                   kind == ParseNodeKind::Neg ||
                   IsTypeofKind(kind) ||
                   IsDeleteKind(kind);
        }
        return false;
    }

    bool isReturnStatement(ParseNode* node) {
        return node->isKind(ParseNodeKind::Return);
    }

    bool isStatementPermittedAfterReturnStatement(ParseNode *node) {
        ParseNodeKind kind = node->getKind();
        return kind == ParseNodeKind::Function ||
               kind == ParseNodeKind::Var ||
               kind == ParseNodeKind::Break ||
               kind == ParseNodeKind::Throw ||
               kind == ParseNodeKind::EmptyStatement;
    }

    bool isSuperBase(ParseNode* node) {
        return node->isKind(ParseNodeKind::SuperBase);
    }

    bool isUsableAsObjectPropertyName(ParseNode* node) {
        return node->isKind(ParseNodeKind::Number)
            || node->isKind(ParseNodeKind::ObjectPropertyName)
            || node->isKind(ParseNodeKind::String)
            || node->isKind(ParseNodeKind::ComputedName);
    }

    inline MOZ_MUST_USE bool finishInitializerAssignment(ParseNode* pn, ParseNode* init);

    void setBeginPosition(ParseNode* pn, ParseNode* oth) {
        setBeginPosition(pn, oth->pn_pos.begin);
    }
    void setBeginPosition(ParseNode* pn, uint32_t begin) {
        pn->pn_pos.begin = begin;
        MOZ_ASSERT(pn->pn_pos.begin <= pn->pn_pos.end);
    }

    void setEndPosition(ParseNode* pn, ParseNode* oth) {
        setEndPosition(pn, oth->pn_pos.end);
    }
    void setEndPosition(ParseNode* pn, uint32_t end) {
        pn->pn_pos.end = end;
        MOZ_ASSERT(pn->pn_pos.begin <= pn->pn_pos.end);
    }

    uint32_t getFunctionNameOffset(ParseNode* func, TokenStreamAnyChars& ts) {
        return func->pn_pos.begin;
    }

    bool isDeclarationKind(ParseNodeKind kind) {
        return kind == ParseNodeKind::Var ||
               kind == ParseNodeKind::Let ||
               kind == ParseNodeKind::Const;
    }

    ParseNode* newList(ParseNodeKind kind, const TokenPos& pos) {
        MOZ_ASSERT(!isDeclarationKind(kind));
        return new_<ListNode>(kind, JSOP_NOP, pos);
    }

  private:
    template<typename T>
    ParseNode* newList(ParseNodeKind kind, const T& begin) = delete;

  public:
    ParseNode* newList(ParseNodeKind kind, ParseNode* kid) {
        MOZ_ASSERT(!isDeclarationKind(kind));
        return new_<ListNode>(kind, JSOP_NOP, kid);
    }

    ParseNode* newDeclarationList(ParseNodeKind kind, const TokenPos& pos) {
        MOZ_ASSERT(isDeclarationKind(kind));
        return new_<ListNode>(kind, JSOP_NOP, pos);
    }

    bool isDeclarationList(ParseNode* node) {
        return isDeclarationKind(node->getKind());
    }

    ParseNode* singleBindingFromDeclaration(ParseNode* decl) {
        MOZ_ASSERT(isDeclarationList(decl));
        MOZ_ASSERT(decl->pn_count == 1);
        return decl->pn_head;
    }

    ParseNode* newCommaExpressionList(ParseNode* kid) {
        return new_<ListNode>(ParseNodeKind::Comma, JSOP_NOP, kid);
    }

    void addList(ParseNode* list, ParseNode* kid) {
        if (sourceKind_ == SourceKind::Text)
            list->append(kid);
        else
            list->appendWithoutOrderAssumption(kid);
    }

    void setOp(ParseNode* pn, JSOp op) {
        pn->setOp(op);
    }
    void setListFlag(ParseNode* pn, unsigned flag) {
        MOZ_ASSERT(pn->isArity(PN_LIST));
        pn->pn_xflags |= flag;
    }
    MOZ_MUST_USE ParseNode* parenthesize(ParseNode* pn) {
        pn->setInParens(true);
        return pn;
    }
    MOZ_MUST_USE ParseNode* setLikelyIIFE(ParseNode* pn) {
        return parenthesize(pn);
    }
    void setInDirectivePrologue(ParseNode* pn) {
        pn->pn_prologue = true;
    }

    bool isConstant(ParseNode* pn) {
        return pn->isConstant();
    }

    bool isName(ParseNode* node) {
        return node->isKind(ParseNodeKind::Name);
    }

    bool isArgumentsName(ParseNode* node, JSContext* cx) {
        return node->isKind(ParseNodeKind::Name) && node->pn_atom == cx->names().arguments;
    }

    bool isEvalName(ParseNode* node, JSContext* cx) {
        return node->isKind(ParseNodeKind::Name) && node->pn_atom == cx->names().eval;
    }

    bool isAsyncKeyword(ParseNode* node, JSContext* cx) {
        return node->isKind(ParseNodeKind::Name) &&
               node->pn_pos.begin + strlen("async") == node->pn_pos.end &&
               node->pn_atom == cx->names().async;
    }

    PropertyName* maybeDottedProperty(ParseNode* pn) {
        return pn->is<PropertyAccess>() ? &pn->as<PropertyAccess>().name() : nullptr;
    }
    JSAtom* isStringExprStatement(ParseNode* pn, TokenPos* pos) {
        if (JSAtom* atom = pn->isStringExprStatement()) {
            *pos = pn->pn_kid->pn_pos;
            return atom;
        }
        return nullptr;
    }

    void adjustGetToSet(ParseNode* node) {
        node->setOp(node->isOp(JSOP_GETLOCAL) ? JSOP_SETLOCAL : JSOP_SETNAME);
    }

    bool canSkipLazyInnerFunctions() {
        return !!lazyOuterFunction_;
    }
    bool canSkipLazyClosedOverBindings() {
        return !!lazyOuterFunction_;
    }
    JSFunction* nextLazyInnerFunction() {
        MOZ_ASSERT(lazyInnerFunctionIndex < lazyOuterFunction_->numInnerFunctions());
        return lazyOuterFunction_->innerFunctions()[lazyInnerFunctionIndex++];
    }
    JSAtom* nextLazyClosedOverBinding() {
        MOZ_ASSERT(lazyClosedOverBindingIndex < lazyOuterFunction_->numClosedOverBindings());
        return lazyOuterFunction_->closedOverBindings()[lazyClosedOverBindingIndex++];
    }
};

inline bool
FullParseHandler::addCatchBlock(ParseNode* catchList, ParseNode* lexicalScope,
                                ParseNode* catchName, ParseNode* catchGuard,
                                ParseNode* catchBody)
{
    ParseNode* catchpn = newCatchBlock(catchName, catchGuard, catchBody);
    if (!catchpn)
        return false;
    addList(/* list = */ catchList, /* child = */ lexicalScope);
    lexicalScope->setScopeBody(catchpn);
    return true;
}

inline bool
FullParseHandler::setLastFunctionFormalParameterDefault(ParseNode* funcpn,
                                                        ParseNode* defaultValue)
{
    MOZ_ASSERT(funcpn->isKind(ParseNodeKind::Function));
    MOZ_ASSERT(funcpn->isArity(PN_CODE));

    ParseNode* arg = funcpn->pn_body->last();
    ParseNode* pn = newBinary(ParseNodeKind::Assign, arg, defaultValue);
    if (!pn)
        return false;

    checkAndSetIsDirectRHSAnonFunction(defaultValue);

    funcpn->pn_body->pn_pos.end = pn->pn_pos.end;
    ParseNode* pnchild = funcpn->pn_body->pn_head;
    ParseNode* pnlast = funcpn->pn_body->last();
    MOZ_ASSERT(pnchild);
    if (pnchild == pnlast) {
        funcpn->pn_body->pn_head = pn;
    } else {
        while (pnchild->pn_next != pnlast) {
            MOZ_ASSERT(pnchild->pn_next);
            pnchild = pnchild->pn_next;
        }
        pnchild->pn_next = pn;
    }
    funcpn->pn_body->pn_tail = &pn->pn_next;

    return true;
}

inline bool
FullParseHandler::finishInitializerAssignment(ParseNode* pn, ParseNode* init)
{
    pn->pn_expr = init;
    pn->setOp(JSOP_SETNAME);

    /* The declarator's position must include the initializer. */
    pn->pn_pos.end = init->pn_pos.end;
    return true;
}

} // namespace frontend
} // namespace js

#endif /* frontend_FullParseHandler_h */
