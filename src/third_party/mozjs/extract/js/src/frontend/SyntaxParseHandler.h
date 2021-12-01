/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_SyntaxParseHandler_h
#define frontend_SyntaxParseHandler_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <string.h>

#include "frontend/ParseNode.h"
#include "js/GCAnnotations.h"
#include "vm/JSContext.h"

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
class SyntaxParseHandler
{
    // Remember the last encountered name or string literal during syntax parses.
    JSAtom* lastAtom;
    TokenPos lastStringPos;

    // WARNING: Be careful about adding fields to this function, that might be
    //          GC things (like JSAtom*).  The JS_HAZ_ROOTED causes the GC
    //          analysis to *ignore* anything that might be a rooting hazard in
    //          this class.  The |lastAtom| field above is safe because
    //          SyntaxParseHandler only appears as a field in
    //          PerHandlerParser<SyntaxParseHandler>, and that class inherits
    //          from ParserBase which contains an AutoKeepAtoms field that
    //          prevents atoms from being moved around while the AutoKeepAtoms
    //          lives -- which is as long as ParserBase lives, which is longer
    //          than the PerHandlerParser<SyntaxParseHandler> that inherits
    //          from it will live.

  public:
    enum Node {
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
        NodeFunctionExpressionBlockBody,

        // A non-arrow function expression with AssignmentExpression body -- a
        // proprietary SpiderMonkey extension.
        NodeFunctionExpressionClosure,

        NodeFunctionArrow,
        NodeFunctionStatement,

        // This is needed for proper assignment-target handling.  ES6 formally
        // requires function calls *not* pass IsValidSimpleAssignmentTarget,
        // but at last check there were still sites with |f() = 5| and similar
        // in code not actually executed (or at least not executed enough to be
        // noticed).
        NodeFunctionCall,

        // Node representing normal names which don't require any special
        // casing.
        NodeName,

        // Nodes representing the names "arguments" and "eval".
        NodeArgumentsName,
        NodeEvalName,

        // Node representing the "async" name, which may actually be a
        // contextual keyword.
        NodePotentialAsyncKeyword,

        NodeDottedProperty,
        NodeElement,

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

        // Assignment expressions in condition contexts could be typos for
        // equality checks.  (Think |if (x = y)| versus |if (x == y)|.)  Thus
        // we need this to treat |if (x = y)| as a possible typo and
        // |if ((x = y))| as a deliberate assignment within a condition.
        //
        // (Technically this isn't needed, as these are *only* extraWarnings
        // warnings, and parsing with that option disables syntax parsing.  But
        // it seems best to be consistent, and perhaps the syntax parser will
        // eventually enforce extraWarnings and will require this then.)
        NodeUnparenthesizedAssignment,

        // This node is necessary to determine if the base operand in an
        // exponentiation operation is an unparenthesized unary expression.
        // We want to reject |-2 ** 3|, but still need to allow |(-2) ** 3|.
        NodeUnparenthesizedUnary,

        // This node is necessary to determine if the LHS of a property access is
        // super related.
        NodeSuperBase
    };

    bool isNonArrowFunctionExpression(Node node) const {
        return node == NodeFunctionExpressionBlockBody || node == NodeFunctionExpressionClosure;
    }

    bool isPropertyAccess(Node node) {
        return node == NodeDottedProperty || node == NodeElement;
    }

    bool isFunctionCall(Node node) {
        // Note: super() is a special form, *not* a function call.
        return node == NodeFunctionCall;
    }

    static bool isUnparenthesizedDestructuringPattern(Node node) {
        return node == NodeUnparenthesizedArray || node == NodeUnparenthesizedObject;
    }

    static bool isParenthesizedDestructuringPattern(Node node) {
        // Technically this isn't a destructuring target at all -- the grammar
        // doesn't treat it as such.  But we need to know when this happens to
        // consider it a SyntaxError rather than an invalid-left-hand-side
        // ReferenceError.
        return node == NodeParenthesizedArray || node == NodeParenthesizedObject;
    }

  public:
    SyntaxParseHandler(JSContext* cx, LifoAlloc& alloc, LazyScript* lazyOuterFunction)
      : lastAtom(nullptr)
    {}

    static Node null() { return NodeFailure; }

    void prepareNodeForMutation(Node node) {}
    void freeTree(Node node) {}

    Node newName(PropertyName* name, const TokenPos& pos, JSContext* cx) {
        lastAtom = name;
        if (name == cx->names().arguments)
            return NodeArgumentsName;
        if (pos.begin + strlen("async") == pos.end && name == cx->names().async)
            return NodePotentialAsyncKeyword;
        if (name == cx->names().eval)
            return NodeEvalName;
        return NodeName;
    }

