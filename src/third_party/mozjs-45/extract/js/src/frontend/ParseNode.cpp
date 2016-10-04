/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ParseNode-inl.h"

#include "frontend/Parser.h"

#include "jscntxtinlines.h"

using namespace js;
using namespace js::frontend;

using mozilla::ArrayLength;
using mozilla::IsFinite;

#ifdef DEBUG
void
ParseNode::checkListConsistency()
{
    MOZ_ASSERT(isArity(PN_LIST));
    ParseNode** tail;
    uint32_t count = 0;
    if (pn_head) {
        ParseNode* last = pn_head;
        ParseNode* pn = last;
        while (pn) {
            last = pn;
            pn = pn->pn_next;
            count++;
        }

        tail = &last->pn_next;
    } else {
        tail = &pn_head;
    }
    MOZ_ASSERT(pn_tail == tail);
    MOZ_ASSERT(pn_count == count);
}
#endif

/* Add |node| to |parser|'s free node list. */
void
ParseNodeAllocator::freeNode(ParseNode* pn)
{
    /* Catch back-to-back dup recycles. */
    MOZ_ASSERT(pn != freelist);

    /*
     * It's too hard to clear these nodes from the AtomDefnMaps, etc. that
     * hold references to them, so we never free them. It's our caller's job to
     * recognize and process these, since their children do need to be dealt
     * with.
     */
    MOZ_ASSERT(!pn->isUsed());
    MOZ_ASSERT(!pn->isDefn());

#ifdef DEBUG
    /* Poison the node, to catch attempts to use it without initializing it. */
    memset(pn, 0xab, sizeof(*pn));
#endif

    pn->pn_next = freelist;
    freelist = pn;
}

namespace {

/*
 * A work pool of ParseNodes. The work pool is a stack, chained together
 * by nodes' pn_next fields. We use this to avoid creating deep C++ stacks
 * when recycling deep parse trees.
 *
 * Since parse nodes are probably allocated in something close to the order
 * they appear in a depth-first traversal of the tree, making the work pool
 * a stack should give us pretty good locality.
 */
class NodeStack {
  public:
    NodeStack() : top(nullptr) { }
    bool empty() { return top == nullptr; }
    void push(ParseNode* pn) {
        pn->pn_next = top;
        top = pn;
    }
    /* Push the children of the PN_LIST node |pn| on the stack. */
    void pushList(ParseNode* pn) {
        /* This clobbers pn->pn_head if the list is empty; should be okay. */
        *pn->pn_tail = top;
        top = pn->pn_head;
    }
    ParseNode* pop() {
        MOZ_ASSERT(!empty());
        ParseNode* hold = top; /* my kingdom for a prog1 */
        top = top->pn_next;
        return hold;
    }
  private:
    ParseNode* top;
};

} /* anonymous namespace */

enum class PushResult { Recyclable, CleanUpLater };

static PushResult
PushCodeNodeChildren(ParseNode* node, NodeStack* stack)
{
    MOZ_ASSERT(node->isArity(PN_CODE));

    /*
     * Function nodes are linked into the function box tree, and may appear
     * on method lists. Both of those lists are singly-linked, so trying to
     * update them now could result in quadratic behavior when recycling
     * trees containing many functions; and the lists can be very long. So
     * we put off cleaning the lists up until just before function
     * analysis, when we call CleanFunctionList.
     *
     * In fact, we can't recycle the parse node yet, either: it may appear
     * on a method list, and reusing the node would corrupt that. Instead,
     * we clear its pn_funbox pointer to mark it as deleted;
     * CleanFunctionList recycles it as well.
     *
     * We do recycle the nodes around it, though, so we must clear pointers
     * to them to avoid leaving dangling references where someone can find
     * them.
     */
    node->pn_funbox = nullptr;
    if (node->pn_body)
        stack->push(node->pn_body);
    node->pn_body = nullptr;

    return PushResult::CleanUpLater;
}

static PushResult
PushNameNodeChildren(ParseNode* node, NodeStack* stack)
{
    MOZ_ASSERT(node->isArity(PN_NAME));

    /*
     * Because used/defn nodes appear in AtomDefnMaps and elsewhere, we
     * don't recycle them. (We'll recover their storage when we free the
     * temporary arena.) However, we do recycle the nodes around them, so
     * clean up the pointers to avoid dangling references. The top-level
     * decls table carries references to them that later iterations through
     * the compileScript loop may find, so they need to be neat.
     *
     * pn_expr and pn_lexdef share storage; the latter isn't an owning
     * reference.
     */
    if (!node->isUsed()) {
        if (node->pn_expr)
            stack->push(node->pn_expr);
        node->pn_expr = nullptr;
    }

    if (!node->isUsed() && !node->isDefn())
        return PushResult::Recyclable;

    return PushResult::CleanUpLater;
}

