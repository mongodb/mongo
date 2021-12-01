/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ParseNode-inl.h"

#include "mozilla/FloatingPoint.h"

#include "frontend/Parser.h"

#include "vm/JSContext-inl.h"

using namespace js;
using namespace js::frontend;

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

    if (node->pn_expr)
        stack->push(node->pn_expr);
    node->pn_expr = nullptr;
    return PushResult::Recyclable;
}

static PushResult
PushScopeNodeChildren(ParseNode* node, NodeStack* stack)
{
    MOZ_ASSERT(node->isArity(PN_SCOPE));

    if (node->scopeBody())
        stack->push(node->scopeBody());
    node->setScopeBody(nullptr);
    return PushResult::Recyclable;
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
      case ParseNodeKind::EmptyStatement:
      case ParseNodeKind::String:
      case ParseNodeKind::TemplateString:
      case ParseNodeKind::RegExp:
      case ParseNodeKind::True:
      case ParseNodeKind::False:
      case ParseNodeKind::Null:
      case ParseNodeKind::RawUndefined:
      case ParseNodeKind::Elision:
      case ParseNodeKind::Generator:
      case ParseNodeKind::Number:
      case ParseNodeKind::Break:
      case ParseNodeKind::Continue:
      case ParseNodeKind::Debugger:
      case ParseNodeKind::ExportBatchSpec:
      case ParseNodeKind::ObjectPropertyName:
      case ParseNodeKind::PosHolder:
        MOZ_ASSERT(pn->isArity(PN_NULLARY));
        return PushResult::Recyclable;

      // Nodes with a single non-null child.
      case ParseNodeKind::ExpressionStatement:
      case ParseNodeKind::TypeOfName:
      case ParseNodeKind::TypeOfExpr:
      case ParseNodeKind::Void:
      case ParseNodeKind::Not:
      case ParseNodeKind::BitNot:
      case ParseNodeKind::Throw:
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
      case ParseNodeKind::ComputedName:
      case ParseNodeKind::Spread:
      case ParseNodeKind::MutateProto:
      case ParseNodeKind::Export:
      case ParseNodeKind::SuperBase:
        return PushUnaryNodeChild(pn, stack);

      // Nodes with a single nullable child.
      case ParseNodeKind::This: {
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        if (pn->pn_kid)
            stack->push(pn->pn_kid);
        return PushResult::Recyclable;
      }

      // Binary nodes with two non-null children.

      // All assignment and compound assignment nodes qualify.
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
      // ...and a few others.
      case ParseNodeKind::Elem:
      case ParseNodeKind::ImportSpec:
      case ParseNodeKind::ExportSpec:
      case ParseNodeKind::Colon:
      case ParseNodeKind::Shorthand:
      case ParseNodeKind::DoWhile:
      case ParseNodeKind::While:
      case ParseNodeKind::Switch:
      case ParseNodeKind::ClassMethod:
      case ParseNodeKind::NewTarget:
      case ParseNodeKind::SetThis:
      case ParseNodeKind::For:
      case ParseNodeKind::With: {
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        stack->push(pn->pn_left);
        stack->push(pn->pn_right);
        return PushResult::Recyclable;
      }

      // Default clauses are ParseNodeKind::Case but do not have case
      // expressions. Named class expressions do not have outer binding nodes.
      // So both are binary nodes with a possibly-null pn_left.
      case ParseNodeKind::Case:
      case ParseNodeKind::ClassNames: {
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        if (pn->pn_left)
            stack->push(pn->pn_left);
        stack->push(pn->pn_right);
        return PushResult::Recyclable;
      }

      // The child is an assignment of a ParseNodeKind::Generator node to the
      // '.generator' local, for a synthesized, prepended initial yield.
      case ParseNodeKind::InitialYield: {
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        MOZ_ASSERT(pn->pn_kid->isKind(ParseNodeKind::Assign) &&
                   pn->pn_kid->pn_left->isKind(ParseNodeKind::Name) &&
                   pn->pn_kid->pn_right->isKind(ParseNodeKind::Generator));
        stack->push(pn->pn_kid);
        return PushResult::Recyclable;
      }

      // The child is the expression being yielded.
      case ParseNodeKind::YieldStar:
      case ParseNodeKind::Yield:
      case ParseNodeKind::Await: {
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        if (pn->pn_kid)
            stack->push(pn->pn_kid);
        return PushResult::Recyclable;
      }

      // A return node's child is what you'd expect: the return expression,
      // if any.
      case ParseNodeKind::Return: {
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        if (pn->pn_kid)
            stack->push(pn->pn_kid);
        return PushResult::Recyclable;
      }

      // Import and export-from nodes have a list of specifiers on the left
      // and a module string on the right.
      case ParseNodeKind::Import:
      case ParseNodeKind::ExportFrom: {
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        MOZ_ASSERT_IF(pn->isKind(ParseNodeKind::Import),
                      pn->pn_left->isKind(ParseNodeKind::ImportSpecList));
        MOZ_ASSERT_IF(pn->isKind(ParseNodeKind::ExportFrom),
                      pn->pn_left->isKind(ParseNodeKind::ExportSpecList));
        MOZ_ASSERT(pn->pn_left->isArity(PN_LIST));
        MOZ_ASSERT(pn->pn_right->isKind(ParseNodeKind::String));
        stack->pushList(pn->pn_left);
        stack->push(pn->pn_right);
        return PushResult::Recyclable;
      }

      case ParseNodeKind::ExportDefault: {
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        MOZ_ASSERT_IF(pn->pn_right, pn->pn_right->isKind(ParseNodeKind::Name));
        stack->push(pn->pn_left);
        if (pn->pn_right)
            stack->push(pn->pn_right);
        return PushResult::Recyclable;
      }

      // Ternary nodes with all children non-null.
      case ParseNodeKind::Conditional: {
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        stack->push(pn->pn_kid1);
        stack->push(pn->pn_kid2);
        stack->push(pn->pn_kid3);
        return PushResult::Recyclable;
      }

      // For for-in and for-of, the first child is the left-hand side of the
      // 'in' or 'of' (a declaration or an assignment target). The second
      // child is always null, and the third child is the expression looped
      // over.  For example, in |for (var p in obj)|, the first child is |var
      // p|, the second child is null, and the third child is |obj|.
      case ParseNodeKind::ForIn:
      case ParseNodeKind::ForOf: {
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        MOZ_ASSERT(!pn->pn_kid2);
        stack->push(pn->pn_kid1);
        stack->push(pn->pn_kid3);
        return PushResult::Recyclable;
      }

      // for (;;) nodes have one child per optional component of the loop head.
      case ParseNodeKind::ForHead: {
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
      case ParseNodeKind::Class: {
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
      case ParseNodeKind::If: {
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        stack->push(pn->pn_kid1);
        stack->push(pn->pn_kid2);
        if (pn->pn_kid3)
            stack->push(pn->pn_kid3);
        return PushResult::Recyclable;
      }

      // try-statements have statements to execute, and one or both of a
      // catch-list and a finally-block.
      case ParseNodeKind::Try: {
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        MOZ_ASSERT(pn->pn_kid2 || pn->pn_kid3);
        stack->push(pn->pn_kid1);
        if (pn->pn_kid2)
            stack->push(pn->pn_kid2);
        if (pn->pn_kid3)
            stack->push(pn->pn_kid3);
        return PushResult::Recyclable;
      }

      // A catch node has left node as catch-variable pattern (or null if
      // omitted) and right node as the statements in the catch block.
      case ParseNodeKind::Catch: {
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        if (pn->pn_left)
            stack->push(pn->pn_left);
        stack->push(pn->pn_right);
        return PushResult::Recyclable;
      }

      // List nodes with all non-null children.
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
      case ParseNodeKind::Pipeline:
      case ParseNodeKind::Comma:
      case ParseNodeKind::New:
      case ParseNodeKind::Call:
      case ParseNodeKind::SuperCall:
      case ParseNodeKind::Array:
      case ParseNodeKind::Object:
      case ParseNodeKind::TemplateStringList:
      case ParseNodeKind::TaggedTemplate:
      case ParseNodeKind::CallSiteObj:
      case ParseNodeKind::Var:
      case ParseNodeKind::Const:
      case ParseNodeKind::Let:
      case ParseNodeKind::StatementList:
      case ParseNodeKind::ImportSpecList:
      case ParseNodeKind::ExportSpecList:
      case ParseNodeKind::ParamsBody:
      case ParseNodeKind::ClassMethodList:
        return PushListNodeChildren(pn, stack);

      case ParseNodeKind::Label:
      case ParseNodeKind::Dot:
      case ParseNodeKind::Name:
        return PushNameNodeChildren(pn, stack);

      case ParseNodeKind::LexicalScope:
        return PushScopeNodeChildren(pn, stack);

      case ParseNodeKind::Function:
      case ParseNodeKind::Module:
        return PushCodeNodeChildren(pn, stack);

      case ParseNodeKind::Limit: // invalid sentinel value
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

    LifoAlloc::AutoFallibleScope fallibleAllocator(&alloc);
    void* p = alloc.alloc(sizeof (ParseNode));
    if (!p)
        ReportOutOfMemory(cx);
    return p;
}

ParseNode*
ParseNode::appendOrCreateList(ParseNodeKind kind, ParseNode* left, ParseNode* right,
                              FullParseHandler* handler, ParseContext* pc)
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
            (kind == ParseNodeKind::Pow ? !left->pn_parens : left->isBinaryOperation()))
        {
            ListNode* list = &left->as<ListNode>();

            list->append(right);
            list->pn_pos.end = right->pn_pos.end;

            return list;
        }
    }

    ParseNode* list = handler->new_<ListNode>(kind, JSOP_NOP, left);
    if (!list)
        return nullptr;

    list->append(right);
    return list;
}

