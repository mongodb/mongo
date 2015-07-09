/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_SyntaxParseHandler_h
#define frontend_SyntaxParseHandler_h

#include "mozilla/Attributes.h"

#include "frontend/ParseNode.h"
#include "frontend/TokenStream.h"

namespace js {
namespace frontend {

template <typename ParseHandler>
class Parser;

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
class SyntaxParseHandler
{
    // Remember the last encountered name or string literal during syntax parses.
    JSAtom* lastAtom;
    TokenPos lastStringPos;
    TokenStream& tokenStream;

  public:
    enum Node {
        NodeFailure = 0,
        NodeGeneric,
        NodeName,
        NodeGetProp,
        NodeStringExprStatement,
        NodeLValue,

        // In rare cases a parenthesized |node| doesn't have the same semantics
        // as |node|.  Each such node has a special Node value, and we use a
        // different Node value to represent the parenthesized form.  See also
        // isUnparenthesized*(Node), newExprStatement(Node, uint32_t),
        // parenthesize(Node), and meaningMightChangeIfParenthesized(Node).

        // The directive prologue at the start of a FunctionBody or ScriptBody
        // is the longest sequence (possibly empty) of string literal
        // expression statements at the start of a function.  Thus we need this
        // to treat |"use strict";| as a possible Use Strict Directive and
        // |("use strict");| as a useless statement.
        NodeUnparenthesizedString,

        // Legacy generator expressions of the form |(expr for (...))| and
        // array comprehensions of the form |[expr for (...)]|) don't permit
        // |expr| to be a comma expression.  Thus we need this to treat
        // |(a(), b for (x in []))| as a syntax error and
        // |((a(), b) for (x in []))| as a generator that calls |a| and then
        // yields |b| each time it's resumed.
        NodeUnparenthesizedCommaExpr,

        // Yield expressions currently (but not in ES6 -- a SpiderMonkey bug to
        // fix) must generally be parenthesized.  (See the uses of
        // isUnparenthesizedYieldExpression in Parser.cpp for the rare
        // exceptions.)  Thus we need this to treat |yield 1, 2;| as a syntax
        // error and |(yield 1), 2;| as a comma expression that will yield 1,
        // then evaluate to 2.
        NodeUnparenthesizedYieldExpr,

        // Assignment expressions in condition contexts could be typos for
        // equality checks.  (Think |if (x = y)| versus |if (x == y)|.)  Thus
        // we need this to treat |if (x = y)| as a possible typo and
        // |if ((x = y))| as a deliberate assignment within a condition.
        //
        // (Technically this isn't needed, as these are *only* extraWarnings
        // warnings, and parsing with that option disables syntax parsing.  But
        // it seems best to be consistent, and perhaps the syntax parser will
        // eventually enforce extraWarnings and will require this then.)
        NodeUnparenthesizedAssignment
    };
    typedef Definition::Kind DefinitionNode;

  private:
    static bool meaningMightChangeIfParenthesized(Node node) {
        return node == NodeUnparenthesizedString ||
               node == NodeUnparenthesizedCommaExpr ||
               node == NodeUnparenthesizedYieldExpr ||
               node == NodeUnparenthesizedAssignment;
    }


  public:
    SyntaxParseHandler(ExclusiveContext* cx, LifoAlloc& alloc,
                       TokenStream& tokenStream, Parser<SyntaxParseHandler>* syntaxParser,
                       LazyScript* lazyOuterFunction)
      : lastAtom(nullptr),
        tokenStream(tokenStream)
    {}

    static Node null() { return NodeFailure; }

    void trace(JSTracer* trc) {}

    Node newName(PropertyName* name, uint32_t blockid, const TokenPos& pos) {
        lastAtom = name;
        return NodeName;
    }

    Node newComputedName(Node expr, uint32_t start, uint32_t end) {
        return NodeName;
    }

    DefinitionNode newPlaceholder(JSAtom* atom, uint32_t blockid, const TokenPos& pos) {
        return Definition::PLACEHOLDER;
    }

    Node newObjectLiteralPropertyName(JSAtom* atom, const TokenPos& pos) {
        return NodeName;
    }

    Node newNumber(double value, DecimalPoint decimalPoint, const TokenPos& pos) { return NodeGeneric; }
    Node newBooleanLiteral(bool cond, const TokenPos& pos) { return NodeGeneric; }

    Node newStringLiteral(JSAtom* atom, const TokenPos& pos) {
        lastAtom = atom;
        lastStringPos = pos;
        return NodeUnparenthesizedString;
    }

    Node newTemplateStringLiteral(JSAtom* atom, const TokenPos& pos) {
        return NodeGeneric;
    }

    Node newCallSiteObject(uint32_t begin, unsigned blockidGen) {
        return NodeGeneric;
    }

    bool addToCallSiteObject(Node callSiteObj, Node rawNode, Node cookedNode) {
        return true;
    }

    Node newThisLiteral(const TokenPos& pos) { return NodeGeneric; }
    Node newNullLiteral(const TokenPos& pos) { return NodeGeneric; }