static PushResult
PushListNodeChildren(ParseNode* node, NodeStack* stack)
{
    MOZ_ASSERT(node->isArity(PN_LIST));
    node->checkListConsistency();

    stack->pushList(node);

    return PushResult::Recyclable;
}

static PushResult
PushUnaryNodeChild(ParseNode* node, NodeStack* stack)
{
    MOZ_ASSERT(node->isArity(PN_UNARY));

    stack->push(node->pn_kid);

    return PushResult::Recyclable;
}

/*
 * Push the children of |pn| on |stack|. Return true if |pn| itself could be
 * safely recycled, or false if it must be cleaned later (pn_used and pn_defn
 * nodes, and all function nodes; see comments for CleanFunctionList in
 * SemanticAnalysis.cpp). Some callers want to free |pn|; others
 * (js::ParseNodeAllocator::prepareNodeForMutation) don't care about |pn|, and
 * just need to take care of its children.
 */
static PushResult
PushNodeChildren(ParseNode* pn, NodeStack* stack)
{
    switch (pn->getKind()) {
      // Trivial nodes that refer to no nodes, are referred to by nothing
      // but their parents, are never used, and are never a definition.
      case PNK_NOP:
      case PNK_STRING:
      case PNK_TEMPLATE_STRING:
      case PNK_REGEXP:
      case PNK_TRUE:
      case PNK_FALSE:
      case PNK_NULL:
      case PNK_ELISION:
      case PNK_GENERATOR:
      case PNK_NUMBER:
      case PNK_BREAK:
      case PNK_CONTINUE:
      case PNK_DEBUGGER:
      case PNK_EXPORT_BATCH_SPEC:
      case PNK_OBJECT_PROPERTY_NAME:
      case PNK_POSHOLDER:
        MOZ_ASSERT(pn->isArity(PN_NULLARY));
        MOZ_ASSERT(!pn->isUsed(), "handle non-trivial cases separately");
        MOZ_ASSERT(!pn->isDefn(), "handle non-trivial cases separately");
        return PushResult::Recyclable;

      // Nodes with a single non-null child.
      case PNK_TYPEOFNAME:
      case PNK_TYPEOFEXPR:
      case PNK_VOID:
      case PNK_NOT:
      case PNK_BITNOT:
      case PNK_THROW:
      case PNK_DELETENAME:
      case PNK_DELETEPROP:
      case PNK_DELETEELEM:
      case PNK_DELETEEXPR:
      case PNK_POS:
      case PNK_NEG:
      case PNK_PREINCREMENT:
      case PNK_POSTINCREMENT:
      case PNK_PREDECREMENT:
      case PNK_POSTDECREMENT:
      case PNK_COMPUTED_NAME:
      case PNK_ARRAYPUSH:
      case PNK_SPREAD:
      case PNK_MUTATEPROTO:
      case PNK_EXPORT:
      case PNK_SUPERBASE:
        return PushUnaryNodeChild(pn, stack);

      // Nodes with a single nullable child.
      case PNK_THIS:
      case PNK_SEMI: {
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        if (pn->pn_kid)
            stack->push(pn->pn_kid);
        return PushResult::Recyclable;
      }

      // Binary nodes with two non-null children.

      // All assignment and compound assignment nodes qualify.
      case PNK_ASSIGN:
      case PNK_ADDASSIGN:
      case PNK_SUBASSIGN:
      case PNK_BITORASSIGN:
      case PNK_BITXORASSIGN:
      case PNK_BITANDASSIGN:
      case PNK_LSHASSIGN:
      case PNK_RSHASSIGN:
      case PNK_URSHASSIGN:
      case PNK_MULASSIGN:
      case PNK_DIVASSIGN:
      case PNK_MODASSIGN:
      case PNK_POWASSIGN:
      // ...and a few others.
      case PNK_ELEM:
      case PNK_IMPORT_SPEC:
      case PNK_EXPORT_SPEC:
      case PNK_COLON:
      case PNK_SHORTHAND:
      case PNK_DOWHILE:
      case PNK_WHILE:
      case PNK_SWITCH:
      case PNK_LETBLOCK:
      case PNK_CLASSMETHOD:
      case PNK_NEWTARGET:
      case PNK_SETTHIS:
      case PNK_FOR:
      case PNK_COMPREHENSIONFOR: {
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        stack->push(pn->pn_left);
        stack->push(pn->pn_right);
        return PushResult::Recyclable;
      }

      // Default clauses are PNK_CASE but do not have case expressions.
      // Named class expressions do not have outer binding nodes.
      // So both are binary nodes with a possibly-null pn_left.
      case PNK_CASE:
      case PNK_CLASSNAMES: {
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        if (pn->pn_left)
            stack->push(pn->pn_left);
        stack->push(pn->pn_right);
        return PushResult::Recyclable;
      }

      // PNK_WITH is PN_BINARY_OBJ -- that is, PN_BINARY with (irrelevant for
      // this method's purposes) the addition of the StaticWithObject as
      // pn_binary_obj.  Both left (expression) and right (statement) are
      // non-null.
      case PNK_WITH: {
        MOZ_ASSERT(pn->isArity(PN_BINARY_OBJ));
        stack->push(pn->pn_left);
        stack->push(pn->pn_right);
        return PushResult::Recyclable;
      }

      // The left half is the expression being yielded.  The right half is
      // internal goop: a name reference to the invisible '.generator' local
      // variable, or an assignment of a PNK_GENERATOR node to the '.generator'
      // local, for a synthesized, prepended initial yield.  Yum!
      case PNK_YIELD_STAR:
      case PNK_YIELD: {
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        MOZ_ASSERT(pn->pn_right);
        MOZ_ASSERT(pn->pn_right->isKind(PNK_NAME) ||
                   (pn->pn_right->isKind(PNK_ASSIGN) &&
                    pn->pn_right->pn_left->isKind(PNK_NAME) &&
                    pn->pn_right->pn_right->isKind(PNK_GENERATOR)));
        if (pn->pn_left)
            stack->push(pn->pn_left);
        stack->push(pn->pn_right);
        return PushResult::Recyclable;
      }

      // A return node's child is what you'd expect: the return expression,
      // if any.
      case PNK_RETURN: {
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        if (pn->pn_kid)
            stack->push(pn->pn_kid);
        return PushResult::Recyclable;
      }

      // Import and export-from nodes have a list of specifiers on the left
      // and a module string on the right.
      case PNK_IMPORT:
      case PNK_EXPORT_FROM: {
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        MOZ_ASSERT_IF(pn->isKind(PNK_IMPORT), pn->pn_left->isKind(PNK_IMPORT_SPEC_LIST));
        MOZ_ASSERT_IF(pn->isKind(PNK_EXPORT_FROM), pn->pn_left->isKind(PNK_EXPORT_SPEC_LIST));
        MOZ_ASSERT(pn->pn_left->isArity(PN_LIST));
        MOZ_ASSERT(pn->pn_right->isKind(PNK_STRING));
        stack->pushList(pn->pn_left);
        stack->push(pn->pn_right);
        return PushResult::Recyclable;
      }

      case PNK_EXPORT_DEFAULT: {
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        MOZ_ASSERT_IF(pn->pn_right, pn->pn_right->isKind(PNK_NAME));
        stack->push(pn->pn_left);
        if (pn->pn_right)
            stack->push(pn->pn_right);
        return PushResult::Recyclable;
      }

      // Ternary nodes with all children non-null.
      case PNK_CONDITIONAL: {
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        stack->push(pn->pn_kid1);
        stack->push(pn->pn_kid2);
        stack->push(pn->pn_kid3);
        return PushResult::Recyclable;
      }

      // For for-in and for-of, the first child is any declaration present in
      // the for-loop (and null if not).  The second child is the expression or
      // pattern assigned every loop, and the third child is the expression
      // looped over.  For example, in |for (var p in obj)|, the first child is
      // |var p|, the second child is |p| (a node distinct from the one in
      // |var p|), and the third child is |obj|.
      case PNK_FORIN:
      case PNK_FOROF: {
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        if (pn->pn_kid1)
            stack->push(pn->pn_kid1);
        stack->push(pn->pn_kid2);
        stack->push(pn->pn_kid3);
        return PushResult::Recyclable;
      }

      // for (;;) nodes have one child per optional component of the loop head.
      case PNK_FORHEAD: {
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        if (pn->pn_kid1)
            stack->push(pn->pn_kid1);
        if (pn->pn_kid2)
            stack->push(pn->pn_kid2);
        if (pn->pn_kid3)
            stack->push(pn->pn_kid3);
        return PushResult::Recyclable;
      }

      // classes might have an optional node for the heritage, as well as the names
      case PNK_CLASS: {
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        if (pn->pn_kid1)
            stack->push(pn->pn_kid1);
        if (pn->pn_kid2)
            stack->push(pn->pn_kid2);
        stack->push(pn->pn_kid3);
        return PushResult::Recyclable;
      }

      // if-statement nodes have condition and consequent children and a
      // possibly-null alternative.
      case PNK_IF: {
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        stack->push(pn->pn_kid1);
        stack->push(pn->pn_kid2);
        if (pn->pn_kid3)
            stack->push(pn->pn_kid3);
        return PushResult::Recyclable;
      }

      // try-statements have statements to execute, and one or both of a
      // catch-list and a finally-block.
      case PNK_TRY: {
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        MOZ_ASSERT(pn->pn_kid2 || pn->pn_kid3);
        stack->push(pn->pn_kid1);
        if (pn->pn_kid2)
            stack->push(pn->pn_kid2);
        if (pn->pn_kid3)
            stack->push(pn->pn_kid3);
        return PushResult::Recyclable;
      }

      // A catch node has first kid as catch-variable pattern, the second kid
      // as catch condition (which, if non-null, records the |<cond>| in
      // SpiderMonkey's |catch (e if <cond>)| extension), and third kid as the
      // statements in the catch block.
      case PNK_CATCH: {
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        stack->push(pn->pn_kid1);
        if (pn->pn_kid2)
            stack->push(pn->pn_kid2);
        stack->push(pn->pn_kid3);
        return PushResult::Recyclable;
      }

      // List nodes with all non-null children.
      case PNK_OR:
      case PNK_AND:
      case PNK_BITOR:
      case PNK_BITXOR:
      case PNK_BITAND:
      case PNK_STRICTEQ:
      case PNK_EQ:
      case PNK_STRICTNE:
      case PNK_NE:
      case PNK_LT:
      case PNK_LE:
      case PNK_GT:
      case PNK_GE:
      case PNK_INSTANCEOF:
      case PNK_IN:
      case PNK_LSH:
      case PNK_RSH:
      case PNK_URSH:
      case PNK_ADD:
      case PNK_SUB:
      case PNK_STAR:
      case PNK_DIV:
      case PNK_MOD:
      case PNK_POW:
      case PNK_COMMA:
      case PNK_NEW:
      case PNK_CALL:
      case PNK_SUPERCALL:
      case PNK_GENEXP:
      case PNK_ARRAY:
      case PNK_OBJECT:
      case PNK_TEMPLATE_STRING_LIST:
      case PNK_TAGGED_TEMPLATE:
      case PNK_CALLSITEOBJ:
      case PNK_VAR:
      case PNK_CONST:
      case PNK_LET:
      case PNK_CATCHLIST:
      case PNK_STATEMENTLIST:
      case PNK_IMPORT_SPEC_LIST:
      case PNK_EXPORT_SPEC_LIST:
      case PNK_ARGSBODY:
      case PNK_CLASSMETHODLIST:
        return PushListNodeChildren(pn, stack);

      // Array comprehension nodes are lists with a single child:
      // PNK_COMPREHENSIONFOR for comprehensions, PNK_LEXICALSCOPE for legacy
      // comprehensions.  Probably this should be a non-list eventually.
      case PNK_ARRAYCOMP: {
#ifdef DEBUG
        MOZ_ASSERT(pn->isKind(PNK_ARRAYCOMP));
        MOZ_ASSERT(pn->isArity(PN_LIST));
        MOZ_ASSERT(pn->pn_count == 1);
        MOZ_ASSERT(pn->pn_head->isKind(PNK_LEXICALSCOPE) ||
                   pn->pn_head->isKind(PNK_COMPREHENSIONFOR));
#endif
        return PushListNodeChildren(pn, stack);
      }

      case PNK_LABEL:
      case PNK_DOT:
      case PNK_LEXICALSCOPE:
      case PNK_NAME:
        return PushNameNodeChildren(pn, stack);

      case PNK_FUNCTION:
      case PNK_MODULE:
        return PushCodeNodeChildren(pn, stack);

      case PNK_LIMIT: // invalid sentinel value
        MOZ_CRASH("invalid node kind");
    }

    MOZ_CRASH("bad ParseNodeKind");
    return PushResult::CleanUpLater;
}