#ifdef DEBUG

static const char * const parseNodeNames[] = {
#define STRINGIFY(name) #name,
    FOR_EACH_PARSE_NODE_KIND(STRINGIFY)
#undef STRINGIFY
};

void
frontend::DumpParseTree(ParseNode* pn, GenericPrinter& out, int indent)
{
    if (pn == nullptr)
        out.put("#NULL");
    else
        pn->dump(out, indent);
}

static void
IndentNewLine(GenericPrinter& out, int indent)
{
    out.putChar('\n');
    for (int i = 0; i < indent; ++i)
        out.putChar(' ');
}

void
ParseNode::dump(GenericPrinter& out)
{
    dump(out, 0);
    out.putChar('\n');
}

void
ParseNode::dump()
{
    js::Fprinter out(stderr);
    dump(out);
}

void
ParseNode::dump(GenericPrinter& out, int indent)
{
    switch (pn_arity) {
      case PN_NULLARY:
        ((NullaryNode*) this)->dump(out);
        break;
      case PN_UNARY:
        ((UnaryNode*) this)->dump(out, indent);
        break;
      case PN_BINARY:
        ((BinaryNode*) this)->dump(out, indent);
        break;
      case PN_TERNARY:
        ((TernaryNode*) this)->dump(out, indent);
        break;
      case PN_CODE:
        ((CodeNode*) this)->dump(out, indent);
        break;
      case PN_LIST:
        ((ListNode*) this)->dump(out, indent);
        break;
      case PN_NAME:
        ((NameNode*) this)->dump(out, indent);
        break;
      case PN_SCOPE:
        ((LexicalScopeNode*) this)->dump(out, indent);
        break;
      default:
        out.printf("#<BAD NODE %p, kind=%u, arity=%u>",
                (void*) this, unsigned(getKind()), unsigned(pn_arity));
        break;
    }
}

