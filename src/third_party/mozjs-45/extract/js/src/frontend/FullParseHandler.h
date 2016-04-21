/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_FullParseHandler_h
#define frontend_FullParseHandler_h

#include "mozilla/Attributes.h"
#include "mozilla/PodOperations.h"

#include "frontend/ParseNode.h"
#include "frontend/SharedContext.h"

namespace js {
namespace frontend {

template <typename ParseHandler>
class Parser;

class SyntaxParseHandler;

// Parse handler used when generating a full parse tree for all code which the
// parser encounters.
class FullParseHandler
{
    ParseNodeAllocator allocator;
    TokenStream& tokenStream;

    ParseNode* allocParseNode(size_t size) {
        MOZ_ASSERT(size == sizeof(ParseNode));
        return static_cast<ParseNode*>(allocator.allocNode());
    }

    ParseNode* cloneNode(const ParseNode& other) {
        ParseNode* node = allocParseNode(sizeof(ParseNode));
        if (!node)
            return nullptr;
        mozilla::PodAssign(node, &other);
        return node;
    }

    /*
     * If this is a full parse to construct the bytecode for a function that
     * was previously lazily parsed, that lazy function and the current index
     * into its inner functions. We do not want to reparse the inner functions.
     */
    LazyScript * const lazyOuterFunction_;
    size_t lazyInnerFunctionIndex;

    const TokenPos& pos() {
        return tokenStream.currentToken().pos;
    }

    inline ParseNode* makeAssignmentFromArg(ParseNode* arg, ParseNode* lhs, ParseNode* rhs);
    inline void replaceLastFunctionArgument(ParseNode* funcpn, ParseNode* pn);

  public:

    /*
     * If non-nullptr, points to a syntax parser which can be used for inner
     * functions. Cleared if language features not handled by the syntax parser
     * are encountered, in which case all future activity will use the full
     * parser.
     */
    Parser<SyntaxParseHandler>* syntaxParser;

    /* new_ methods for creating parse nodes. These report OOM on context. */
    JS_DECLARE_NEW_METHODS(new_, allocParseNode, inline)

    typedef ParseNode* Node;
    typedef Definition* DefinitionNode;

    bool isPropertyAccess(ParseNode* node) {
        return node->isKind(PNK_DOT) || node->isKind(PNK_ELEM);
    }

    bool isFunctionCall(ParseNode* node) {
        // Note: super() is a special form, *not* a function call.
        return node->isKind(PNK_CALL);
    }

    static bool isUnparenthesizedDestructuringPattern(ParseNode* node) {
        return !node->isInParens() && (node->isKind(PNK_OBJECT) || node->isKind(PNK_ARRAY));
    }

    static bool isParenthesizedDestructuringPattern(ParseNode* node) {
        // Technically this isn't a destructuring pattern at all -- the grammar
        // doesn't treat it as such.  But we need to know when this happens to
        // consider it a SyntaxError rather than an invalid-left-hand-side
        // ReferenceError.
        return node->isInParens() && (node->isKind(PNK_OBJECT) || node->isKind(PNK_ARRAY));
    }

    static bool isDestructuringPatternAnyParentheses(ParseNode* node) {
        return isUnparenthesizedDestructuringPattern(node) ||
               isParenthesizedDestructuringPattern(node);
    }

    FullParseHandler(ExclusiveContext* cx, LifoAlloc& alloc,
                     TokenStream& tokenStream, Parser<SyntaxParseHandler>* syntaxParser,
                     LazyScript* lazyOuterFunction)
      : allocator(cx, alloc),
        tokenStream(tokenStream),
        lazyOuterFunction_(lazyOuterFunction),
        lazyInnerFunctionIndex(0),
        syntaxParser(syntaxParser)
    {}

    static ParseNode* null() { return nullptr; }

    ParseNode* freeTree(ParseNode* pn) { return allocator.freeTree(pn); }
    void prepareNodeForMutation(ParseNode* pn) { return allocator.prepareNodeForMutation(pn); }
    const Token& currentToken() { return tokenStream.currentToken(); }

    ParseNode* newName(PropertyName* name, uint32_t blockid, const TokenPos& pos,
                       ExclusiveContext* cx)
    {
        return new_<NameNode>(PNK_NAME, JSOP_GETNAME, name, blockid, pos);
    }

    ParseNode* newComputedName(ParseNode* expr, uint32_t begin, uint32_t end) {
        TokenPos pos(begin, end);
        return new_<UnaryNode>(PNK_COMPUTED_NAME, JSOP_NOP, pos, expr);
    }

    Definition* newPlaceholder(JSAtom* atom, uint32_t blockid, const TokenPos& pos) {
        Definition* dn =
            (Definition*) new_<NameNode>(PNK_NAME, JSOP_NOP, atom, blockid, pos);
        if (!dn)
            return nullptr;
        dn->setDefn(true);
        dn->pn_dflags |= PND_PLACEHOLDER;
        return dn;
    }

    ParseNode* newObjectLiteralPropertyName(JSAtom* atom, const TokenPos& pos) {
        return new_<NullaryNode>(PNK_OBJECT_PROPERTY_NAME, JSOP_NOP, pos, atom);
    }