/*
 * Prepare |pn| to be mutated in place into a new kind of node. Recycle all
 * |pn|'s recyclable children (but not |pn| itself!), and disconnect it from
 * metadata structures (the function box tree).
 */
void
ParseNodeAllocator::prepareNodeForMutation(ParseNode* pn)
{
    // Nothing to do for nullary nodes.
    if (pn->isArity(PN_NULLARY))
        return;

    // Put |pn|'s children (but not |pn| itself) on a work stack.
    NodeStack stack;
    PushNodeChildren(pn, &stack);

    // For each node on the work stack, push its children on the work stack,
    // and free the node if we can.
    while (!stack.empty()) {
        pn = stack.pop();
        if (PushNodeChildren(pn, &stack) == PushResult::Recyclable)
            freeNode(pn);
    }
}

/*
 * Return the nodes in the subtree |pn| to the parser's free node list, for
 * reallocation.
 */
ParseNode*
ParseNodeAllocator::freeTree(ParseNode* pn)
{
    if (!pn)
        return nullptr;

    ParseNode* savedNext = pn->pn_next;

    NodeStack stack;
    for (;;) {
        if (PushNodeChildren(pn, &stack) == PushResult::Recyclable)
            freeNode(pn);
        if (stack.empty())
            break;
        pn = stack.pop();
    }

    return savedNext;
}