    template <class Boxer>
    Node newRegExp(RegExpObject* reobj, const TokenPos& pos, Boxer& boxer) { return NodeGeneric; }

    Node newConditional(Node cond, Node thenExpr, Node elseExpr) { return NodeGeneric; }

    Node newElision() { return NodeGeneric; }

    Node newDelete(uint32_t begin, Node expr) { return NodeGeneric; }

    Node newUnary(ParseNodeKind kind, JSOp op, uint32_t begin, Node kid) {
        return NodeGeneric;
    }

    Node newBinary(ParseNodeKind kind, JSOp op = JSOP_NOP) { return NodeGeneric; }
    Node newBinary(ParseNodeKind kind, Node left, JSOp op = JSOP_NOP) { return NodeGeneric; }
    Node newBinary(ParseNodeKind kind, Node left, Node right, JSOp op = JSOP_NOP) {
        return NodeGeneric;
    }
    Node appendOrCreateList(ParseNodeKind kind, Node left, Node right,
                            ParseContext<SyntaxParseHandler>* pc, JSOp op = JSOP_NOP) {
        return NodeGeneric;
    }

    Node newTernary(ParseNodeKind kind, Node first, Node second, Node third, JSOp op = JSOP_NOP) {
        return NodeGeneric;
    }

    // Expressions

    Node newArrayComprehension(Node body, unsigned blockid, const TokenPos& pos) {
        return NodeGeneric;
    }
    Node newArrayLiteral(uint32_t begin, unsigned blockid) { return NodeGeneric; }
    bool addElision(Node literal, const TokenPos& pos) { return true; }
    bool addSpreadElement(Node literal, uint32_t begin, Node inner) { return true; }
    void addArrayElement(Node literal, Node element) { }

    Node newObjectLiteral(uint32_t begin) { return NodeGeneric; }
    bool addPrototypeMutation(Node literal, uint32_t begin, Node expr) { return true; }
    bool addPropertyDefinition(Node literal, Node name, Node expr) { return true; }
    bool addShorthand(Node literal, Node name, Node expr) { return true; }
    bool addMethodDefinition(Node literal, Node name, Node fn, JSOp op) { return true; }
    Node newYieldExpression(uint32_t begin, Node value, Node gen) { return NodeUnparenthesizedYieldExpr; }
    Node newYieldStarExpression(uint32_t begin, Node value, Node gen) { return NodeGeneric; }

    // Statements

    Node newStatementList(unsigned blockid, const TokenPos& pos) { return NodeGeneric; }
    void addStatementToList(Node list, Node stmt, ParseContext<SyntaxParseHandler>* pc) {}
    bool prependInitialYield(Node stmtList, Node gen) { return true; }
    Node newEmptyStatement(const TokenPos& pos) { return NodeGeneric; }

    Node newExprStatement(Node expr, uint32_t end) {
        return expr == NodeUnparenthesizedString ? NodeStringExprStatement : NodeGeneric;
    }

    Node newIfStatement(uint32_t begin, Node cond, Node then, Node else_) { return NodeGeneric; }
    Node newDoWhileStatement(Node body, Node cond, const TokenPos& pos) { return NodeGeneric; }
    Node newWhileStatement(uint32_t begin, Node cond, Node body) { return NodeGeneric; }
    Node newSwitchStatement(uint32_t begin, Node discriminant, Node caseList) { return NodeGeneric; }
    Node newCaseOrDefault(uint32_t begin, Node expr, Node body) { return NodeGeneric; }
    Node newContinueStatement(PropertyName* label, const TokenPos& pos) { return NodeGeneric; }
    Node newBreakStatement(PropertyName* label, const TokenPos& pos) { return NodeGeneric; }
    Node newReturnStatement(Node expr, Node genrval, const TokenPos& pos) { return NodeGeneric; }

    Node newLabeledStatement(PropertyName* label, Node stmt, uint32_t begin) {
        return NodeGeneric;
    }

    Node newThrowStatement(Node expr, const TokenPos& pos) { return NodeGeneric; }
    Node newTryStatement(uint32_t begin, Node body, Node catchList, Node finallyBlock) {
        return NodeGeneric;
    }
    Node newDebuggerStatement(const TokenPos& pos) { return NodeGeneric; }

    Node newPropertyAccess(Node pn, PropertyName* name, uint32_t end) {
        lastAtom = name;
        return NodeGetProp;
    }

    Node newPropertyByValue(Node pn, Node kid, uint32_t end) { return NodeLValue; }

    bool addCatchBlock(Node catchList, Node letBlock,
                       Node catchName, Node catchGuard, Node catchBody) { return true; }

    void setLastFunctionArgumentDefault(Node funcpn, Node pn) {}
    Node newFunctionDefinition() { return NodeGeneric; }
    void setFunctionBody(Node pn, Node kid) {}
    void setFunctionBox(Node pn, FunctionBox* funbox) {}
    void addFunctionArgument(Node pn, Node argpn) {}

    Node newForStatement(uint32_t begin, Node forHead, Node body, unsigned iflags) {
        return NodeGeneric;
    }