    ParseNode* newNumber(double value, DecimalPoint decimalPoint, const TokenPos& pos) {
        ParseNode* pn = new_<NullaryNode>(PNK_NUMBER, pos);
        if (!pn)
            return nullptr;
        pn->initNumber(value, decimalPoint);
        return pn;
    }

    ParseNode* newBooleanLiteral(bool cond, const TokenPos& pos) {
        return new_<BooleanLiteral>(cond, pos);
    }

    ParseNode* newStringLiteral(JSAtom* atom, const TokenPos& pos) {
        return new_<NullaryNode>(PNK_STRING, JSOP_NOP, pos, atom);
    }

    ParseNode* newTemplateStringLiteral(JSAtom* atom, const TokenPos& pos) {
        return new_<NullaryNode>(PNK_TEMPLATE_STRING, JSOP_NOP, pos, atom);
    }

    ParseNode* newCallSiteObject(uint32_t begin) {
        ParseNode* callSite = new_<CallSiteNode>(begin);
        if (!callSite)
            return null();

        Node propExpr = newArrayLiteral(getPosition(callSite).begin);
        if (!propExpr)
            return null();

        addArrayElement(callSite, propExpr);

        return callSite;
    }

    bool addToCallSiteObject(ParseNode* callSiteObj, ParseNode* rawNode, ParseNode* cookedNode) {
        MOZ_ASSERT(callSiteObj->isKind(PNK_CALLSITEOBJ));

        addArrayElement(callSiteObj, cookedNode);
        addArrayElement(callSiteObj->pn_head, rawNode);

        /*
         * We don't know when the last noSubstTemplate will come in, and we
         * don't want to deal with this outside this method
         */
        setEndPosition(callSiteObj, callSiteObj->pn_head);
        return true;
    }

    ParseNode* newThisLiteral(const TokenPos& pos, ParseNode* thisName) {
        return new_<ThisLiteral>(pos, thisName);
    }