/*
 * Allocate a ParseNode from parser's node freelist or, failing that, from
 * cx's temporary arena.
 */
void*
ParseNodeAllocator::allocNode()
{
    if (ParseNode* pn = freelist) {
        freelist = pn->pn_next;
        return pn;
    }

    void* p = alloc.alloc(sizeof (ParseNode));
    if (!p)
        ReportOutOfMemory(cx);
    return p;
}

ParseNode*
ParseNode::appendOrCreateList(ParseNodeKind kind, JSOp op, ParseNode* left, ParseNode* right,
                              FullParseHandler* handler, ParseContext<FullParseHandler>* pc)
{
    // The asm.js specification is written in ECMAScript grammar terms that
    // specify *only* a binary tree.  It's a royal pain to implement the asm.js
    // spec to act upon n-ary lists as created below.  So for asm.js, form a
    // binary tree of lists exactly as ECMAScript would by skipping the
    // following optimization.
    if (!pc->useAsmOrInsideUseAsm()) {
        // Left-associative trees of a given operator (e.g. |a + b + c|) are
        // binary trees in the spec: (+ (+ a b) c) in Lisp terms.  Recursively
        // processing such a tree, exactly implemented that way, would blow the
        // the stack.  We use a list node that uses O(1) stack to represent
        // such operations: (+ a b c).
        //
        // (**) is right-associative; per spec |a ** b ** c| parses as
        // (** a (** b c)). But we treat this the same way, creating a list
        // node: (** a b c). All consumers must understand that this must be
        // processed with a right fold, whereas the list (+ a b c) must be
        // processed with a left fold because (+) is left-associative.
        //
        if (left->isKind(kind) &&
            left->isOp(op) &&
            (CodeSpec[op].format & JOF_LEFTASSOC ||
             (kind == PNK_POW && !left->pn_parens)))
        {
            ListNode* list = &left->as<ListNode>();

            list->append(right);
            list->pn_pos.end = right->pn_pos.end;

            return list;
        }
    }

    ParseNode* list = handler->new_<ListNode>(kind, op, left);
    if (!list)
        return nullptr;

    list->append(right);
    return list;
}