void
NullaryNode::dump(GenericPrinter& out)
{
    switch (getKind()) {
      case ParseNodeKind::True:  out.put("#true");  break;
      case ParseNodeKind::False: out.put("#false"); break;
      case ParseNodeKind::Null:  out.put("#null");  break;
      case ParseNodeKind::RawUndefined: out.put("#undefined"); break;

      case ParseNodeKind::Number: {
        ToCStringBuf cbuf;
        const char* cstr = NumberToCString(nullptr, &cbuf, pn_dval);
        if (!IsFinite(pn_dval))
            out.put("#");
        if (cstr)
            out.printf("%s", cstr);
        else
            out.printf("%g", pn_dval);
        break;
      }

      case ParseNodeKind::String:
        pn_atom->dumpCharsNoNewline(out);
        break;

      default:
        out.printf("(%s)", parseNodeNames[size_t(getKind())]);
    }
}

void
UnaryNode::dump(GenericPrinter& out, int indent)
{
    const char* name = parseNodeNames[size_t(getKind())];
    out.printf("(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_kid, out, indent);
    out.printf(")");
}

void
BinaryNode::dump(GenericPrinter& out, int indent)
{
    const char* name = parseNodeNames[size_t(getKind())];
    out.printf("(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_left, out, indent);
    IndentNewLine(out, indent);
    DumpParseTree(pn_right, out, indent);
    out.printf(")");
}

void
TernaryNode::dump(GenericPrinter& out, int indent)
{
    const char* name = parseNodeNames[size_t(getKind())];
    out.printf("(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_kid1, out, indent);
    IndentNewLine(out, indent);
    DumpParseTree(pn_kid2, out, indent);
    IndentNewLine(out, indent);
    DumpParseTree(pn_kid3, out, indent);
    out.printf(")");
}

void
CodeNode::dump(GenericPrinter& out, int indent)
{
    const char* name = parseNodeNames[size_t(getKind())];
    out.printf("(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_body, out, indent);
    out.printf(")");
}

void
ListNode::dump(GenericPrinter& out, int indent)
{
    const char* name = parseNodeNames[size_t(getKind())];
    out.printf("(%s [", name);
    if (pn_head != nullptr) {
        indent += strlen(name) + 3;
        DumpParseTree(pn_head, out, indent);
        ParseNode* pn = pn_head->pn_next;
        while (pn != nullptr) {
            IndentNewLine(out, indent);
            DumpParseTree(pn, out, indent);
            pn = pn->pn_next;
        }
    }
    out.printf("])");
}

template <typename CharT>
static void
DumpName(GenericPrinter& out, const CharT* s, size_t len)
{
    if (len == 0)
        out.put("#<zero-length name>");

    for (size_t i = 0; i < len; i++) {
        char16_t c = s[i];
        if (c > 32 && c < 127)
            out.putChar(c);
        else if (c <= 255)
            out.printf("\\x%02x", unsigned(c));
        else
            out.printf("\\u%04x", unsigned(c));
    }
}

void
NameNode::dump(GenericPrinter& out, int indent)
{
    if (isKind(ParseNodeKind::Name) || isKind(ParseNodeKind::Dot)) {
        if (isKind(ParseNodeKind::Dot))
            out.put("(.");

        if (!pn_atom) {
            out.put("#<null name>");
        } else if (getOp() == JSOP_GETARG && pn_atom->length() == 0) {
            // Dump destructuring parameter.
            out.put("(#<zero-length name> ");
            DumpParseTree(expr(), out, indent + 21);
            out.printf(")");
        } else {
            JS::AutoCheckCannotGC nogc;
            if (pn_atom->hasLatin1Chars())
                DumpName(out, pn_atom->latin1Chars(nogc), pn_atom->length());
            else
                DumpName(out, pn_atom->twoByteChars(nogc), pn_atom->length());
        }

        if (isKind(ParseNodeKind::Dot)) {
            out.putChar(' ');
            if (as<PropertyAccess>().isSuper())
                out.put("super");
            else
                DumpParseTree(expr(), out, indent + 2);
            out.printf(")");
        }
        return;
    }

    const char* name = parseNodeNames[size_t(getKind())];
    out.printf("(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(expr(), out, indent);
    out.printf(")");
}

void
LexicalScopeNode::dump(GenericPrinter& out, int indent)
{
    const char* name = parseNodeNames[size_t(getKind())];
    out.printf("(%s [", name);
    int nameIndent = indent + strlen(name) + 3;
    if (!isEmptyScope()) {
        LexicalScope::Data* bindings = scopeBindings();
        for (uint32_t i = 0; i < bindings->length; i++) {
            JSAtom* name = bindings->names[i].name();
            JS::AutoCheckCannotGC nogc;
            if (name->hasLatin1Chars())
                DumpName(out, name->latin1Chars(nogc), name->length());
            else
                DumpName(out, name->twoByteChars(nogc), name->length());
            if (i < bindings->length - 1)
                IndentNewLine(out, nameIndent);
        }
    }
    out.putChar(']');
    indent += 2;
    IndentNewLine(out, indent);
    DumpParseTree(scopeBody(), out, indent);
    out.printf(")");
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

/* static */ void
ObjectBox::TraceList(JSTracer* trc, ObjectBox* listHead)
{
    for (ObjectBox* box = listHead; box; box = box->traceLink)
        box->trace(trc);
}

void
ObjectBox::trace(JSTracer* trc)
{
    TraceRoot(trc, &object, "parser.object");
}

void
FunctionBox::trace(JSTracer* trc)
{
    ObjectBox::trace(trc);
    if (enclosingScope_)
        TraceRoot(trc, &enclosingScope_, "funbox-enclosingScope");
}

bool
js::frontend::IsAnonymousFunctionDefinition(ParseNode* pn)
{
    // ES 2017 draft
    // 12.15.2 (ArrowFunction, AsyncArrowFunction).
    // 14.1.12 (FunctionExpression).
    // 14.4.8 (GeneratorExpression).
    // 14.6.8 (AsyncFunctionExpression)
    if (pn->isKind(ParseNodeKind::Function) && !pn->pn_funbox->function()->explicitName())
        return true;

    // 14.5.8 (ClassExpression)
    if (pn->is<ClassNode>() && !pn->as<ClassNode>().names())
        return true;

    return false;
}