    Node newForHead(ParseNodeKind kind, Node decls, Node lhs, Node rhs, const TokenPos& pos) {
        return NodeGeneric;
    }

    Node newLexicalScope(ObjectBox* blockbox) { return NodeGeneric; }
    void setLexicalScopeBody(Node block, Node body) {}

    Node newLetExpression(Node vars, Node block, const TokenPos& pos) {
        return NodeGeneric;
    }

    Node newLetBlock(Node vars, Node block, const TokenPos& pos) {
        return NodeGeneric;
    }

    bool finishInitializerAssignment(Node pn, Node init, JSOp op) { return true; }

    void setBeginPosition(Node pn, Node oth) {}
    void setBeginPosition(Node pn, uint32_t begin) {}

    void setEndPosition(Node pn, Node oth) {}
    void setEndPosition(Node pn, uint32_t end) {}


    void setPosition(Node pn, const TokenPos& pos) {}
    TokenPos getPosition(Node pn) {
        return tokenStream.currentToken().pos;
    }

    Node newList(ParseNodeKind kind, JSOp op = JSOP_NOP) {
        return NodeGeneric;
    }
    Node newList(ParseNodeKind kind, Node kid, JSOp op = JSOP_NOP) {
        return NodeGeneric;
    }

    Node newCatchList() {
        return newList(PNK_CATCHLIST, JSOP_NOP);
    }

    Node newCommaExpressionList(Node kid) {
        return NodeUnparenthesizedCommaExpr;
    }

    void addList(Node list, Node kid) {
        MOZ_ASSERT(list == NodeGeneric || list == NodeUnparenthesizedCommaExpr);
    }

    Node newAssignment(ParseNodeKind kind, Node lhs, Node rhs,
                       ParseContext<SyntaxParseHandler>* pc, JSOp op)
    {
        if (kind == PNK_ASSIGN)
            return NodeUnparenthesizedAssignment;
        return newBinary(kind, lhs, rhs, op);
    }

    bool isUnparenthesizedYieldExpression(Node node) {
        return node == NodeUnparenthesizedYieldExpr;
    }

    bool isUnparenthesizedCommaExpression(Node node) {
        return node == NodeUnparenthesizedCommaExpr;
    }

    bool isUnparenthesizedAssignment(Node node) {
        return node == NodeUnparenthesizedAssignment;
    }

    void setOp(Node pn, JSOp op) {}
    void setBlockId(Node pn, unsigned blockid) {}
    void setFlag(Node pn, unsigned flag) {}
    void setListFlag(Node pn, unsigned flag) {}
    MOZ_WARN_UNUSED_RESULT Node parenthesize(Node node) {
        if (meaningMightChangeIfParenthesized(node))
            return NodeGeneric;

        // In all other cases, the parenthesized form of |node| is equivalent
        // to the unparenthesized form: return |node| unchanged.
        return node;
    }
    MOZ_WARN_UNUSED_RESULT Node setLikelyIIFE(Node pn) {
        return pn; // Remain in syntax-parse mode.
    }
    void setPrologue(Node pn) {}

    bool isConstant(Node pn) { return false; }
    PropertyName* isName(Node pn) {
        return (pn == NodeName) ? lastAtom->asPropertyName() : nullptr;
    }
    PropertyName* isGetProp(Node pn) {
        return (pn == NodeGetProp) ? lastAtom->asPropertyName() : nullptr;
    }
    JSAtom* isStringExprStatement(Node pn, TokenPos* pos) {
        if (pn == NodeStringExprStatement) {
            *pos = lastStringPos;
            return lastAtom;
        }
        return nullptr;
    }

    Node makeAssignment(Node pn, Node rhs) { return NodeGeneric; }

    static Node getDefinitionNode(DefinitionNode dn) { return NodeGeneric; }
    static Definition::Kind getDefinitionKind(DefinitionNode dn) { return dn; }
    static bool isPlaceholderDefinition(DefinitionNode dn) { return dn == Definition::PLACEHOLDER; }
    void linkUseToDef(Node pn, DefinitionNode dn) {}
    DefinitionNode resolve(DefinitionNode dn) { return dn; }
    void deoptimizeUsesWithin(DefinitionNode dn, const TokenPos& pos) {}
    bool dependencyCovered(Node pn, unsigned blockid, bool functionScope) {
        // Only resolve lexical dependencies in cases where a definition covers
        // the entire function. Not enough information is kept to compare the
        // dependency location with blockid.
        return functionScope;
    }
    void markMaybeUninitializedLexicalUseInSwitch(Node pn, DefinitionNode dn,
                                                  uint16_t firstDominatingLexicalSlot) {}

    static uintptr_t definitionToBits(DefinitionNode dn) {
        // Use a shift, as DefinitionList tags the lower bit of its associated union.
        return uintptr_t(dn << 1);
    }
    static DefinitionNode definitionFromBits(uintptr_t bits) {
        return (DefinitionNode) (bits >> 1);
    }
    static DefinitionNode nullDefinition() {
        return Definition::MISSING;
    }
    void disableSyntaxParser() {
    }
};

} // namespace frontend
} // namespace js

#endif /* frontend_SyntaxParseHandler_h */