const char*
Definition::kindString(Kind kind)
{
    static const char* const table[] = {
        "",
        js_var_str,
        js_const_str,
        js_let_str,
        "argument",
        js_function_str,
        "unknown",
        js_import_str
    };

    MOZ_ASSERT(size_t(kind) < ArrayLength(table));
    return table[kind];
}

namespace js {
namespace frontend {

/*
 * This function assumes the cloned tree is for use in the same statement and
 * binding context as the original tree.
 */
template <>
ParseNode*
Parser<FullParseHandler>::cloneParseTree(ParseNode* opn)
{
    JS_CHECK_RECURSION(context, return nullptr);

    if (opn->isKind(PNK_COMPUTED_NAME)) {
        report(ParseError, false, opn, JSMSG_COMPUTED_NAME_IN_PATTERN);
        return null();
    }

    ParseNode* pn = handler.new_<ParseNode>(opn->getKind(), opn->getOp(), opn->getArity(),
                                            opn->pn_pos);
    if (!pn)
        return nullptr;
    pn->setInParens(opn->isInParens());
    pn->setDefn(opn->isDefn());
    pn->setUsed(opn->isUsed());

    switch (pn->getArity()) {
#define NULLCHECK(e)    JS_BEGIN_MACRO if (!(e)) return nullptr; JS_END_MACRO

      case PN_CODE: {
        RootedFunction fun(context, opn->pn_funbox->function());
        NULLCHECK(pn->pn_funbox = newFunctionBox(pn, fun, pc,
                                                 Directives(/* strict = */ opn->pn_funbox->strict()),
                                                 opn->pn_funbox->generatorKind()));
        NULLCHECK(pn->pn_body = cloneParseTree(opn->pn_body));
        pn->pn_scopecoord = opn->pn_scopecoord;
        pn->pn_dflags = opn->pn_dflags;
        pn->pn_blockid = opn->pn_blockid;
        break;
      }

      case PN_LIST:
        pn->makeEmpty();
        for (ParseNode* opn2 = opn->pn_head; opn2; opn2 = opn2->pn_next) {
            ParseNode* pn2;
            NULLCHECK(pn2 = cloneParseTree(opn2));
            pn->append(pn2);
        }
        pn->pn_xflags = opn->pn_xflags;
        break;

      case PN_TERNARY:
        if (opn->pn_kid1)
            NULLCHECK(pn->pn_kid1 = cloneParseTree(opn->pn_kid1));
        if (opn->pn_kid2)
            NULLCHECK(pn->pn_kid2 = cloneParseTree(opn->pn_kid2));
        if (opn->pn_kid3)
            NULLCHECK(pn->pn_kid3 = cloneParseTree(opn->pn_kid3));
        break;

      case PN_BINARY:
      case PN_BINARY_OBJ:
        if (opn->pn_left)
            NULLCHECK(pn->pn_left = cloneParseTree(opn->pn_left));
        if (opn->pn_right) {
            if (opn->pn_right != opn->pn_left)
                NULLCHECK(pn->pn_right = cloneParseTree(opn->pn_right));
            else
                pn->pn_right = pn->pn_left;
        }
        if (opn->isArity(PN_BINARY)) {
            pn->pn_iflags = opn->pn_iflags;
        } else {
            MOZ_ASSERT(opn->isArity(PN_BINARY_OBJ));
            pn->pn_binary_obj = opn->pn_binary_obj;
        }
        break;

      case PN_UNARY:
        if (opn->pn_kid)
            NULLCHECK(pn->pn_kid = cloneParseTree(opn->pn_kid));
        break;

      case PN_NAME:
        // PN_NAME could mean several arms in pn_u, so copy the whole thing.
        pn->pn_u = opn->pn_u;
        if (opn->isUsed()) {
            /*
             * The old name is a use of its pn_lexdef. Make the clone also be a
             * use of that definition.
             */
            Definition* dn = pn->pn_lexdef;

            pn->pn_link = dn->dn_uses;
            pn->pn_dflags = opn->pn_dflags;
            dn->dn_uses = pn;
        } else if (opn->pn_expr) {
            NULLCHECK(pn->pn_expr = cloneParseTree(opn->pn_expr));

            /*
             * If the old name is a definition, the new one has pn_defn set.
             * Make the old name a use of the new node.
             */
            if (opn->isDefn()) {
                opn->setDefn(false);
                handler.linkUseToDef(opn, (Definition*) pn);
            }
        }
        break;

      case PN_NULLARY:
        pn->pn_u = opn->pn_u;
        break;

#undef NULLCHECK
    }
    return pn;
}

template <>
ParseNode*
Parser<FullParseHandler>::cloneLeftHandSide(ParseNode* opn);

/*
 * Used by Parser::cloneLeftHandSide to clone a default expression
 * in the form of
 *    [a = default] or {a: b = default}
 */
template <>
ParseNode*
Parser<FullParseHandler>::cloneDestructuringDefault(ParseNode* opn)
{
    MOZ_ASSERT(opn->isKind(PNK_ASSIGN));

    report(ParseError, false, opn, JSMSG_DEFAULT_IN_PATTERN);
    return null();
}

/*
 * Used by Parser::forStatement and comprehensionTail to clone the TARGET in
 *   for (var/const/let TARGET in EXPR)
 *
 * opn must be the pn_head of a node produced by Parser::variables, so its form
 * is known to be LHS = NAME | [LHS] | {id:LHS}.
 *
 * The cloned tree is for use only in the same statement and binding context as
 * the original tree.
 */
template <>
ParseNode*
Parser<FullParseHandler>::cloneLeftHandSide(ParseNode* opn)
{
    ParseNode* pn = handler.new_<ParseNode>(opn->getKind(), opn->getOp(), opn->getArity(),
                                            opn->pn_pos);
    if (!pn)
        return nullptr;
    pn->setInParens(opn->isInParens());
    pn->setDefn(opn->isDefn());
    pn->setUsed(opn->isUsed());

    if (opn->isArity(PN_LIST)) {
        MOZ_ASSERT(opn->isKind(PNK_ARRAY) || opn->isKind(PNK_OBJECT));
        pn->makeEmpty();
        for (ParseNode* opn2 = opn->pn_head; opn2; opn2 = opn2->pn_next) {
            ParseNode* pn2;
            if (opn->isKind(PNK_OBJECT)) {
                if (opn2->isKind(PNK_MUTATEPROTO)) {
                    ParseNode* target = opn2->pn_kid->isKind(PNK_ASSIGN)
                                        ? cloneDestructuringDefault(opn2->pn_kid)
                                        : cloneLeftHandSide(opn2->pn_kid);
                    if (!target)
                        return nullptr;
                    pn2 = handler.new_<UnaryNode>(PNK_MUTATEPROTO, JSOP_NOP, opn2->pn_pos, target);
                } else {
                    MOZ_ASSERT(opn2->isArity(PN_BINARY));
                    MOZ_ASSERT(opn2->isKind(PNK_COLON) || opn2->isKind(PNK_SHORTHAND));

                    ParseNode* tag = cloneParseTree(opn2->pn_left);
                    if (!tag)
                        return nullptr;
                    ParseNode* target = opn2->pn_right->isKind(PNK_ASSIGN)
                                        ? cloneDestructuringDefault(opn2->pn_right)
                                        : cloneLeftHandSide(opn2->pn_right);
                    if (!target)
                        return nullptr;

                    pn2 = handler.new_<BinaryNode>(opn2->getKind(), JSOP_INITPROP, opn2->pn_pos, tag, target);
                }
            } else if (opn2->isArity(PN_NULLARY)) {
                MOZ_ASSERT(opn2->isKind(PNK_ELISION));
                pn2 = cloneParseTree(opn2);
            } else if (opn2->isKind(PNK_SPREAD)) {
                ParseNode* target = cloneLeftHandSide(opn2->pn_kid);
                if (!target)
                    return nullptr;
                pn2 = handler.new_<UnaryNode>(PNK_SPREAD, JSOP_NOP, opn2->pn_pos, target);
            } else if (opn2->isKind(PNK_ASSIGN)) {
                pn2 = cloneDestructuringDefault(opn2);
            } else {
                pn2 = cloneLeftHandSide(opn2);
            }

            if (!pn2)
                return nullptr;
            pn->append(pn2);
        }
        pn->pn_xflags = opn->pn_xflags;
        return pn;
    }

    MOZ_ASSERT(opn->isArity(PN_NAME));
    MOZ_ASSERT(opn->isKind(PNK_NAME));

    /* If opn is a definition or use, make pn a use. */
    pn->pn_u.name = opn->pn_u.name;
    pn->setOp(JSOP_SETNAME);
    if (opn->isUsed()) {
        Definition* dn = pn->pn_lexdef;

        pn->pn_link = dn->dn_uses;
        dn->dn_uses = pn;
    } else {
        pn->pn_expr = nullptr;
        if (opn->isDefn()) {
            /* We copied some definition-specific state into pn. Clear it out. */
            pn->pn_scopecoord.makeFree();
            pn->pn_dflags &= ~(PND_LEXICAL | PND_BOUND);
            pn->setDefn(false);

            handler.linkUseToDef(pn, (Definition*) opn);
        }
    }
    return pn;
}

} /* namespace frontend */
} /* namespace js */