    Node newComputedName(Node expr, uint32_t start, uint32_t end) {
        return NodeGeneric;
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

    Node newCallSiteObject(uint32_t begin) {
        return NodeGeneric;
    }

    void addToCallSiteObject(Node callSiteObj, Node rawNode, Node cookedNode) {}

    Node newThisLiteral(const TokenPos& pos, Node thisName) { return NodeGeneric; }
    Node newNullLiteral(const TokenPos& pos) { return NodeGeneric; }
    Node newRawUndefinedLiteral(const TokenPos& pos) { return NodeGeneric; }

    template <class Boxer>
    Node newRegExp(Node reobj, const TokenPos& pos, Boxer& boxer) { return NodeGeneric; }

    Node newConditional(Node cond, Node thenExpr, Node elseExpr) { return NodeGeneric; }

    Node newElision() { return NodeGeneric; }

    Node newDelete(uint32_t begin, Node expr) {
        return NodeUnparenthesizedUnary;
    }

    Node newTypeof(uint32_t begin, Node kid) {
        return NodeUnparenthesizedUnary;
    }

    Node newUnary(ParseNodeKind kind, uint32_t begin, Node kid) {
        return NodeUnparenthesizedUnary;
    }

    Node newUpdate(ParseNodeKind kind, uint32_t begin, Node kid) {
        return NodeGeneric;
    }

    Node newSpread(uint32_t begin, Node kid) {
        return NodeGeneric;
    }

    Node appendOrCreateList(ParseNodeKind kind, Node left, Node right, ParseContext* pc) {
        return NodeGeneric;
    }

    // Expressions

    Node newArrayLiteral(uint32_t begin) { return NodeUnparenthesizedArray; }
    MOZ_MUST_USE bool addElision(Node literal, const TokenPos& pos) { return true; }
    MOZ_MUST_USE bool addSpreadElement(Node literal, uint32_t begin, Node inner) { return true; }
    void addArrayElement(Node literal, Node element) { }

    Node newCall(const TokenPos& pos) { return NodeFunctionCall; }
    Node newSuperCall(Node callee) { return NodeGeneric; }
    Node newTaggedTemplate(const TokenPos& pos) { return NodeGeneric; }

    Node newObjectLiteral(uint32_t begin) { return NodeUnparenthesizedObject; }
    Node newClassMethodList(uint32_t begin) { return NodeGeneric; }
    Node newClassNames(Node outer, Node inner, const TokenPos& pos) { return NodeGeneric; }
    Node newClass(Node name, Node heritage, Node methodBlock, const TokenPos& pos) { return NodeGeneric; }

    Node newNewTarget(Node newHolder, Node targetHolder) { return NodeGeneric; }
    Node newPosHolder(const TokenPos& pos) { return NodeGeneric; }
    Node newSuperBase(Node thisName, const TokenPos& pos) { return NodeSuperBase; }

    MOZ_MUST_USE bool addPrototypeMutation(Node literal, uint32_t begin, Node expr) { return true; }
    MOZ_MUST_USE bool addPropertyDefinition(Node literal, Node name, Node expr) { return true; }
    MOZ_MUST_USE bool addShorthand(Node literal, Node name, Node expr) { return true; }
    MOZ_MUST_USE bool addSpreadProperty(Node literal, uint32_t begin, Node inner) { return true; }
    MOZ_MUST_USE bool addObjectMethodDefinition(Node literal, Node name, Node fn, AccessorType atype) { return true; }
    MOZ_MUST_USE bool addClassMethodDefinition(Node literal, Node name, Node fn, AccessorType atype, bool isStatic) { return true; }
    Node newYieldExpression(uint32_t begin, Node value) { return NodeGeneric; }
    Node newYieldStarExpression(uint32_t begin, Node value) { return NodeGeneric; }
    Node newAwaitExpression(uint32_t begin, Node value) { return NodeGeneric; }

    // Statements

    Node newStatementList(const TokenPos& pos) { return NodeGeneric; }
    void addStatementToList(Node list, Node stmt) {}
    void setListEndPosition(Node list, const TokenPos& pos) {}
    void addCaseStatementToList(Node list, Node stmt) {}
    MOZ_MUST_USE bool prependInitialYield(Node stmtList, Node gen) { return true; }
    Node newEmptyStatement(const TokenPos& pos) { return NodeEmptyStatement; }

    Node newExportDeclaration(Node kid, const TokenPos& pos) {
        return NodeGeneric;
    }
    Node newExportFromDeclaration(uint32_t begin, Node exportSpecSet, Node moduleSpec) {
        return NodeGeneric;
    }
    Node newExportDefaultDeclaration(Node kid, Node maybeBinding, const TokenPos& pos) {
        return NodeGeneric;
    }
    Node newExportSpec(Node bindingName, Node exportName) {
        return NodeGeneric;
    }
    Node newExportBatchSpec(const TokenPos& pos) {
        return NodeGeneric;
    }

    Node newSetThis(Node thisName, Node value) { return value; }

    Node newExprStatement(Node expr, uint32_t end) {
        return expr == NodeUnparenthesizedString ? NodeStringExprStatement : NodeGeneric;
    }

    Node newIfStatement(uint32_t begin, Node cond, Node then, Node else_) { return NodeGeneric; }
    Node newDoWhileStatement(Node body, Node cond, const TokenPos& pos) { return NodeGeneric; }
    Node newWhileStatement(uint32_t begin, Node cond, Node body) { return NodeGeneric; }
    Node newSwitchStatement(uint32_t begin, Node discriminant, Node caseList) { return NodeGeneric; }
    Node newCaseOrDefault(uint32_t begin, Node expr, Node body) { return NodeGeneric; }
    Node newContinueStatement(PropertyName* label, const TokenPos& pos) { return NodeGeneric; }
    Node newBreakStatement(PropertyName* label, const TokenPos& pos) { return NodeBreak; }
    Node newReturnStatement(Node expr, const TokenPos& pos) { return NodeReturn; }
    Node newExpressionBody(Node expr) { return NodeReturn; }
    Node newWithStatement(uint32_t begin, Node expr, Node body) { return NodeGeneric; }

    Node newLabeledStatement(PropertyName* label, Node stmt, uint32_t begin) {
        return NodeGeneric;
    }

    Node newThrowStatement(Node expr, const TokenPos& pos) { return NodeThrow; }
    Node newTryStatement(uint32_t begin, Node body, Node catchScope, Node finallyBlock) {
        return NodeGeneric;
    }
    Node newDebuggerStatement(const TokenPos& pos) { return NodeGeneric; }

    Node newPropertyAccess(Node expr, PropertyName* key, uint32_t end) {
        lastAtom = key;
        return NodeDottedProperty;
    }

    Node newPropertyByValue(Node pn, Node kid, uint32_t end) { return NodeElement; }

    MOZ_MUST_USE bool setupCatchScope(Node letBlock, Node catchName, Node catchBody) {
        return true;
    }

    MOZ_MUST_USE bool setLastFunctionFormalParameterDefault(Node funcpn, Node pn) { return true; }

    void checkAndSetIsDirectRHSAnonFunction(Node pn) {}

    Node newFunctionStatement(const TokenPos& pos) { return NodeFunctionStatement; }

    Node newFunctionExpression(const TokenPos& pos) {
        // All non-arrow function expressions are initially presumed to have
        // block body.  This will be overridden later *if* the function
        // expression permissibly has an AssignmentExpression body.
        return NodeFunctionExpressionBlockBody;
    }

    Node newArrowFunction(const TokenPos& pos) { return NodeFunctionArrow; }

    bool isExpressionClosure(Node node) const {
        return node == NodeFunctionExpressionClosure;
    }

    void noteExpressionClosure(Node* funcNode) const {
        *funcNode = NodeFunctionExpressionClosure;
    }

    void setFunctionFormalParametersAndBody(Node funcNode, Node kid) {}
    void setFunctionBody(Node pn, Node kid) {}
    void setFunctionBox(Node pn, FunctionBox* funbox) {}
    void addFunctionFormalParameter(Node pn, Node argpn) {}

    Node newForStatement(uint32_t begin, Node forHead, Node body, unsigned iflags) {
        return NodeGeneric;
    }

    Node newForHead(Node init, Node test, Node update, const TokenPos& pos) {
        return NodeGeneric;
    }

    Node newForInOrOfHead(ParseNodeKind kind, Node target, Node iteratedExpr, const TokenPos& pos) {
        return NodeGeneric;
    }

    MOZ_MUST_USE bool finishInitializerAssignment(Node pn, Node init) { return true; }

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

    Node newList(ParseNodeKind kind, const TokenPos& pos) {
        MOZ_ASSERT(kind != ParseNodeKind::Var);
        MOZ_ASSERT(kind != ParseNodeKind::Let);
        MOZ_ASSERT(kind != ParseNodeKind::Const);
        return NodeGeneric;
    }

    Node newList(ParseNodeKind kind, Node kid) {
        return newList(kind, TokenPos());
    }

    Node newDeclarationList(ParseNodeKind kind, const TokenPos& pos) {
        if (kind == ParseNodeKind::Var)
            return NodeVarDeclaration;
        MOZ_ASSERT(kind == ParseNodeKind::Let || kind == ParseNodeKind::Const);
        return NodeLexicalDeclaration;
    }

    bool isDeclarationList(Node node) {
        return node == NodeVarDeclaration || node == NodeLexicalDeclaration;
    }

    // This method should only be called from parsers using FullParseHandler.
    Node singleBindingFromDeclaration(Node decl) = delete;

    Node newCommaExpressionList(Node kid) {
        return NodeGeneric;
    }

    void addList(Node list, Node kid) {
        MOZ_ASSERT(list == NodeGeneric ||
                   list == NodeUnparenthesizedArray ||
                   list == NodeUnparenthesizedObject ||
                   list == NodeVarDeclaration ||
                   list == NodeLexicalDeclaration ||
                   list == NodeFunctionCall);
    }

    Node newNewExpression(uint32_t begin, Node ctor) {
        return NodeGeneric;
    }

    Node newAssignment(ParseNodeKind kind, Node lhs, Node rhs) {
        return kind == ParseNodeKind::Assign ? NodeUnparenthesizedAssignment : NodeGeneric;
    }

    bool isUnparenthesizedAssignment(Node node) {
        return node == NodeUnparenthesizedAssignment;
    }

    bool isUnparenthesizedUnaryExpression(Node node) {
        return node == NodeUnparenthesizedUnary;
    }

    bool isReturnStatement(Node node) {
        return node == NodeReturn;
    }

    bool isStatementPermittedAfterReturnStatement(Node pn) {
        return pn == NodeFunctionStatement || isNonArrowFunctionExpression(pn) ||
               pn == NodeVarDeclaration ||
               pn == NodeBreak ||
               pn == NodeThrow ||
               pn == NodeEmptyStatement;
    }

    bool isSuperBase(Node pn) {
        return pn == NodeSuperBase;
    }

    void setOp(Node pn, JSOp op) {}
    void setListFlag(Node pn, unsigned flag) {}
    MOZ_MUST_USE Node parenthesize(Node node) {
        // A number of nodes have different behavior upon parenthesization, but
        // only in some circumstances.  Convert these nodes to special
        // parenthesized forms.
        if (node == NodeUnparenthesizedArray)
            return NodeParenthesizedArray;
        if (node == NodeUnparenthesizedObject)
            return NodeParenthesizedObject;

        // Other nodes need not be recognizable after parenthesization; convert
        // them to a generic node.
        if (node == NodeUnparenthesizedString ||
            node == NodeUnparenthesizedAssignment ||
            node == NodeUnparenthesizedUnary)
        {
            return NodeGeneric;
        }

        // Convert parenthesized |async| to a normal name node.
        if (node == NodePotentialAsyncKeyword)
            return NodeName;

        // In all other cases, the parenthesized form of |node| is equivalent
        // to the unparenthesized form: return |node| unchanged.
        return node;
    }
    MOZ_MUST_USE Node setLikelyIIFE(Node pn) {
        return pn; // Remain in syntax-parse mode.
    }
    void setInDirectivePrologue(Node pn) {}

    bool isConstant(Node pn) { return false; }

    bool isName(Node node) {
        return node == NodeName ||
               node == NodeArgumentsName ||
               node == NodeEvalName ||
               node == NodePotentialAsyncKeyword;
    }

    bool isArgumentsName(Node node, JSContext* cx) {
        return node == NodeArgumentsName;
    }

    bool isEvalName(Node node, JSContext* cx) {
        return node == NodeEvalName;
    }

    bool isAsyncKeyword(Node node, JSContext* cx) {
        return node == NodePotentialAsyncKeyword;
    }

    PropertyName* maybeDottedProperty(Node node) {
        // Note: |super.apply(...)| is a special form that calls an "apply"
        // method retrieved from one value, but using a *different* value as
        // |this|.  It's not really eligible for the funapply/funcall
        // optimizations as they're currently implemented (assuming a single
        // value is used for both retrieval and |this|).
        if (node != NodeDottedProperty)
            return nullptr;
        return lastAtom->asPropertyName();
    }

    JSAtom* isStringExprStatement(Node pn, TokenPos* pos) {
        if (pn == NodeStringExprStatement) {
            *pos = lastStringPos;
            return lastAtom;
        }
        return nullptr;
    }

    bool canSkipLazyInnerFunctions() {
        return false;
    }
    bool canSkipLazyClosedOverBindings() {
        return false;
    }
    JSAtom* nextLazyClosedOverBinding() {
        MOZ_CRASH("SyntaxParseHandler::canSkipLazyClosedOverBindings must return false");
    }

    void adjustGetToSet(Node node) {}
} JS_HAZ_ROOTED; // See the top of SyntaxParseHandler for why this is safe.

} // namespace frontend
} // namespace js

#endif /* frontend_SyntaxParseHandler_h */