    ParseNode* newNullLiteral(const TokenPos& pos) {
        return new_<NullLiteral>(pos);
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

    void markAsSetCall(ParseNode* pn) {
        pn->pn_xflags |= PNX_SETCALL;
    }

    ParseNode* newDelete(uint32_t begin, ParseNode* expr) {
        if (expr->isKind(PNK_NAME)) {
            expr->pn_dflags |= PND_DEOPTIMIZED;
            expr->setOp(JSOP_DELNAME);
            return newUnary(PNK_DELETENAME, JSOP_NOP, begin, expr);
        }

        if (expr->isKind(PNK_DOT))
            return newUnary(PNK_DELETEPROP, JSOP_NOP, begin, expr);

        if (expr->isKind(PNK_ELEM))
            return newUnary(PNK_DELETEELEM, JSOP_NOP, begin, expr);

        return newUnary(PNK_DELETEEXPR, JSOP_NOP, begin, expr);
    }

    ParseNode* newTypeof(uint32_t begin, ParseNode* kid) {
        TokenPos pos(begin, kid->pn_pos.end);
        ParseNodeKind kind = kid->isKind(PNK_NAME) ? PNK_TYPEOFNAME : PNK_TYPEOFEXPR;
        return new_<UnaryNode>(kind, JSOP_NOP, pos, kid);
    }

    ParseNode* newNullary(ParseNodeKind kind, JSOp op, const TokenPos& pos) {
        return new_<NullaryNode>(kind, op, pos);
    }

    ParseNode* newUnary(ParseNodeKind kind, JSOp op, uint32_t begin, ParseNode* kid) {
        TokenPos pos(begin, kid ? kid->pn_pos.end : begin + 1);
        return new_<UnaryNode>(kind, op, pos, kid);
    }

    ParseNode* newBinary(ParseNodeKind kind, JSOp op = JSOP_NOP) {
        return new_<BinaryNode>(kind, op, pos(), (ParseNode*) nullptr, (ParseNode*) nullptr);
    }
    ParseNode* newBinary(ParseNodeKind kind, ParseNode* left,
                         JSOp op = JSOP_NOP) {
        return new_<BinaryNode>(kind, op, left->pn_pos, left, (ParseNode*) nullptr);
    }
    ParseNode* newBinary(ParseNodeKind kind, ParseNode* left, ParseNode* right,
                         JSOp op = JSOP_NOP) {
        TokenPos pos(left->pn_pos.begin, right->pn_pos.end);
        return new_<BinaryNode>(kind, op, pos, left, right);
    }
    ParseNode* appendOrCreateList(ParseNodeKind kind, ParseNode* left, ParseNode* right,
                                  ParseContext<FullParseHandler>* pc, JSOp op = JSOP_NOP)
    {
        return ParseNode::appendOrCreateList(kind, op, left, right, this, pc);
    }

    ParseNode* newTernary(ParseNodeKind kind,
                          ParseNode* first, ParseNode* second, ParseNode* third,
                          JSOp op = JSOP_NOP)
    {
        return new_<TernaryNode>(kind, op, first, second, third);
    }

    // Expressions

    ParseNode* newArrayComprehension(ParseNode* body, const TokenPos& pos) {
        MOZ_ASSERT(pos.begin <= body->pn_pos.begin);
        MOZ_ASSERT(body->pn_pos.end <= pos.end);
        ParseNode* pn = new_<ListNode>(PNK_ARRAYCOMP, pos);
        if (!pn)
            return nullptr;
        pn->append(body);
        return pn;
    }

    ParseNode* newArrayLiteral(uint32_t begin) {
        ParseNode* literal = new_<ListNode>(PNK_ARRAY, TokenPos(begin, begin + 1));
        // Later in this stack: remove dependency on this opcode.
        if (literal)
            literal->setOp(JSOP_NEWINIT);
        return literal;
    }

    bool addElision(ParseNode* literal, const TokenPos& pos) {
        ParseNode* elision = new_<NullaryNode>(PNK_ELISION, pos);
        if (!elision)
            return false;
        literal->append(elision);
        literal->pn_xflags |= PNX_ARRAYHOLESPREAD | PNX_NONCONST;
        return true;
    }

    bool addSpreadElement(ParseNode* literal, uint32_t begin, ParseNode* inner) {
        TokenPos pos(begin, inner->pn_pos.end);
        ParseNode* spread = new_<UnaryNode>(PNK_SPREAD, JSOP_NOP, pos, inner);
        if (!spread)
            return null();
        literal->append(spread);
        literal->pn_xflags |= PNX_ARRAYHOLESPREAD | PNX_NONCONST;
        return true;
    }

    void addArrayElement(ParseNode* literal, ParseNode* element) {
        if (!element->isConstant())
            literal->pn_xflags |= PNX_NONCONST;
        literal->append(element);
    }

    ParseNode* newCall() {
        return newList(PNK_CALL, JSOP_CALL);
    }

    ParseNode* newTaggedTemplate() {
        return newList(PNK_TAGGED_TEMPLATE, JSOP_CALL);
    }

    ParseNode* newObjectLiteral(uint32_t begin) {
        ParseNode* literal = new_<ListNode>(PNK_OBJECT, TokenPos(begin, begin + 1));
        // Later in this stack: remove dependency on this opcode.
        if (literal)
            literal->setOp(JSOP_NEWINIT);
        return literal;
    }

    ParseNode* newClass(ParseNode* name, ParseNode* heritage, ParseNode* methodBlock) {
        return new_<ClassNode>(name, heritage, methodBlock);
    }
    ParseNode* newClassMethodList(uint32_t begin) {
        return new_<ListNode>(PNK_CLASSMETHODLIST, TokenPos(begin, begin + 1));
    }
    ParseNode* newClassNames(ParseNode* outer, ParseNode* inner, const TokenPos& pos) {
        return new_<ClassNames>(outer, inner, pos);
    }
    ParseNode* newNewTarget(ParseNode* newHolder, ParseNode* targetHolder) {
        return new_<BinaryNode>(PNK_NEWTARGET, JSOP_NOP, newHolder, targetHolder);
    }
    ParseNode* newPosHolder(const TokenPos& pos) {
        return new_<NullaryNode>(PNK_POSHOLDER, pos);
    }
    ParseNode* newSuperBase(ParseNode* thisName, const TokenPos& pos) {
        return new_<UnaryNode>(PNK_SUPERBASE, JSOP_NOP, pos, thisName);
    }

    bool addPrototypeMutation(ParseNode* literal, uint32_t begin, ParseNode* expr) {
        // Object literals with mutated [[Prototype]] are non-constant so that
        // singleton objects will have Object.prototype as their [[Prototype]].
        setListFlag(literal, PNX_NONCONST);

        ParseNode* mutation = newUnary(PNK_MUTATEPROTO, JSOP_NOP, begin, expr);
        if (!mutation)
            return false;
        literal->append(mutation);
        return true;
    }

    bool addPropertyDefinition(ParseNode* literal, ParseNode* key, ParseNode* val) {
        MOZ_ASSERT(literal->isKind(PNK_OBJECT));
        MOZ_ASSERT(literal->isArity(PN_LIST));
        MOZ_ASSERT(key->isKind(PNK_NUMBER) ||
                   key->isKind(PNK_OBJECT_PROPERTY_NAME) ||
                   key->isKind(PNK_STRING) ||
                   key->isKind(PNK_COMPUTED_NAME));

        ParseNode* propdef = newBinary(PNK_COLON, key, val, JSOP_INITPROP);
        if (!propdef)
            return false;
        literal->append(propdef);
        return true;
    }

    bool addShorthand(ParseNode* literal, ParseNode* name, ParseNode* expr) {
        MOZ_ASSERT(literal->isKind(PNK_OBJECT));
        MOZ_ASSERT(literal->isArity(PN_LIST));
        MOZ_ASSERT(name->isKind(PNK_OBJECT_PROPERTY_NAME));
        MOZ_ASSERT(expr->isKind(PNK_NAME));
        MOZ_ASSERT(name->pn_atom == expr->pn_atom);

        setListFlag(literal, PNX_NONCONST);
        ParseNode* propdef = newBinary(PNK_SHORTHAND, name, expr, JSOP_INITPROP);
        if (!propdef)
            return false;
        literal->append(propdef);
        return true;
    }

    bool addObjectMethodDefinition(ParseNode* literal, ParseNode* key, ParseNode* fn, JSOp op)
    {
        MOZ_ASSERT(literal->isArity(PN_LIST));
        MOZ_ASSERT(key->isKind(PNK_NUMBER) ||
                   key->isKind(PNK_OBJECT_PROPERTY_NAME) ||
                   key->isKind(PNK_STRING) ||
                   key->isKind(PNK_COMPUTED_NAME));
        literal->pn_xflags |= PNX_NONCONST;

        ParseNode* propdef = newBinary(PNK_COLON, key, fn, op);
        if (!propdef)
            return false;
        literal->append(propdef);
        return true;
    }

    bool addClassMethodDefinition(ParseNode* methodList, ParseNode* key, ParseNode* fn, JSOp op,
                                  bool isStatic)
    {
        MOZ_ASSERT(methodList->isKind(PNK_CLASSMETHODLIST));
        MOZ_ASSERT(key->isKind(PNK_NUMBER) ||
                   key->isKind(PNK_OBJECT_PROPERTY_NAME) ||
                   key->isKind(PNK_STRING) ||
                   key->isKind(PNK_COMPUTED_NAME));

        ParseNode* classMethod = new_<ClassMethod>(key, fn, op, isStatic);
        if (!classMethod)
            return false;
        methodList->append(classMethod);
        return true;
    }

    ParseNode* newYieldExpression(uint32_t begin, ParseNode* value, ParseNode* gen,
                                  JSOp op = JSOP_YIELD) {
        TokenPos pos(begin, value ? value->pn_pos.end : begin + 1);
        return new_<BinaryNode>(PNK_YIELD, op, pos, value, gen);
    }

    ParseNode* newYieldStarExpression(uint32_t begin, ParseNode* value, ParseNode* gen) {
        TokenPos pos(begin, value->pn_pos.end);
        return new_<BinaryNode>(PNK_YIELD_STAR, JSOP_NOP, pos, value, gen);
    }

    // Statements

    ParseNode* newStatementList(unsigned blockid, const TokenPos& pos) {
        ParseNode* pn = new_<ListNode>(PNK_STATEMENTLIST, pos);
        if (pn)
            pn->pn_blockid = blockid;
        return pn;
    }

    template <typename PC>
    void addStatementToList(ParseNode* list, ParseNode* stmt, PC* pc) {
        MOZ_ASSERT(list->isKind(PNK_STATEMENTLIST));

        if (stmt->isKind(PNK_FUNCTION)) {
            if (pc->atBodyLevel()) {
                // PNX_FUNCDEFS notifies the emitter that the block contains
                // body-level function definitions that should be processed
                // before the rest of nodes.
                list->pn_xflags |= PNX_FUNCDEFS;
            } else {
                // General deoptimization was done in Parser::functionDef.
                MOZ_ASSERT_IF(pc->sc->isFunctionBox(),
                              pc->sc->asFunctionBox()->hasExtensibleScope());
            }
        }

        list->append(stmt);
    }

    bool prependInitialYield(ParseNode* stmtList, ParseNode* genName) {
        MOZ_ASSERT(stmtList->isKind(PNK_STATEMENTLIST));

        TokenPos yieldPos(stmtList->pn_pos.begin, stmtList->pn_pos.begin + 1);
        ParseNode* makeGen = new_<NullaryNode>(PNK_GENERATOR, yieldPos);
        if (!makeGen)
            return false;

        MOZ_ASSERT(genName->getOp() == JSOP_GETNAME);
        genName->setOp(JSOP_SETNAME);
        genName->markAsAssigned();
        ParseNode* genInit = newBinary(PNK_ASSIGN, genName, makeGen);
        if (!genInit)
            return false;

        ParseNode* initialYield = newYieldExpression(yieldPos.begin, nullptr, genInit,
                                                     JSOP_INITIALYIELD);
        if (!initialYield)
            return false;

        stmtList->prepend(initialYield);
        return true;
    }

    ParseNode* newSetThis(ParseNode* thisName, ParseNode* val) {
        MOZ_ASSERT(thisName->getOp() == JSOP_GETNAME);
        thisName->setOp(JSOP_SETNAME);
        thisName->markAsAssigned();
        return newBinary(PNK_SETTHIS, thisName, val);
    }

    ParseNode* newEmptyStatement(const TokenPos& pos) {
        return new_<UnaryNode>(PNK_SEMI, JSOP_NOP, pos, (ParseNode*) nullptr);
    }

    ParseNode* newImportDeclaration(ParseNode* importSpecSet,
                                    ParseNode* moduleSpec, const TokenPos& pos)
    {
        ParseNode* pn = new_<BinaryNode>(PNK_IMPORT, JSOP_NOP, pos,
                                         importSpecSet, moduleSpec);
        if (!pn)
            return null();
        return pn;
    }

    ParseNode* newExportDeclaration(ParseNode* kid, const TokenPos& pos) {
        return new_<UnaryNode>(PNK_EXPORT, JSOP_NOP, pos, kid);
    }

    ParseNode* newExportFromDeclaration(uint32_t begin, ParseNode* exportSpecSet,
                                        ParseNode* moduleSpec)
    {
        ParseNode* pn = new_<BinaryNode>(PNK_EXPORT_FROM, JSOP_NOP, exportSpecSet, moduleSpec);
        if (!pn)
            return null();
        pn->pn_pos.begin = begin;
        return pn;
    }

    ParseNode* newExportDefaultDeclaration(ParseNode* kid, ParseNode* maybeBinding,
                                           const TokenPos& pos) {
        return new_<BinaryNode>(PNK_EXPORT_DEFAULT, JSOP_NOP, pos, kid, maybeBinding);
    }

    ParseNode* newExprStatement(ParseNode* expr, uint32_t end) {
        MOZ_ASSERT(expr->pn_pos.end <= end);
        return new_<UnaryNode>(PNK_SEMI, JSOP_NOP, TokenPos(expr->pn_pos.begin, end), expr);
    }

    ParseNode* newIfStatement(uint32_t begin, ParseNode* cond, ParseNode* thenBranch,
                              ParseNode* elseBranch)
    {
        ParseNode* pn = new_<TernaryNode>(PNK_IF, JSOP_NOP, cond, thenBranch, elseBranch);
        if (!pn)
            return null();
        pn->pn_pos.begin = begin;
        return pn;
    }

    ParseNode* newDoWhileStatement(ParseNode* body, ParseNode* cond, const TokenPos& pos) {
        return new_<BinaryNode>(PNK_DOWHILE, JSOP_NOP, pos, body, cond);
    }

    ParseNode* newWhileStatement(uint32_t begin, ParseNode* cond, ParseNode* body) {
        TokenPos pos(begin, body->pn_pos.end);
        return new_<BinaryNode>(PNK_WHILE, JSOP_NOP, pos, cond, body);
    }

    ParseNode* newForStatement(uint32_t begin, ParseNode* forHead, ParseNode* body,
                               unsigned iflags)
    {
        /* A FOR node is binary, left is loop control and right is the body. */
        JSOp op = forHead->isKind(PNK_FORIN) ? JSOP_ITER : JSOP_NOP;
        BinaryNode* pn = new_<BinaryNode>(PNK_FOR, op, TokenPos(begin, body->pn_pos.end),
                                          forHead, body);
        if (!pn)
            return null();
        pn->pn_iflags = iflags;
        return pn;
    }

    ParseNode* newComprehensionFor(uint32_t begin, ParseNode* forHead, ParseNode* body) {
        // A PNK_COMPREHENSIONFOR node is binary: left is loop control, right
        // is the body.
        MOZ_ASSERT(forHead->isKind(PNK_FORIN) || forHead->isKind(PNK_FOROF));
        JSOp op = forHead->isKind(PNK_FORIN) ? JSOP_ITER : JSOP_NOP;
        BinaryNode* pn = new_<BinaryNode>(PNK_COMPREHENSIONFOR, op,
                                          TokenPos(begin, body->pn_pos.end), forHead, body);
        if (!pn)
            return null();
        pn->pn_iflags = JSOP_ITER;
        return pn;
    }

    ParseNode* newForHead(ParseNodeKind kind, ParseNode* pn1, ParseNode* pn2, ParseNode* pn3,
                          const TokenPos& pos)
    {
        MOZ_ASSERT(kind == PNK_FORIN || kind == PNK_FOROF || kind == PNK_FORHEAD);
        return new_<TernaryNode>(kind, JSOP_NOP, pn1, pn2, pn3, pos);
    }

    ParseNode* newSwitchStatement(uint32_t begin, ParseNode* discriminant, ParseNode* caseList) {
        TokenPos pos(begin, caseList->pn_pos.end);
        return new_<BinaryNode>(PNK_SWITCH, JSOP_NOP, pos, discriminant, caseList);
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
        return new_<UnaryNode>(PNK_RETURN, JSOP_RETURN, pos, expr);
    }

    ParseNode* newWithStatement(uint32_t begin, ParseNode* expr, ParseNode* body,
                                ObjectBox* staticWith) {
        return new_<BinaryObjNode>(PNK_WITH, JSOP_NOP, TokenPos(begin, body->pn_pos.end),
                                   expr, body, staticWith);
    }

    ParseNode* newLabeledStatement(PropertyName* label, ParseNode* stmt, uint32_t begin) {
        return new_<LabeledStatement>(label, stmt, begin);
    }

    ParseNode* newThrowStatement(ParseNode* expr, const TokenPos& pos) {
        MOZ_ASSERT(pos.encloses(expr->pn_pos));
        return new_<UnaryNode>(PNK_THROW, JSOP_THROW, pos, expr);
    }

    ParseNode* newTryStatement(uint32_t begin, ParseNode* body, ParseNode* catchList,
                               ParseNode* finallyBlock) {
        TokenPos pos(begin, (finallyBlock ? finallyBlock : catchList)->pn_pos.end);
        return new_<TernaryNode>(PNK_TRY, JSOP_NOP, body, catchList, finallyBlock, pos);
    }

    ParseNode* newDebuggerStatement(const TokenPos& pos) {
        return new_<DebuggerStatement>(pos);
    }

    ParseNode* newPropertyAccess(ParseNode* pn, PropertyName* name, uint32_t end) {
        return new_<PropertyAccess>(pn, name, pn->pn_pos.begin, end);
    }

    ParseNode* newPropertyByValue(ParseNode* lhs, ParseNode* index, uint32_t end) {
        return new_<PropertyByValue>(lhs, index, lhs->pn_pos.begin, end);
    }

    inline bool addCatchBlock(ParseNode* catchList, ParseNode* letBlock,
                              ParseNode* catchName, ParseNode* catchGuard, ParseNode* catchBody);

    inline bool setLastFunctionArgumentDefault(ParseNode* funcpn, ParseNode* pn);
    inline void setLastFunctionArgumentDestructuring(ParseNode* funcpn, ParseNode* pn);

    ParseNode* newFunctionDefinition() {
        return new_<CodeNode>(PNK_FUNCTION, pos());
    }
    void setFunctionBody(ParseNode* pn, ParseNode* kid) {
        pn->pn_body = kid;
    }
    void setFunctionBox(ParseNode* pn, FunctionBox* funbox) {
        MOZ_ASSERT(pn->isKind(PNK_FUNCTION));
        pn->pn_funbox = funbox;
    }
    void addFunctionArgument(ParseNode* pn, ParseNode* argpn) {
        pn->pn_body->append(argpn);
    }
    void setDerivedClassConstructor(ParseNode* pn) {
        MOZ_ASSERT(pn->isKind(PNK_FUNCTION));
        pn->pn_funbox->setDerivedClassConstructor();
    }

    ParseNode* newModule() {
        return new_<CodeNode>(PNK_MODULE, pos());
    }
    void setModuleBox(ParseNode* pn, ModuleBox* modulebox) {
        MOZ_ASSERT(pn->isKind(PNK_MODULE));
        pn->pn_modulebox = modulebox;
    }

    ParseNode* newLexicalScope(ObjectBox* blockBox) {
        return new_<LexicalScopeNode>(blockBox, pos());
    }
    void setLexicalScopeBody(ParseNode* block, ParseNode* body) {
        block->pn_expr = body;
    }

    ParseNode* newLetBlock(ParseNode* vars, ParseNode* block, const TokenPos& pos) {
        ParseNode* letBlock = newBinary(PNK_LETBLOCK, vars, block);
        if (!letBlock)
            return nullptr;
        letBlock->pn_pos = pos;
        return letBlock;
    }

    ParseNode* newAssignment(ParseNodeKind kind, ParseNode* lhs, ParseNode* rhs,
                             ParseContext<FullParseHandler>* pc, JSOp op)
    {
        return newBinary(kind, lhs, rhs, op);
    }

    bool isUnparenthesizedYieldExpression(ParseNode* node) {
        return node->isKind(PNK_YIELD) && !node->isInParens();
    }

    bool isUnparenthesizedCommaExpression(ParseNode* node) {
        return node->isKind(PNK_COMMA) && !node->isInParens();
    }

    bool isUnparenthesizedAssignment(Node node) {
        if (node->isKind(PNK_ASSIGN) && !node->isInParens()) {
            // PNK_ASSIGN is also (mis)used for things like |var name = expr;|.
            // But this method is only called on actual expressions, so we can
            // just assert the node's op is the one used for plain assignment.
            MOZ_ASSERT(node->isOp(JSOP_NOP));
            return true;
        }

        return false;
    }

    bool isReturnStatement(ParseNode* node) {
        return node->isKind(PNK_RETURN);
    }

    bool isStatementPermittedAfterReturnStatement(ParseNode *node) {
        ParseNodeKind kind = node->getKind();
        return kind == PNK_FUNCTION || kind == PNK_VAR || kind == PNK_BREAK || kind == PNK_THROW ||
               (kind == PNK_SEMI && !node->pn_kid);
    }

    bool isSuperBase(ParseNode* node) {
        return node->isKind(PNK_SUPERBASE);
    }

    inline bool finishInitializerAssignment(ParseNode* pn, ParseNode* init, JSOp op);
    inline void setLexicalDeclarationOp(ParseNode* pn, JSOp op);

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

    void setPosition(ParseNode* pn, const TokenPos& pos) {
        pn->pn_pos = pos;
    }
    TokenPos getPosition(ParseNode* pn) {
        return pn->pn_pos;
    }

    ParseNode* newList(ParseNodeKind kind, JSOp op = JSOP_NOP) {
        MOZ_ASSERT(kind != PNK_VAR);
        return new_<ListNode>(kind, op, pos());
    }
    ParseNode* newList(ParseNodeKind kind, uint32_t begin, JSOp op = JSOP_NOP) {
        return new_<ListNode>(kind, op, TokenPos(begin, begin + 1));
    }
    ParseNode* newDeclarationList(ParseNodeKind kind, JSOp op = JSOP_NOP) {
        MOZ_ASSERT(kind == PNK_VAR || kind == PNK_CONST || kind == PNK_LET);
        return new_<ListNode>(kind, op, pos());
    }

    /* New list with one initial child node. kid must be non-null. */
    ParseNode* newList(ParseNodeKind kind, ParseNode* kid, JSOp op = JSOP_NOP) {
        MOZ_ASSERT(kind != PNK_VAR);
        return new_<ListNode>(kind, op, kid);
    }
    ParseNode* newDeclarationList(ParseNodeKind kind, ParseNode* kid, JSOp op = JSOP_NOP) {
        MOZ_ASSERT(kind == PNK_VAR || kind == PNK_CONST || kind == PNK_LET);
        return new_<ListNode>(kind, op, kid);
    }

    ParseNode* newCatchList() {
        return new_<ListNode>(PNK_CATCHLIST, JSOP_NOP, pos());
    }

    ParseNode* newCommaExpressionList(ParseNode* kid) {
        return newList(PNK_COMMA, kid, JSOP_NOP);
    }

    void addList(ParseNode* list, ParseNode* kid) {
        list->append(kid);
    }

    void setOp(ParseNode* pn, JSOp op) {
        pn->setOp(op);
    }
    void setBlockId(ParseNode* pn, unsigned blockid) {
        pn->pn_blockid = blockid;
    }
    void setFlag(ParseNode* pn, unsigned flag) {
        pn->pn_dflags |= flag;
    }
    void setListFlag(ParseNode* pn, unsigned flag) {
        MOZ_ASSERT(pn->isArity(PN_LIST));
        pn->pn_xflags |= flag;
    }
    MOZ_WARN_UNUSED_RESULT ParseNode* parenthesize(ParseNode* pn) {
        pn->setInParens(true);
        return pn;
    }
    MOZ_WARN_UNUSED_RESULT ParseNode* setLikelyIIFE(ParseNode* pn) {
        return parenthesize(pn);
    }
    void setPrologue(ParseNode* pn) {
        pn->pn_prologue = true;
    }

    bool isConstant(ParseNode* pn) {
        return pn->isConstant();
    }

    PropertyName* maybeUnparenthesizedName(ParseNode* pn) {
        if (!pn->isInParens() && pn->isKind(PNK_NAME))
            return pn->pn_atom->asPropertyName();
        return nullptr;
    }

    PropertyName* maybeParenthesizedName(ParseNode* pn) {
        if (pn->isInParens() && pn->isKind(PNK_NAME))
            return pn->pn_atom->asPropertyName();
        return nullptr;
    }

    PropertyName* maybeNameAnyParentheses(ParseNode* node) {
        if (PropertyName* name = maybeUnparenthesizedName(node))
            return name;
        return maybeParenthesizedName(node);
    }

    bool isCall(ParseNode* pn) {
        return pn->isKind(PNK_CALL);
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

    void markAsAssigned(ParseNode* node) { node->markAsAssigned(); }
    void adjustGetToSet(ParseNode* node) {
        node->setOp(node->isOp(JSOP_GETLOCAL) ? JSOP_SETLOCAL : JSOP_SETNAME);
    }
    void maybeDespecializeSet(ParseNode* node) {
        if (!(CodeSpec[node->getOp()].format & JOF_SET))
            node->setOp(JSOP_SETNAME);
    }

    inline ParseNode* makeAssignment(ParseNode* pn, ParseNode* rhs);

    static Definition* getDefinitionNode(Definition* dn) {
        return dn;
    }
    static Definition::Kind getDefinitionKind(Definition* dn) {
        return dn->kind();
    }
    static bool isPlaceholderDefinition(Definition* dn) {
        return dn->isPlaceholder();
    }
    void linkUseToDef(ParseNode* pn, Definition* dn)
    {
        MOZ_ASSERT(!pn->isUsed());
        MOZ_ASSERT(!pn->isDefn());
        MOZ_ASSERT(pn != dn->dn_uses);
        MOZ_ASSERT(dn->isDefn());
        pn->pn_link = dn->dn_uses;
        dn->dn_uses = pn;
        dn->pn_dflags |= pn->pn_dflags & PND_USE2DEF_FLAGS;
        pn->setUsed(true);
        pn->pn_lexdef = dn;
    }
    Definition* resolve(Definition* dn) {
        return dn->resolve();
    }
    void deoptimizeUsesWithin(Definition* dn, const TokenPos& pos)
    {
        for (ParseNode* pnu = dn->dn_uses; pnu; pnu = pnu->pn_link) {
            MOZ_ASSERT(pnu->isUsed());
            MOZ_ASSERT(!pnu->isDefn());
            if (pnu->pn_pos.begin >= pos.begin && pnu->pn_pos.end <= pos.end)
                pnu->pn_dflags |= PND_DEOPTIMIZED;
        }
    }
    bool dependencyCovered(ParseNode* pn, unsigned blockid, bool functionScope) {
        return pn->pn_blockid >= blockid;
    }
    void markMaybeUninitializedLexicalUseInSwitch(ParseNode* pn, Definition* dn,
                                                  uint16_t firstDominatingLexicalSlot)
    {
        MOZ_ASSERT(pn->isUsed());
        if (dn->isLexical() && dn->pn_scopecoord.slot() < firstDominatingLexicalSlot)
            pn->pn_dflags |= PND_LEXICAL;
    }

    static uintptr_t definitionToBits(Definition* dn) {
        return uintptr_t(dn);
    }
    static Definition* definitionFromBits(uintptr_t bits) {
        return (Definition*) bits;
    }
    static Definition* nullDefinition() {
        return nullptr;
    }
    void disableSyntaxParser() {
        syntaxParser = nullptr;
    }

    LazyScript* lazyOuterFunction() {
        return lazyOuterFunction_;
    }
    JSFunction* nextLazyInnerFunction() {
        MOZ_ASSERT(lazyInnerFunctionIndex < lazyOuterFunction()->numInnerFunctions());
        return lazyOuterFunction()->innerFunctions()[lazyInnerFunctionIndex++];
    }
};

inline bool
FullParseHandler::addCatchBlock(ParseNode* catchList, ParseNode* letBlock,
                                ParseNode* catchName, ParseNode* catchGuard, ParseNode* catchBody)
{
    ParseNode* catchpn = newTernary(PNK_CATCH, catchName, catchGuard, catchBody);
    if (!catchpn)
        return false;

    catchList->append(letBlock);
    letBlock->pn_expr = catchpn;
    return true;
}

inline ParseNode*
FullParseHandler::makeAssignmentFromArg(ParseNode* arg, ParseNode* lhs, ParseNode* rhs)
{
    return newBinary(PNK_ASSIGN, lhs, rhs, JSOP_NOP);
}

inline void
FullParseHandler::replaceLastFunctionArgument(ParseNode* funcpn, ParseNode* pn)
{
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
}

inline bool
FullParseHandler::setLastFunctionArgumentDefault(ParseNode* funcpn, ParseNode* defaultValue)
{
    ParseNode* arg = funcpn->pn_body->last();
    MOZ_ASSERT(arg->isKind(PNK_NAME));
    ParseNode* lhs = arg->pn_expr ? arg->pn_expr : arg;
    ParseNode* pn = makeAssignmentFromArg(arg, lhs, defaultValue);
    if (!pn)
        return false;

    if (arg->pn_expr)
        arg->pn_expr = pn;
    else
        replaceLastFunctionArgument(funcpn, pn);
    return true;
}

inline void
FullParseHandler::setLastFunctionArgumentDestructuring(ParseNode* funcpn, ParseNode* destruct)
{
    ParseNode* arg = funcpn->pn_body->last();
    MOZ_ASSERT(arg->isKind(PNK_NAME));
    MOZ_ASSERT(!arg->isUsed());
    MOZ_ASSERT(arg->isDefn());
    arg->pn_expr = destruct;
}

inline bool
FullParseHandler::finishInitializerAssignment(ParseNode* pn, ParseNode* init, JSOp op)
{
    if (pn->isUsed()) {
        pn = makeAssignment(pn, init);
        if (!pn)
            return false;
    } else {
        pn->pn_expr = init;
    }

    if (pn->pn_dflags & PND_BOUND)
        pn->setOp(JSOP_SETLOCAL);
    else
        pn->setOp(JSOP_SETNAME);

    pn->markAsAssigned();

    /* The declarator's position must include the initializer. */
    pn->pn_pos.end = init->pn_pos.end;
    return true;
}

inline void
FullParseHandler::setLexicalDeclarationOp(ParseNode* pn, JSOp op)
{
    if (op == JSOP_DEFLET || op == JSOP_DEFCONST) {
        // Subtlety here. Lexical definitions that are PND_BOUND but whose
        // scope coordinates are free are global lexicals. They cannot use
        // scope coordinate lookup because we rely on being able to clone
        // scripts to run on multiple globals. However, they always go on the
        // global lexical scope, so in that sense they are bound.
        pn->setOp(pn->pn_scopecoord.isFree() ? JSOP_INITGLEXICAL : JSOP_INITLEXICAL);
    }
}

inline ParseNode*
FullParseHandler::makeAssignment(ParseNode* pn, ParseNode* rhs)
{
    ParseNode* lhs = cloneNode(*pn);
    if (!lhs)
        return nullptr;

    if (pn->isUsed()) {
        Definition* dn = pn->pn_lexdef;
        ParseNode** pnup = &dn->dn_uses;

        while (*pnup != pn)
            pnup = &(*pnup)->pn_link;
        *pnup = lhs;
        lhs->pn_link = pn->pn_link;
        pn->pn_link = nullptr;
    }

    pn->setKind(PNK_ASSIGN);
    pn->setOp(JSOP_NOP);
    pn->setArity(PN_BINARY);
    pn->setInParens(false);
    pn->setUsed(false);
    pn->setDefn(false);
    pn->pn_left = lhs;
    pn->pn_right = rhs;
    pn->pn_pos.end = rhs->pn_pos.end;
    return lhs;
}

} // namespace frontend
} // namespace js

#endif /* frontend_FullParseHandler_h */