#ifdef DEBUG

static const char * const parseNodeNames[] = {
#define STRINGIFY(name) #name,
    FOR_EACH_PARSE_NODE_KIND(STRINGIFY)
#undef STRINGIFY
};

void
frontend::DumpParseTree(ParseNode* pn, int indent)
{
    if (pn == nullptr)
        fprintf(stderr, "#NULL");
    else
        pn->dump(indent);
}

static void
IndentNewLine(int indent)
{
    fputc('\n', stderr);
    for (int i = 0; i < indent; ++i)
        fputc(' ', stderr);
}

void
ParseNode::dump()
{
    dump(0);
    fputc('\n', stderr);
}

void
ParseNode::dump(int indent)
{
    switch (pn_arity) {
      case PN_NULLARY:
        ((NullaryNode*) this)->dump();
        break;
      case PN_UNARY:
        ((UnaryNode*) this)->dump(indent);
        break;
      case PN_BINARY:
        ((BinaryNode*) this)->dump(indent);
        break;
      case PN_BINARY_OBJ:
        ((BinaryObjNode*) this)->dump(indent);
        break;
      case PN_TERNARY:
        ((TernaryNode*) this)->dump(indent);
        break;
      case PN_CODE:
        ((CodeNode*) this)->dump(indent);
        break;
      case PN_LIST:
        ((ListNode*) this)->dump(indent);
        break;
      case PN_NAME:
        ((NameNode*) this)->dump(indent);
        break;
      default:
        fprintf(stderr, "#<BAD NODE %p, kind=%u, arity=%u>",
                (void*) this, unsigned(getKind()), unsigned(pn_arity));
        break;
    }
}

void
NullaryNode::dump()
{
    switch (getKind()) {
      case PNK_TRUE:  fprintf(stderr, "#true");  break;
      case PNK_FALSE: fprintf(stderr, "#false"); break;
      case PNK_NULL:  fprintf(stderr, "#null");  break;

      case PNK_NUMBER: {
        ToCStringBuf cbuf;
        const char* cstr = NumberToCString(nullptr, &cbuf, pn_dval);
        if (!IsFinite(pn_dval))
            fputc('#', stderr);
        if (cstr)
            fprintf(stderr, "%s", cstr);
        else
            fprintf(stderr, "%g", pn_dval);
        break;
      }

      case PNK_STRING:
        pn_atom->dumpCharsNoNewline();
        break;

      default:
        fprintf(stderr, "(%s)", parseNodeNames[getKind()]);
    }
}

void
UnaryNode::dump(int indent)
{
    const char* name = parseNodeNames[getKind()];
    fprintf(stderr, "(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_kid, indent);
    fprintf(stderr, ")");
}

void
BinaryNode::dump(int indent)
{
    const char* name = parseNodeNames[getKind()];
    fprintf(stderr, "(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_left, indent);
    IndentNewLine(indent);
    DumpParseTree(pn_right, indent);
    fprintf(stderr, ")");
}

void
BinaryObjNode::dump(int indent)
{
    const char* name = parseNodeNames[getKind()];
    fprintf(stderr, "(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_left, indent);
    IndentNewLine(indent);
    DumpParseTree(pn_right, indent);
    fprintf(stderr, ")");
}

void
TernaryNode::dump(int indent)
{
    const char* name = parseNodeNames[getKind()];
    fprintf(stderr, "(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_kid1, indent);
    IndentNewLine(indent);
    DumpParseTree(pn_kid2, indent);
    IndentNewLine(indent);
    DumpParseTree(pn_kid3, indent);
    fprintf(stderr, ")");
}

void
CodeNode::dump(int indent)
{
    const char* name = parseNodeNames[getKind()];
    fprintf(stderr, "(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_body, indent);
    fprintf(stderr, ")");
}

void
ListNode::dump(int indent)
{
    const char* name = parseNodeNames[getKind()];
    fprintf(stderr, "(%s [", name);
    if (pn_head != nullptr) {
        indent += strlen(name) + 3;
        DumpParseTree(pn_head, indent);
        ParseNode* pn = pn_head->pn_next;
        while (pn != nullptr) {
            IndentNewLine(indent);
            DumpParseTree(pn, indent);
            pn = pn->pn_next;
        }
    }
    fprintf(stderr, "])");
}

template <typename CharT>
static void
DumpName(const CharT* s, size_t len)
{
    if (len == 0)
        fprintf(stderr, "#<zero-length name>");

    for (size_t i = 0; i < len; i++) {
        char16_t c = s[i];
        if (c > 32 && c < 127)
            fputc(c, stderr);
        else if (c <= 255)
            fprintf(stderr, "\\x%02x", unsigned(c));
        else
            fprintf(stderr, "\\u%04x", unsigned(c));
    }
}

void
NameNode::dump(int indent)
{
    if (isKind(PNK_NAME) || isKind(PNK_DOT)) {
        if (isKind(PNK_DOT))
            fprintf(stderr, "(.");

        if (!pn_atom) {
            fprintf(stderr, "#<null name>");
        } else if (getOp() == JSOP_GETARG && pn_atom->length() == 0) {
            // Dump destructuring parameter.
            fprintf(stderr, "(#<zero-length name> ");
            DumpParseTree(expr(), indent + 21);
            fputc(')', stderr);
        } else {
            JS::AutoCheckCannotGC nogc;
            if (pn_atom->hasLatin1Chars())
                DumpName(pn_atom->latin1Chars(nogc), pn_atom->length());
            else
                DumpName(pn_atom->twoByteChars(nogc), pn_atom->length());
        }

        if (isKind(PNK_DOT)) {
            fputc(' ', stderr);
            if (as<PropertyAccess>().isSuper())
                fprintf(stderr, "super");
            else
                DumpParseTree(expr(), indent + 2);
            fputc(')', stderr);
        }
        return;
    }

    MOZ_ASSERT(!isUsed());
    const char* name = parseNodeNames[getKind()];
    if (isUsed())
        fprintf(stderr, "(%s)", name);
    else {
        fprintf(stderr, "(%s ", name);
        indent += strlen(name) + 2;
        DumpParseTree(expr(), indent);
        fprintf(stderr, ")");
    }
}
#endif

ObjectBox::ObjectBox(JSObject* object, ObjectBox* traceLink)
  : object(object),
    traceLink(traceLink),
    emitLink(nullptr)
{
    MOZ_ASSERT(!object->is<JSFunction>());
    MOZ_ASSERT(object->isTenured());
}

ObjectBox::ObjectBox(JSFunction* function, ObjectBox* traceLink)
  : object(function),
    traceLink(traceLink),
    emitLink(nullptr)
{
    MOZ_ASSERT(object->is<JSFunction>());
    MOZ_ASSERT(asFunctionBox()->function() == function);
    MOZ_ASSERT(object->isTenured());
}

FunctionBox*
ObjectBox::asFunctionBox()
{
    MOZ_ASSERT(isFunctionBox());
    return static_cast<FunctionBox*>(this);
}

ModuleBox*
ObjectBox::asModuleBox()
{
    MOZ_ASSERT(isModuleBox());
    return static_cast<ModuleBox*>(this);
}

void
ObjectBox::trace(JSTracer* trc)
{
    ObjectBox* box = this;
    while (box) {
        TraceRoot(trc, &box->object, "parser.object");
        if (box->isFunctionBox()) {
            FunctionBox* funbox = box->asFunctionBox();
            funbox->bindings.trace(trc);
            if (funbox->enclosingStaticScope_)
                TraceRoot(trc, &funbox->enclosingStaticScope_, "funbox-enclosingStaticScope");
        } else if (box->isModuleBox()) {
            ModuleBox* modulebox = box->asModuleBox();
            modulebox->bindings.trace(trc);
            modulebox->exportNames.trace(trc);
        }
        box = box->traceLink;
    }
}
