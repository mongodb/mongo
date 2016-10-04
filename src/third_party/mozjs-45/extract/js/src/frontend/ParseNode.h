/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ParseNode_h
#define frontend_ParseNode_h

#include "mozilla/Attributes.h"

#include "frontend/TokenStream.h"

namespace js {
namespace frontend {

template <typename ParseHandler>
struct ParseContext;

class FullParseHandler;
class FunctionBox;
class ModuleBox;
class ObjectBox;

// A packed ScopeCoordinate for use in the frontend during bytecode
// compilation.
//
// Definitions start out !isFree() && isHopsUnknown().
// Uses start out isFree().
//
// The BCE computes the correct number of hops based on the static scope
// chain. This is ncessary because due to hoisting, the Parser does not know
// the final static scope chain.
//
// The BCE also computes the correct slot number depending on whether the
// binding is aliased. If it is aliased, the slot number is the slot on the
// dynamic scope object. Otherwise, the slot number is the frame slot.
class PackedScopeCoordinate
{
    uint32_t hops_ : SCOPECOORD_HOPS_BITS;
    uint32_t slot_ : SCOPECOORD_SLOT_BITS;

    void checkInvariants() {
        static_assert(sizeof(PackedScopeCoordinate) == sizeof(uint32_t),
                      "Not necessary for correctness, but good for ParseNode memory use");
    }

  public:
    // Steal one value to represent the sentinel value signaling that the
    // binding is free, and one value to represent the sentinel value
    // signaling that the number of hop count need to be computed by the
    // BytecodeEmitter.
    static const uint32_t UNKNOWN_HOPS = SCOPECOORD_HOPS_LIMIT - 1;
    static const uint32_t UNKNOWN_SLOT = SCOPECOORD_SLOT_LIMIT - 1;
    bool isHopsUnknown() const { return hops_ == UNKNOWN_HOPS; }
    bool isFree() const { return slot_ == UNKNOWN_SLOT; }

    uint32_t hops() const { MOZ_ASSERT(!isFree()); return hops_; }
    uint32_t slot() const { MOZ_ASSERT(!isFree()); return slot_; }

    bool setSlot(TokenStream& ts, uint32_t newSlot) {
        if (newSlot >= UNKNOWN_SLOT)
            return ts.reportError(JSMSG_TOO_MANY_LOCALS);
        slot_ = newSlot;
        return true;
    }

    bool setHops(TokenStream& ts, uint32_t newHops) {
        if (newHops >= UNKNOWN_HOPS)
            return ts.reportError(JSMSG_TOO_DEEP, js_function_str);
        hops_ = newHops;
        return true;
    }

    bool set(TokenStream& ts, uint32_t newHops, uint32_t newSlot) {
        return setHops(ts, newHops) && setSlot(ts, newSlot);
    }

    void makeFree() {
        hops_ = UNKNOWN_HOPS;
        slot_ = UNKNOWN_SLOT;
        MOZ_ASSERT(isFree());
    }
};

#define FOR_EACH_PARSE_NODE_KIND(F) \
    F(NOP) \
    F(SEMI) \
    F(COMMA) \
    F(CONDITIONAL) \
    F(COLON) \
    F(SHORTHAND) \
    F(POS) \
    F(NEG) \
    F(PREINCREMENT) \
    F(POSTINCREMENT) \
    F(PREDECREMENT) \
    F(POSTDECREMENT) \
    F(DOT) \
    F(ELEM) \
    F(ARRAY) \
    F(ELISION) \
    F(STATEMENTLIST) \
    F(LABEL) \
    F(OBJECT) \
    F(CALL) \
    F(NAME) \
    F(OBJECT_PROPERTY_NAME) \
    F(COMPUTED_NAME) \
    F(NUMBER) \
    F(STRING) \
    F(TEMPLATE_STRING_LIST) \
    F(TEMPLATE_STRING) \
    F(TAGGED_TEMPLATE) \
    F(CALLSITEOBJ) \
    F(REGEXP) \
    F(TRUE) \
    F(FALSE) \
    F(NULL) \
    F(THIS) \
    F(FUNCTION) \
    F(MODULE) \
    F(IF) \
    F(SWITCH) \
    F(CASE) \
    F(WHILE) \
    F(DOWHILE) \
    F(FOR) \
    F(COMPREHENSIONFOR) \
    F(BREAK) \
    F(CONTINUE) \
    F(VAR) \
    F(CONST) \
    F(WITH) \
    F(RETURN) \
    F(NEW) \
    /* Delete operations.  These must be sequential. */ \
    F(DELETENAME) \
    F(DELETEPROP) \
    F(DELETEELEM) \
    F(DELETEEXPR) \
    F(TRY) \
    F(CATCH) \
    F(CATCHLIST) \
    F(THROW) \
    F(DEBUGGER) \
    F(GENERATOR) \
    F(YIELD) \
    F(YIELD_STAR) \
    F(GENEXP) \
    F(ARRAYCOMP) \
    F(ARRAYPUSH) \
    F(LEXICALSCOPE) \
    F(LET) \
    F(LETBLOCK) \
    F(IMPORT) \
    F(IMPORT_SPEC_LIST) \
    F(IMPORT_SPEC) \
    F(EXPORT) \
    F(EXPORT_FROM) \
    F(EXPORT_DEFAULT) \
    F(EXPORT_SPEC_LIST) \
    F(EXPORT_SPEC) \
    F(EXPORT_BATCH_SPEC) \
    F(FORIN) \
    F(FOROF) \
    F(FORHEAD) \
    F(ARGSBODY) \
    F(SPREAD) \
    F(MUTATEPROTO) \
    F(CLASS) \
    F(CLASSMETHOD) \
    F(CLASSMETHODLIST) \
    F(CLASSNAMES) \
    F(NEWTARGET) \
    F(POSHOLDER) \
    F(SUPERBASE) \
    F(SUPERCALL) \
    F(SETTHIS) \
    \
    /* Unary operators. */ \
    F(TYPEOFNAME) \
    F(TYPEOFEXPR) \
    F(VOID) \
    F(NOT) \
    F(BITNOT) \
    \
    /* \
     * Binary operators. \
     * These must be in the same order as TOK_OR and friends in TokenStream.h. \
     */ \
    F(OR) \
    F(AND) \
    F(BITOR) \
    F(BITXOR) \
    F(BITAND) \
    F(STRICTEQ) \
    F(EQ) \
    F(STRICTNE) \
    F(NE) \
    F(LT) \
    F(LE) \
    F(GT) \
    F(GE) \
    F(INSTANCEOF) \
    F(IN) \
    F(LSH) \
    F(RSH) \
    F(URSH) \
    F(ADD) \
    F(SUB) \
    F(STAR) \
    F(DIV) \
    F(MOD) \
    F(POW) \
    \
    /* Assignment operators (= += -= etc.). */ \
    /* ParseNode::isAssignment assumes all these are consecutive. */ \
    F(ASSIGN) \
    F(ADDASSIGN) \
    F(SUBASSIGN) \
    F(BITORASSIGN) \
    F(BITXORASSIGN) \
    F(BITANDASSIGN) \
    F(LSHASSIGN) \
    F(RSHASSIGN) \
    F(URSHASSIGN) \
    F(MULASSIGN) \
    F(DIVASSIGN) \
    F(MODASSIGN) \
    F(POWASSIGN)

/*
 * Parsing builds a tree of nodes that directs code generation.  This tree is
 * not a concrete syntax tree in all respects (for example, || and && are left
 * associative, but (A && B && C) translates into the right-associated tree
 * <A && <B && C>> so that code generation can emit a left-associative branch
 * around <B && C> when A is false).  Nodes are labeled by kind, with a
 * secondary JSOp label when needed.
 *
 * The long comment after this enum block describes the kinds in detail.
 */
enum ParseNodeKind
{
#define EMIT_ENUM(name) PNK_##name,
    FOR_EACH_PARSE_NODE_KIND(EMIT_ENUM)
#undef EMIT_ENUM
    PNK_LIMIT, /* domain size */
    PNK_BINOP_FIRST = PNK_OR,
    PNK_BINOP_LAST = PNK_POW,
    PNK_ASSIGNMENT_START = PNK_ASSIGN,
    PNK_ASSIGNMENT_LAST = PNK_POWASSIGN
};

inline bool
IsDeleteKind(ParseNodeKind kind)
{
    return PNK_DELETENAME <= kind && kind <= PNK_DELETEEXPR;
}

/*
 * Label        Variant     Members
 * -----        -------     -------
 * <Definitions>
 * PNK_FUNCTION name        pn_funbox: ptr to js::FunctionBox holding function
 *                            object containing arg and var properties.  We
 *                            create the function object at parse (not emit)
 *                            time to specialize arg and var bytecodes early.
 *                          pn_body: PNK_ARGSBODY, ordinarily;
 *                            PNK_LEXICALSCOPE for implicit function in genexpr
 *                          pn_scopecoord: hops and var index for function
 *                          pn_dflags: PND_* definition/use flags (see below)
 *                          pn_blockid: block id number
 * PNK_ARGSBODY list        list of formal parameters with
 *                              PNK_NAME node with non-empty name for
 *                                SingleNameBinding without Initializer
 *                              PNK_ASSIGN node for SingleNameBinding with
 *                                Initializer
 *                              PNK_NAME node with empty name for destructuring
 *                                pn_expr: PNK_ARRAY, PNK_OBJECT, or PNK_ASSIGN
 *                                  PNK_ARRAY or PNK_OBJECT for BindingPattern
 *                                    without Initializer
 *                                  PNK_ASSIGN for BindingPattern with
 *                                    Initializer
 *                          followed by:
 *                              PNK_STATEMENTLIST node for function body
 *                                statements,
 *                              PNK_RETURN for expression closure
 *                          pn_count: 1 + number of formal parameters
 *                          pn_tree: PNK_ARGSBODY or PNK_STATEMENTLIST node
 * PNK_SPREAD   unary       pn_kid: expression being spread
 *
 * <Statements>
 * PNK_STATEMENTLIST list   pn_head: list of pn_count statements
 * PNK_IF       ternary     pn_kid1: cond, pn_kid2: then, pn_kid3: else or null.
 *                            In body of a comprehension or desugared generator
 *                            expression, pn_kid2 is PNK_YIELD, PNK_ARRAYPUSH,
 *                            or (if the push was optimized away) empty
 *                            PNK_STATEMENTLIST.
 * PNK_SWITCH   binary      pn_left: discriminant
 *                          pn_right: list of PNK_CASE nodes, with at most one
 *                            default node, or if there are let bindings
 *                            in the top level of the switch body's cases, a
 *                            PNK_LEXICALSCOPE node that contains the list of
 *                            PNK_CASE nodes.
 * PNK_CASE     binary      pn_left: case-expression if CaseClause, or
 *                            null if DefaultClause
 *                          pn_right: PNK_STATEMENTLIST node for this case's
 *                            statements
 *                          pn_u.binary.offset: scratch space for the emitter
 * PNK_WHILE    binary      pn_left: cond, pn_right: body
 * PNK_DOWHILE  binary      pn_left: body, pn_right: cond
 * PNK_FOR      binary      pn_left: either PNK_FORIN (for-in statement),
 *                            PNK_FOROF (for-of) or PNK_FORHEAD (for(;;))
 *                          pn_right: body
 * PNK_COMPREHENSIONFOR     pn_left: either PNK_FORIN or PNK_FOROF
 *              binary      pn_right: body
 * PNK_FORIN    ternary     pn_kid1: PNK_VAR to left of 'in', or nullptr
 *                          pn_kid2: PNK_NAME or destructuring expr
 *                            to left of 'in'; if pn_kid1, then this
 *                            is a clone of pn_kid1->pn_head
 *                          pn_kid3: object expr to right of 'in'
 * PNK_FOROF    ternary     pn_kid1: PNK_VAR to left of 'of', or nullptr
 *                          pn_kid2: PNK_NAME or destructuring expr
 *                            to left of 'of'; if pn_kid1, then this
 *                            is a clone of pn_kid1->pn_head
 *                          pn_kid3: expr to right of 'of'
 * PNK_FORHEAD  ternary     pn_kid1:  init expr before first ';' or nullptr
 *                          pn_kid2:  cond expr before second ';' or nullptr
 *                          pn_kid3:  update expr after second ';' or nullptr
 * PNK_THROW    unary       pn_op: JSOP_THROW, pn_kid: exception
 * PNK_TRY      ternary     pn_kid1: try block
 *                          pn_kid2: null or PNK_CATCHLIST list
 *                          pn_kid3: null or finally block
 * PNK_CATCHLIST list       pn_head: list of PNK_LEXICALSCOPE nodes, one per
 *                                   catch-block, each with pn_expr pointing
 *                                   to a PNK_CATCH node
 * PNK_CATCH    ternary     pn_kid1: PNK_NAME, PNK_ARRAY, or PNK_OBJECT catch var node
 *                                   (PNK_ARRAY or PNK_OBJECT if destructuring)
 *                          pn_kid2: null or the catch guard expression
 *                          pn_kid3: catch block statements
 * PNK_BREAK    name        pn_atom: label or null
 * PNK_CONTINUE name        pn_atom: label or null
 * PNK_WITH     binary-obj  pn_left: head expr; pn_right: body; pn_binary_obj: StaticWithObject
 * PNK_VAR,     list        pn_head: list of PNK_NAME or PNK_ASSIGN nodes
 * PNK_CONST                         each name node has either
 *                                     pn_used: false
 *                                     pn_atom: variable name
 *                                     pn_expr: initializer or null
 *                                   or
 *                                     pn_used: true
 *                                     pn_atom: variable name
 *                                     pn_lexdef: def node
 *                                   each assignment node has
 *                                     pn_left: PNK_NAME with pn_used true and
 *                                              pn_lexdef (NOT pn_expr) set
 *                                     pn_right: initializer
 * PNK_RETURN   unary       pn_kid: return expr or null
 * PNK_SEMI     unary       pn_kid: expr or null statement
 *                          pn_prologue: true if Directive Prologue member
 *                              in original source, not introduced via
 *                              constant folding or other tree rewriting
 * PNK_LABEL    name        pn_atom: label, pn_expr: labeled statement
 * PNK_IMPORT   binary      pn_left: PNK_IMPORT_SPEC_LIST import specifiers
 *                          pn_right: PNK_STRING module specifier
 * PNK_EXPORT   unary       pn_kid: declaration expression
 * PNK_EXPORT_FROM binary   pn_left: PNK_EXPORT_SPEC_LIST export specifiers
 *                          pn_right: PNK_STRING module specifier
 * PNK_EXPORT_DEFAULT unary pn_kid: export default declaration or expression
 *
 * <Expressions>
 * All left-associated binary trees of the same type are optimized into lists
 * to avoid recursion when processing expression chains.
 * PNK_COMMA    list        pn_head: list of pn_count comma-separated exprs
 * PNK_ASSIGN   binary      pn_left: lvalue, pn_right: rvalue
 * PNK_ADDASSIGN,   binary  pn_left: lvalue, pn_right: rvalue
 * PNK_SUBASSIGN,           pn_op: JSOP_ADD for +=, etc.
 * PNK_BITORASSIGN,
 * PNK_BITXORASSIGN,
 * PNK_BITANDASSIGN,
 * PNK_LSHASSIGN,
 * PNK_RSHASSIGN,
 * PNK_URSHASSIGN,
 * PNK_MULASSIGN,
 * PNK_DIVASSIGN,
 * PNK_MODASSIGN,
 * PNK_POWASSIGN
 * PNK_CONDITIONAL ternary  (cond ? trueExpr : falseExpr)
 *                          pn_kid1: cond, pn_kid2: then, pn_kid3: else
 * PNK_OR,      list        pn_head; list of pn_count subexpressions
 * PNK_AND,                 All of these operators are left-associative except (**).
 * PNK_BITOR,
 * PNK_BITXOR,
 * PNK_BITAND,
 * PNK_EQ,
 * PNK_NE,
 * PNK_STRICTEQ,
 * PNK_STRICTNE,
 * PNK_LT,
 * PNK_LE,
 * PNK_GT,
 * PNK_GE,
 * PNK_LSH,
 * PNK_RSH,
 * PNK_URSH,
 * PNK_ADD,
 * PNK_SUB,
 * PNK_STAR,
 * PNK_DIV,
 * PNK_MOD,
 * PNK_POW                  (**) is right-associative, but forms a list
 *                          nonetheless. Special hacks everywhere.
 *
 * PNK_POS,     unary       pn_kid: UNARY expr
 * PNK_NEG
 * PNK_VOID,    unary       pn_kid: UNARY expr
 * PNK_NOT,
 * PNK_BITNOT
 * PNK_TYPEOFNAME, unary    pn_kid: UNARY expr
 * PNK_TYPEOFEXPR
 * PNK_PREINCREMENT, unary  pn_kid: MEMBER expr
 * PNK_POSTINCREMENT,
 * PNK_PREDECREMENT,
 * PNK_POSTDECREMENT
 * PNK_NEW      list        pn_head: list of ctor, arg1, arg2, ... argN
 *                          pn_count: 1 + N (where N is number of args)
 *                          ctor is a MEMBER expr
 * PNK_DELETENAME unary     pn_kid: PNK_NAME expr
 * PNK_DELETEPROP unary     pn_kid: PNK_DOT expr
 * PNK_DELETEELEM unary     pn_kid: PNK_ELEM expr
 * PNK_DELETESUPERELEM unary pn_kid: PNK_SUPERELEM expr
 * PNK_DELETEEXPR unary     pn_kid: MEMBER expr that's evaluated, then the
 *                          overall delete evaluates to true; can't be a kind
 *                          for a more-specific PNK_DELETE* unless constant
 *                          folding (or a similar parse tree manipulation) has
 *                          occurred
 * PNK_DOT      name        pn_expr: MEMBER expr to left of .
 *                          pn_atom: name to right of .
 * PNK_ELEM     binary      pn_left: MEMBER expr to left of [
 *                          pn_right: expr between [ and ]
 * PNK_CALL     list        pn_head: list of call, arg1, arg2, ... argN
 *                          pn_count: 1 + N (where N is number of args)
 *                          call is a MEMBER expr naming a callable object
 * PNK_GENEXP   list        Exactly like PNK_CALL, used for the implicit call
 *                          in the desugaring of a generator-expression.
 * PNK_ARRAY    list        pn_head: list of pn_count array element exprs
 *                          [,,] holes are represented by PNK_ELISION nodes
 *                          pn_xflags: PN_ENDCOMMA if extra comma at end
 * PNK_OBJECT   list        pn_head: list of pn_count binary PNK_COLON nodes
 * PNK_COLON    binary      key-value pair in object initializer or
 *                          destructuring lhs
 *                          pn_left: property id, pn_right: value
 * PNK_SHORTHAND binary     Same fields as PNK_COLON. This is used for object
 *                          literal properties using shorthand ({x}).
 * PNK_COMPUTED_NAME unary  ES6 ComputedPropertyName.
 *                          pn_kid: the AssignmentExpression inside the square brackets
 * PNK_NAME,    name        pn_atom: name, string, or object atom
 * PNK_STRING               pn_op: JSOP_GETNAME, JSOP_STRING, or JSOP_OBJECT
 *                          If JSOP_GETNAME, pn_op may be JSOP_*ARG or JSOP_*VAR
 *                          with pn_scoppecord telling (hops, slot) and pn_dflags
 *                          telling const-ness and static analysis results
 * PNK_TEMPLATE_STRING_LIST pn_head: list of alternating expr and template strings
 *              list
 * PNK_TEMPLATE_STRING      pn_atom: template string atom
                nullary     pn_op: JSOP_NOP
 * PNK_TAGGED_TEMPLATE      pn_head: list of call, call site object, arg1, arg2, ... argN
 *              list        pn_count: 2 + N (N is the number of substitutions)
 * PNK_CALLSITEOBJ list     pn_head: a PNK_ARRAY node followed by
 *                          list of pn_count - 1 PNK_TEMPLATE_STRING nodes
 * PNK_REGEXP   nullary     pn_objbox: RegExp model object
 * PNK_NAME     name        If pn_used, PNK_NAME uses the lexdef member instead
 *                          of the expr member it overlays
 * PNK_NUMBER   dval        pn_dval: double value of numeric literal
 * PNK_TRUE,    nullary     pn_op: JSOp bytecode
 * PNK_FALSE,
 * PNK_NULL
 *
 * PNK_THIS,        unary   pn_kid: '.this' Name if function `this`, else nullptr
 * PNK_SUPERBASE    unary   pn_kid: '.this' Name
 *
 * PNK_SETTHIS      binary  pn_left: '.this' Name, pn_right: SuperCall
 *
 * PNK_LEXICALSCOPE name    pn_objbox: block object in ObjectBox holder
 *                          pn_expr: block body
 * PNK_GENERATOR    nullary
 * PNK_YIELD,       binary  pn_left: expr or null; pn_right: generator object
 * PNK_YIELD_STAR
 * PNK_ARRAYCOMP    list    pn_count: 1
 *                          pn_head: list of 1 element, which is block
 *                          enclosing for loop(s) and optionally
 *                          if-guarded PNK_ARRAYPUSH
 * PNK_ARRAYPUSH    unary   pn_op: JSOP_ARRAYCOMP
 *                          pn_kid: array comprehension expression
 * PNK_NOP          nullary
 */
enum ParseNodeArity
{
    PN_NULLARY,                         /* 0 kids, only pn_atom/pn_dval/etc. */
    PN_UNARY,                           /* one kid, plus a couple of scalars */
    PN_BINARY,                          /* two kids, plus a couple of scalars */
    PN_BINARY_OBJ,                      /* two kids, plus an objbox */
    PN_TERNARY,                         /* three kids */
    PN_CODE,                            /* module or function definition node */
    PN_LIST,                            /* generic singly linked list */
    PN_NAME                             /* name use or definition node */
};

struct Definition;

class LoopControlStatement;
class BreakStatement;
class ContinueStatement;
class ConditionalExpression;
class PropertyAccess;

class ParseNode
{
    uint32_t            pn_type   : 16, /* PNK_* type */
                        pn_op     : 8,  /* see JSOp enum and jsopcode.tbl */
                        pn_arity  : 4,  /* see ParseNodeArity enum */
                        pn_parens : 1,  /* this expr was enclosed in parens */
                        pn_used   : 1,  /* name node is on a use-chain */
                        pn_defn   : 1;  /* this node is a Definition */

    ParseNode(const ParseNode& other) = delete;
    void operator=(const ParseNode& other) = delete;

  public:
    ParseNode(ParseNodeKind kind, JSOp op, ParseNodeArity arity)
      : pn_type(kind), pn_op(op), pn_arity(arity), pn_parens(0), pn_used(0), pn_defn(0),
        pn_pos(0, 0), pn_next(nullptr), pn_link(nullptr)
    {
        MOZ_ASSERT(kind < PNK_LIMIT);
        memset(&pn_u, 0, sizeof pn_u);
    }

    ParseNode(ParseNodeKind kind, JSOp op, ParseNodeArity arity, const TokenPos& pos)
      : pn_type(kind), pn_op(op), pn_arity(arity), pn_parens(0), pn_used(0), pn_defn(0),
        pn_pos(pos), pn_next(nullptr), pn_link(nullptr)
    {
        MOZ_ASSERT(kind < PNK_LIMIT);
        memset(&pn_u, 0, sizeof pn_u);
    }

    JSOp getOp() const                     { return JSOp(pn_op); }
    void setOp(JSOp op)                    { pn_op = op; }
    bool isOp(JSOp op) const               { return getOp() == op; }

    ParseNodeKind getKind() const {
        MOZ_ASSERT(pn_type < PNK_LIMIT);
        return ParseNodeKind(pn_type);
    }
    void setKind(ParseNodeKind kind) {
        MOZ_ASSERT(kind < PNK_LIMIT);
        pn_type = kind;
    }
    bool isKind(ParseNodeKind kind) const  { return getKind() == kind; }

    ParseNodeArity getArity() const        { return ParseNodeArity(pn_arity); }
    bool isArity(ParseNodeArity a) const   { return getArity() == a; }
    void setArity(ParseNodeArity a)        { pn_arity = a; }

    bool isAssignment() const {
        ParseNodeKind kind = getKind();
        return PNK_ASSIGNMENT_START <= kind && kind <= PNK_ASSIGNMENT_LAST;
    }

    bool isBinaryOperation() const {
        ParseNodeKind kind = getKind();
        return PNK_BINOP_FIRST <= kind && kind <= PNK_BINOP_LAST;
    }

    /* Boolean attributes. */
    bool isInParens() const                { return pn_parens; }
    bool isLikelyIIFE() const              { return isInParens(); }
    void setInParens(bool enabled)         { pn_parens = enabled; }
    bool isUsed() const                    { return pn_used; }
    void setUsed(bool enabled)             { pn_used = enabled; }
    bool isDefn() const                    { return pn_defn; }
    void setDefn(bool enabled)             { pn_defn = enabled; }

    static const unsigned NumDefinitionFlagBits = 10;
    static const unsigned NumListFlagBits = 10;
    static const unsigned NumBlockIdBits = 22;
    static_assert(NumDefinitionFlagBits == NumListFlagBits,
                  "Assumed below to achieve consistent blockid offset");
    static_assert(NumDefinitionFlagBits + NumBlockIdBits <= 32,
                  "This is supposed to fit in a single uint32_t");

    TokenPos            pn_pos;         /* two 16-bit pairs here, for 64 bits */
    ParseNode*          pn_next;        /* intrinsic link in parent PN_LIST */

    /*
     * Nodes that represent lexical bindings may, in addition to being
     * ParseNodes, also be Definition nodes. (Definition is defined far below,
     * with a lengthy comment that you should read.) Each binding has one
     * canonical Definition; all uses of that definition are reached starting
     * from dn_uses, then following subsequent pn_link pointers.
     *
     * The dn_uses chain elements are unordered. Any apparent ordering in some
     * cases, will not be present in all others.
     */
    union {
        ParseNode*      dn_uses;
        ParseNode*      pn_link;
    };

    union {
        struct {                        /* list of next-linked nodes */
            ParseNode*  head;          /* first node in list */
            ParseNode** tail;         /* ptr to ptr to last node in list */
            uint32_t    count;          /* number of nodes in list */
            uint32_t    xflags:NumListFlagBits, /* see PNX_* below */
                        blockid:NumBlockIdBits; /* see name variant below */
        } list;
        struct {                        /* ternary: if, for(;;), ?: */
            ParseNode*  kid1;          /* condition, discriminant, etc. */
            ParseNode*  kid2;          /* then-part, case list, etc. */
            ParseNode*  kid3;          /* else-part, default case, etc. */
        } ternary;
        struct {                        /* two kids if binary */
            ParseNode*  left;
            ParseNode*  right;
            union {
                unsigned iflags;        /* JSITER_* flags for PNK_{COMPREHENSION,}FOR node */
                ObjectBox* objbox;      /* only for PN_BINARY_OBJ */
                bool isStatic;          /* only for PNK_CLASSMETHOD */
                uint32_t offset;        /* for the emitter's use on PNK_CASE nodes */
            };
        } binary;
        struct {                        /* one kid if unary */
            ParseNode*  kid;
            bool        prologue;       /* directive prologue member (as
                                           pn_prologue) */
        } unary;
        struct {                        /* name, labeled statement, etc. */
            union {
                JSAtom*      atom;      /* lexical name or label atom */
                ObjectBox*   objbox;    /* block or regexp object */
                FunctionBox* funbox;    /* function object */
                ModuleBox*   modulebox; /* module object */
            };
            union {
                ParseNode*  expr;      /* module or function body, var
                                           initializer, argument default, or
                                           base object of PNK_DOT */
                Definition* lexdef;    /* lexical definition for this use */
            };
            PackedScopeCoordinate scopeCoord;
            uint32_t    dflags:NumDefinitionFlagBits, /* see PND_* below */
                        blockid:NumBlockIdBits;  /* block number, for subset dominance
                                                    computation */
        } name;
        struct {
            double      value;          /* aligned numeric literal value */
            DecimalPoint decimalPoint;  /* Whether the number has a decimal point */
        } number;
        class {
            friend class LoopControlStatement;
            PropertyName*    label;    /* target of break/continue statement */
        } loopControl;
    } pn_u;

#define pn_modulebox    pn_u.name.modulebox
#define pn_objbox       pn_u.name.objbox
#define pn_funbox       pn_u.name.funbox
#define pn_body         pn_u.name.expr
#define pn_scopecoord   pn_u.name.scopeCoord
#define pn_dflags       pn_u.name.dflags
#define pn_blockid      pn_u.name.blockid
#define pn_head         pn_u.list.head
#define pn_tail         pn_u.list.tail
#define pn_count        pn_u.list.count
#define pn_xflags       pn_u.list.xflags
#define pn_kid1         pn_u.ternary.kid1
#define pn_kid2         pn_u.ternary.kid2
#define pn_kid3         pn_u.ternary.kid3
#define pn_left         pn_u.binary.left
#define pn_right        pn_u.binary.right
#define pn_pval         pn_u.binary.pval
#define pn_iflags       pn_u.binary.iflags
#define pn_binary_obj   pn_u.binary.objbox
#define pn_kid          pn_u.unary.kid
#define pn_prologue     pn_u.unary.prologue
#define pn_atom         pn_u.name.atom
#define pn_objbox       pn_u.name.objbox
#define pn_expr         pn_u.name.expr
#define pn_lexdef       pn_u.name.lexdef
#define pn_dval         pn_u.number.value

  protected:
    void init(TokenKind type, JSOp op, ParseNodeArity arity) {
        pn_type = type;
        pn_op = op;
        pn_arity = arity;
        pn_parens = false;
        MOZ_ASSERT(!pn_used);
        MOZ_ASSERT(!pn_defn);
        pn_next = pn_link = nullptr;
    }

  public:
    /*
     * If |left| is a list of the given kind/left-associative op, append
     * |right| to it and return |left|.  Otherwise return a [left, right] list.
     */
    static ParseNode*
    appendOrCreateList(ParseNodeKind kind, JSOp op, ParseNode* left, ParseNode* right,
                       FullParseHandler* handler, ParseContext<FullParseHandler>* pc);

    inline PropertyName* name() const;
    inline JSAtom* atom() const;

    /*
     * The pn_expr and lexdef members are arms of an unsafe union. Unless you
     * know exactly what you're doing, use only the following methods to access
     * them. For less overhead and assertions for protection, use pn->expr()
     * and pn->lexdef(). Otherwise, use pn->maybeExpr() and pn->maybeLexDef().
     */
    ParseNode* expr() const {
        MOZ_ASSERT(!pn_used);
        MOZ_ASSERT(pn_arity == PN_NAME || pn_arity == PN_CODE);
        return pn_expr;
    }

    Definition* lexdef() const {
        MOZ_ASSERT(pn_used || isDeoptimized());
        MOZ_ASSERT(pn_arity == PN_NAME);
        return pn_lexdef;
    }

    ParseNode* maybeExpr()   { return pn_used ? nullptr : expr(); }
    Definition* maybeLexDef() { return pn_used ? lexdef() : nullptr; }

    Definition* resolve();

/* PN_CODE and PN_NAME pn_dflags bits. */
#define PND_LEXICAL             0x01    /* lexical (block-scoped) binding or use of a hoisted
                                           let or const */
#define PND_CONST               0x02    /* const binding (orthogonal to let) */
#define PND_ASSIGNED            0x04    /* set if ever LHS of assignment */
#define PND_PLACEHOLDER         0x08    /* placeholder definition for lexdep */
#define PND_BOUND               0x10    /* bound to a stack or global slot */
#define PND_DEOPTIMIZED         0x20    /* former pn_used name node, pn_lexdef
                                           still valid, but this use no longer
                                           optimizable via an upvar opcode */
#define PND_CLOSED              0x40    /* variable is closed over */
#define PND_KNOWNALIASED        0x80    /* definition known to be aliased and
                                           already has a translated pnk_scopecoord */
#define PND_IMPLICITARGUMENTS  0x100    /* the definition is a placeholder for
                                           'arguments' that has been converted
                                           into a definition after the function
                                           body has been parsed. */
#define PND_IMPORT             0x200    /* the definition is a module import. */

    static_assert(PND_IMPORT < (1 << NumDefinitionFlagBits), "Not enough bits");

/* Flags to propagate from uses to definition. */
#define PND_USE2DEF_FLAGS (PND_ASSIGNED | PND_CLOSED)

/* PN_LIST pn_xflags bits. */
#define PNX_FUNCDEFS    0x01            /* contains top-level function statements */
#define PNX_SETCALL     0x02            /* call expression in lvalue context */
#define PNX_ARRAYHOLESPREAD 0x04        /* one or more of
                                           1. array initialiser has holes
                                           2. array initializer has spread node */
#define PNX_NONCONST    0x08            /* initialiser has non-constants */

    static_assert(PNX_NONCONST < (1 << NumListFlagBits), "Not enough bits");

    uint32_t frameSlot() const {
        MOZ_ASSERT(pn_arity == PN_CODE || pn_arity == PN_NAME);
        return pn_scopecoord.slot();
    }

    bool functionIsHoisted() const {
        MOZ_ASSERT(pn_arity == PN_CODE && getKind() == PNK_FUNCTION);
        MOZ_ASSERT(isOp(JSOP_LAMBDA) ||        // lambda, genexpr
                   isOp(JSOP_LAMBDA_ARROW) ||  // arrow function
                   isOp(JSOP_DEFFUN) ||        // non-body-level function statement
                   isOp(JSOP_NOP) ||           // body-level function stmt in global code
                   isOp(JSOP_GETLOCAL) ||      // body-level function stmt in function code
                   isOp(JSOP_GETARG));         // body-level function redeclaring formal
        return !isOp(JSOP_LAMBDA) && !isOp(JSOP_LAMBDA_ARROW) && !isOp(JSOP_DEFFUN);
    }

    /*
     * True if this statement node could be a member of a Directive Prologue: an
     * expression statement consisting of a single string literal.
     *
     * This considers only the node and its children, not its context. After
     * parsing, check the node's pn_prologue flag to see if it is indeed part of
     * a directive prologue.
     *
     * Note that a Directive Prologue can contain statements that cannot
     * themselves be directives (string literals that include escape sequences
     * or escaped newlines, say). This member function returns true for such
     * nodes; we use it to determine the extent of the prologue.
     */
    JSAtom* isStringExprStatement() const {
        if (getKind() == PNK_SEMI) {
            MOZ_ASSERT(pn_arity == PN_UNARY);
            ParseNode* kid = pn_kid;
            if (kid && kid->getKind() == PNK_STRING && !kid->pn_parens)
                return kid->pn_atom;
        }
        return nullptr;
    }

    inline bool test(unsigned flag) const;

    bool isLexical() const      { return test(PND_LEXICAL) && !isUsed(); }
    bool isConst() const        { return test(PND_CONST); }
    bool isPlaceholder() const  { return test(PND_PLACEHOLDER); }
    bool isDeoptimized() const  { return test(PND_DEOPTIMIZED); }
    bool isAssigned() const     { return test(PND_ASSIGNED); }
    bool isClosed() const       { return test(PND_CLOSED); }
    bool isBound() const        { return test(PND_BOUND); }
    bool isImplicitArguments() const { return test(PND_IMPLICITARGUMENTS); }
    bool isHoistedLexicalUse() const { return test(PND_LEXICAL) && isUsed(); }
    bool isKnownAliased() const { return test(PND_KNOWNALIASED); }
    bool isImport() const       { return test(PND_IMPORT); }

    /* True if pn is a parsenode representing a literal constant. */
    bool isLiteral() const {
        return isKind(PNK_NUMBER) ||
               isKind(PNK_STRING) ||
               isKind(PNK_TRUE) ||
               isKind(PNK_FALSE) ||
               isKind(PNK_NULL);
    }

    /* Return true if this node appears in a Directive Prologue. */
    bool isDirectivePrologueMember() const { return pn_prologue; }

#ifdef JS_HAS_GENERATOR_EXPRS
    ParseNode* generatorExpr() const {
        MOZ_ASSERT(isKind(PNK_GENEXP));
        ParseNode* callee = this->pn_head;
        ParseNode* body = callee->pn_body;
        MOZ_ASSERT(body->isKind(PNK_STATEMENTLIST));
        MOZ_ASSERT(body->last()->isKind(PNK_LEXICALSCOPE) ||
                   body->last()->isKind(PNK_COMPREHENSIONFOR));
        return body->last();
    }
#endif

    inline void markAsAssigned();

    /*
     * Compute a pointer to the last element in a singly-linked list. NB: list
     * must be non-empty for correct PN_LAST usage -- this is asserted!
     */
    ParseNode* last() const {
        MOZ_ASSERT(pn_arity == PN_LIST);
        MOZ_ASSERT(pn_count != 0);
        return (ParseNode*)(uintptr_t(pn_tail) - offsetof(ParseNode, pn_next));
    }

    void initNumber(double value, DecimalPoint decimalPoint) {
        MOZ_ASSERT(pn_arity == PN_NULLARY);
        MOZ_ASSERT(getKind() == PNK_NUMBER);
        pn_u.number.value = value;
        pn_u.number.decimalPoint = decimalPoint;
    }

    void makeEmpty() {
        MOZ_ASSERT(pn_arity == PN_LIST);
        pn_head = nullptr;
        pn_tail = &pn_head;
        pn_count = 0;
        pn_xflags = 0;
        pn_blockid = 0;
    }

    void initList(ParseNode* pn) {
        MOZ_ASSERT(pn_arity == PN_LIST);
        if (pn->pn_pos.begin < pn_pos.begin)
            pn_pos.begin = pn->pn_pos.begin;
        pn_pos.end = pn->pn_pos.end;
        pn_head = pn;
        pn_tail = &pn->pn_next;
        pn_count = 1;
        pn_xflags = 0;
        pn_blockid = 0;
    }

    void append(ParseNode* pn) {
        MOZ_ASSERT(pn_arity == PN_LIST);
        MOZ_ASSERT(pn->pn_pos.begin >= pn_pos.begin);
        pn_pos.end = pn->pn_pos.end;
        *pn_tail = pn;
        pn_tail = &pn->pn_next;
        pn_count++;
    }

    void prepend(ParseNode* pn) {
        MOZ_ASSERT(pn_arity == PN_LIST);
        pn->pn_next = pn_head;
        pn_head = pn;
        if (pn_tail == &pn_head)
            pn_tail = &pn->pn_next;
        pn_count++;
    }

    void checkListConsistency()
#ifndef DEBUG
    {}
#endif
    ;

    enum AllowConstantObjects {
        DontAllowObjects = 0,
        AllowObjects,
        ForCopyOnWriteArray
    };

    bool getConstantValue(ExclusiveContext* cx, AllowConstantObjects allowObjects, MutableHandleValue vp,
                          Value* compare = nullptr, size_t ncompare = 0,
                          NewObjectKind newKind = TenuredObject);
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
    void dump();
    void dump(int indent);
#endif
};

struct NullaryNode : public ParseNode
{
    NullaryNode(ParseNodeKind kind, const TokenPos& pos)
      : ParseNode(kind, JSOP_NOP, PN_NULLARY, pos) {}
    NullaryNode(ParseNodeKind kind, JSOp op, const TokenPos& pos)
      : ParseNode(kind, op, PN_NULLARY, pos) {}

    // This constructor is for a few mad uses in the emitter. It populates
    // the pn_atom field even though that field belongs to a branch in pn_u
    // that nullary nodes shouldn't use -- bogus.
    NullaryNode(ParseNodeKind kind, JSOp op, const TokenPos& pos, JSAtom* atom)
      : ParseNode(kind, op, PN_NULLARY, pos)
    {
        pn_atom = atom;
    }

#ifdef DEBUG
    void dump();
#endif
};

struct UnaryNode : public ParseNode
{
    UnaryNode(ParseNodeKind kind, JSOp op, const TokenPos& pos, ParseNode* kid)
      : ParseNode(kind, op, PN_UNARY, pos)
    {
        pn_kid = kid;
    }

#ifdef DEBUG
    void dump(int indent);
#endif
};

struct BinaryNode : public ParseNode
{
    BinaryNode(ParseNodeKind kind, JSOp op, const TokenPos& pos, ParseNode* left, ParseNode* right)
      : ParseNode(kind, op, PN_BINARY, pos)
    {
        pn_left = left;
        pn_right = right;
    }

    BinaryNode(ParseNodeKind kind, JSOp op, ParseNode* left, ParseNode* right)
      : ParseNode(kind, op, PN_BINARY, TokenPos::box(left->pn_pos, right->pn_pos))
    {
        pn_left = left;
        pn_right = right;
    }

#ifdef DEBUG
    void dump(int indent);
#endif
};

struct BinaryObjNode : public ParseNode
{
    BinaryObjNode(ParseNodeKind kind, JSOp op, const TokenPos& pos, ParseNode* left, ParseNode* right,
                  ObjectBox* objbox)
      : ParseNode(kind, op, PN_BINARY_OBJ, pos)
    {
        pn_left = left;
        pn_right = right;
        pn_binary_obj = objbox;
    }

#ifdef DEBUG
    void dump(int indent);
#endif
};

struct TernaryNode : public ParseNode
{
    TernaryNode(ParseNodeKind kind, JSOp op, ParseNode* kid1, ParseNode* kid2, ParseNode* kid3)
      : ParseNode(kind, op, PN_TERNARY,
                  TokenPos((kid1 ? kid1 : kid2 ? kid2 : kid3)->pn_pos.begin,
                           (kid3 ? kid3 : kid2 ? kid2 : kid1)->pn_pos.end))
    {
        pn_kid1 = kid1;
        pn_kid2 = kid2;
        pn_kid3 = kid3;
    }

    TernaryNode(ParseNodeKind kind, JSOp op, ParseNode* kid1, ParseNode* kid2, ParseNode* kid3,
                const TokenPos& pos)
      : ParseNode(kind, op, PN_TERNARY, pos)
    {
        pn_kid1 = kid1;
        pn_kid2 = kid2;
        pn_kid3 = kid3;
    }

#ifdef DEBUG
    void dump(int indent);
#endif
};

struct ListNode : public ParseNode
{
    ListNode(ParseNodeKind kind, const TokenPos& pos)
      : ParseNode(kind, JSOP_NOP, PN_LIST, pos)
    {
        makeEmpty();
    }

    ListNode(ParseNodeKind kind, JSOp op, const TokenPos& pos)
      : ParseNode(kind, op, PN_LIST, pos)
    {
        makeEmpty();
    }

    ListNode(ParseNodeKind kind, JSOp op, ParseNode* kid)
      : ParseNode(kind, op, PN_LIST, kid->pn_pos)
    {
        initList(kid);
    }

    static bool test(const ParseNode& node) {
        return node.isArity(PN_LIST);
    }

#ifdef DEBUG
    void dump(int indent);
#endif
};

struct CodeNode : public ParseNode
{
    CodeNode(ParseNodeKind kind, const TokenPos& pos)
      : ParseNode(kind, JSOP_NOP, PN_CODE, pos)
    {
        MOZ_ASSERT(kind == PNK_FUNCTION || kind == PNK_MODULE);
        MOZ_ASSERT(!pn_body);
        MOZ_ASSERT(!pn_objbox);
        MOZ_ASSERT(pn_dflags == 0);
        pn_scopecoord.makeFree();
    }

  public:
#ifdef DEBUG
    void dump(int indent);
#endif
};

struct NameNode : public ParseNode
{
    NameNode(ParseNodeKind kind, JSOp op, JSAtom* atom, uint32_t blockid,
             const TokenPos& pos)
      : ParseNode(kind, op, PN_NAME, pos)
    {
        pn_atom = atom;
        pn_expr = nullptr;
        pn_scopecoord.makeFree();
        pn_dflags = 0;
        pn_blockid = blockid;
        MOZ_ASSERT(pn_blockid == blockid);  // check for bitfield overflow
    }

#ifdef DEBUG
    void dump(int indent);
#endif
};

struct LexicalScopeNode : public ParseNode
{
    LexicalScopeNode(ObjectBox* blockBox, const TokenPos& pos)
      : ParseNode(PNK_LEXICALSCOPE, JSOP_NOP, PN_NAME, pos)
    {
        MOZ_ASSERT(!pn_expr);
        MOZ_ASSERT(pn_dflags == 0);
        MOZ_ASSERT(pn_blockid == 0);
        pn_objbox = blockBox;
        pn_scopecoord.makeFree();
    }

    LexicalScopeNode(ObjectBox* blockBox, ParseNode* blockNode)
      : ParseNode(PNK_LEXICALSCOPE, JSOP_NOP, PN_NAME, blockNode->pn_pos)
    {
        pn_objbox = blockBox;
        pn_expr = blockNode;
        pn_blockid = blockNode->pn_blockid;
    }

    static bool test(const ParseNode& node) {
        return node.isKind(PNK_LEXICALSCOPE);
    }
};

class LabeledStatement : public ParseNode
{
  public:
    LabeledStatement(PropertyName* label, ParseNode* stmt, uint32_t begin)
      : ParseNode(PNK_LABEL, JSOP_NOP, PN_NAME, TokenPos(begin, stmt->pn_pos.end))
    {
        pn_atom = label;
        pn_expr = stmt;
    }

    PropertyName* label() const {
        return pn_atom->asPropertyName();
    }

    ParseNode* statement() const {
        return pn_expr;
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_LABEL);
        MOZ_ASSERT_IF(match, node.isArity(PN_NAME));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_NOP));
        return match;
    }
};

// Inside a switch statement, a CaseClause is a case-label and the subsequent
// statements. The same node type is used for DefaultClauses. The only
// difference is that their caseExpression() is null.
class CaseClause : public BinaryNode
{
  public:
    CaseClause(ParseNode* expr, ParseNode* stmts, uint32_t begin)
      : BinaryNode(PNK_CASE, JSOP_NOP, TokenPos(begin, stmts->pn_pos.end), expr, stmts) {}

    ParseNode* caseExpression() const { return pn_left; }
    bool isDefault() const { return !caseExpression(); }
    ParseNode* statementList() const { return pn_right; }

    // The next CaseClause in the same switch statement.
    CaseClause* next() const { return pn_next ? &pn_next->as<CaseClause>() : nullptr; }

    // Scratch space used by the emitter.
    uint32_t offset() const { return pn_u.binary.offset; }
    void setOffset(uint32_t u) { pn_u.binary.offset = u; }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_CASE);
        MOZ_ASSERT_IF(match, node.isArity(PN_BINARY));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_NOP));
        return match;
    }
};

class LoopControlStatement : public ParseNode
{
  protected:
    LoopControlStatement(ParseNodeKind kind, PropertyName* label, const TokenPos& pos)
      : ParseNode(kind, JSOP_NOP, PN_NULLARY, pos)
    {
        MOZ_ASSERT(kind == PNK_BREAK || kind == PNK_CONTINUE);
        pn_u.loopControl.label = label;
    }

  public:
    /* Label associated with this break/continue statement, if any. */
    PropertyName* label() const {
        return pn_u.loopControl.label;
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_BREAK) || node.isKind(PNK_CONTINUE);
        MOZ_ASSERT_IF(match, node.isArity(PN_NULLARY));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_NOP));
        return match;
    }
};

class BreakStatement : public LoopControlStatement
{
  public:
    BreakStatement(PropertyName* label, const TokenPos& pos)
      : LoopControlStatement(PNK_BREAK, label, pos)
    { }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_BREAK);
        MOZ_ASSERT_IF(match, node.isArity(PN_NULLARY));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_NOP));
        return match;
    }
};

class ContinueStatement : public LoopControlStatement
{
  public:
    ContinueStatement(PropertyName* label, const TokenPos& pos)
      : LoopControlStatement(PNK_CONTINUE, label, pos)
    { }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_CONTINUE);
        MOZ_ASSERT_IF(match, node.isArity(PN_NULLARY));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_NOP));
        return match;
    }
};

class DebuggerStatement : public ParseNode
{
  public:
    explicit DebuggerStatement(const TokenPos& pos)
      : ParseNode(PNK_DEBUGGER, JSOP_NOP, PN_NULLARY, pos)
    { }
};

class ConditionalExpression : public ParseNode
{
  public:
    ConditionalExpression(ParseNode* condition, ParseNode* thenExpr, ParseNode* elseExpr)
      : ParseNode(PNK_CONDITIONAL, JSOP_NOP, PN_TERNARY,
                  TokenPos(condition->pn_pos.begin, elseExpr->pn_pos.end))
    {
        MOZ_ASSERT(condition);
        MOZ_ASSERT(thenExpr);
        MOZ_ASSERT(elseExpr);
        pn_u.ternary.kid1 = condition;
        pn_u.ternary.kid2 = thenExpr;
        pn_u.ternary.kid3 = elseExpr;
    }

    ParseNode& condition() const {
        return *pn_u.ternary.kid1;
    }

    ParseNode& thenExpression() const {
        return *pn_u.ternary.kid2;
    }

    ParseNode& elseExpression() const {
        return *pn_u.ternary.kid3;
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_CONDITIONAL);
        MOZ_ASSERT_IF(match, node.isArity(PN_TERNARY));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_NOP));
        return match;
    }
};

class ThisLiteral : public UnaryNode
{
  public:
    ThisLiteral(const TokenPos& pos, ParseNode* thisName)
      : UnaryNode(PNK_THIS, JSOP_NOP, pos, thisName)
    { }
};

class NullLiteral : public ParseNode
{
  public:
    explicit NullLiteral(const TokenPos& pos) : ParseNode(PNK_NULL, JSOP_NULL, PN_NULLARY, pos) { }
};

class BooleanLiteral : public ParseNode
{
  public:
    BooleanLiteral(bool b, const TokenPos& pos)
      : ParseNode(b ? PNK_TRUE : PNK_FALSE, b ? JSOP_TRUE : JSOP_FALSE, PN_NULLARY, pos)
    { }
};

class RegExpLiteral : public NullaryNode
{
  public:
    RegExpLiteral(ObjectBox* reobj, const TokenPos& pos)
      : NullaryNode(PNK_REGEXP, JSOP_REGEXP, pos)
    {
        pn_objbox = reobj;
    }

    ObjectBox* objbox() const { return pn_objbox; }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_REGEXP);
        MOZ_ASSERT_IF(match, node.isArity(PN_NULLARY));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_REGEXP));
        return match;
    }
};

class PropertyAccess : public ParseNode
{
  public:
    PropertyAccess(ParseNode* lhs, PropertyName* name, uint32_t begin, uint32_t end)
      : ParseNode(PNK_DOT, JSOP_NOP, PN_NAME, TokenPos(begin, end))
    {
        MOZ_ASSERT(lhs != nullptr);
        MOZ_ASSERT(name != nullptr);
        pn_u.name.expr = lhs;
        pn_u.name.atom = name;
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_DOT);
        MOZ_ASSERT_IF(match, node.isArity(PN_NAME));
        return match;
    }

    ParseNode& expression() const {
        return *pn_u.name.expr;
    }

    PropertyName& name() const {
        return *pn_u.name.atom->asPropertyName();
    }

    bool isSuper() const {
        // PNK_SUPERBASE cannot result from any expression syntax.
        return expression().isKind(PNK_SUPERBASE);
    }
};

class PropertyByValue : public ParseNode
{
  public:
    PropertyByValue(ParseNode* lhs, ParseNode* propExpr, uint32_t begin, uint32_t end)
      : ParseNode(PNK_ELEM, JSOP_NOP, PN_BINARY, TokenPos(begin, end))
    {
        pn_u.binary.left = lhs;
        pn_u.binary.right = propExpr;
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_ELEM);
        MOZ_ASSERT_IF(match, node.isArity(PN_BINARY));
        return match;
    }

    bool isSuper() const {
        return pn_left->isKind(PNK_SUPERBASE);
    }
};

/*
 * A CallSiteNode represents the implicit call site object argument in a TaggedTemplate.
 */
struct CallSiteNode : public ListNode {
    explicit CallSiteNode(uint32_t begin): ListNode(PNK_CALLSITEOBJ, TokenPos(begin, begin + 1)) {}

    static bool test(const ParseNode& node) {
        return node.isKind(PNK_CALLSITEOBJ);
    }

    bool getRawArrayValue(ExclusiveContext* cx, MutableHandleValue vp) {
        return pn_head->getConstantValue(cx, AllowObjects, vp);
    }
};

struct ClassMethod : public BinaryNode {
    /*
     * Method defintions often keep a name and function body that overlap,
     * so explicitly define the beginning and end here.
     */
    ClassMethod(ParseNode* name, ParseNode* body, JSOp op, bool isStatic)
      : BinaryNode(PNK_CLASSMETHOD, op, TokenPos(name->pn_pos.begin, body->pn_pos.end), name, body)
    {
        pn_u.binary.isStatic = isStatic;
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_CLASSMETHOD);
        MOZ_ASSERT_IF(match, node.isArity(PN_BINARY));
        return match;
    }

    ParseNode& name() const {
        return *pn_u.binary.left;
    }
    ParseNode& method() const {
        return *pn_u.binary.right;
    }
    bool isStatic() const {
        return pn_u.binary.isStatic;
    }
};

struct ClassNames : public BinaryNode {
    ClassNames(ParseNode* outerBinding, ParseNode* innerBinding, const TokenPos& pos)
      : BinaryNode(PNK_CLASSNAMES, JSOP_NOP, pos, outerBinding, innerBinding)
    {
        MOZ_ASSERT_IF(outerBinding, outerBinding->isKind(PNK_NAME));
        MOZ_ASSERT(innerBinding->isKind(PNK_NAME));
        MOZ_ASSERT_IF(outerBinding, innerBinding->pn_atom == outerBinding->pn_atom);
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_CLASSNAMES);
        MOZ_ASSERT_IF(match, node.isArity(PN_BINARY));
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
    ParseNode* outerBinding() const {
        return pn_u.binary.left;
    }
    ParseNode* innerBinding() const {
        return pn_u.binary.right;
    }
};

struct ClassNode : public TernaryNode {
    ClassNode(ParseNode* names, ParseNode* heritage, ParseNode* methodsOrBlock)
      : TernaryNode(PNK_CLASS, JSOP_NOP, names, heritage, methodsOrBlock)
    {
        MOZ_ASSERT_IF(names, names->is<ClassNames>());
        MOZ_ASSERT(methodsOrBlock->is<LexicalScopeNode>() ||
                   methodsOrBlock->isKind(PNK_CLASSMETHODLIST));
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(PNK_CLASS);
        MOZ_ASSERT_IF(match, node.isArity(PN_TERNARY));
        return match;
    }

    ClassNames* names() const {
        return pn_kid1 ? &pn_kid1->as<ClassNames>() : nullptr;
    }
    ParseNode* heritage() const {
        return pn_kid2;
    }
    ParseNode* methodList() const {
        if (pn_kid3->isKind(PNK_CLASSMETHODLIST))
            return pn_kid3;

        MOZ_ASSERT(pn_kid3->is<LexicalScopeNode>());
        ParseNode* list = pn_kid3->pn_expr;
        MOZ_ASSERT(list->isKind(PNK_CLASSMETHODLIST));
        return list;
    }
    ObjectBox* scopeObject() const {
        MOZ_ASSERT(pn_kid3->is<LexicalScopeNode>());
        return pn_kid3->pn_objbox;
    }
};

#ifdef DEBUG
void DumpParseTree(ParseNode* pn, int indent = 0);
#endif

/*
 * js::Definition is a degenerate subtype of the PN_FUNC and PN_NAME variants
 * of js::ParseNode, allocated only for function, var, const, and let
 * declarations that define truly lexical bindings. This means that a child of
 * a PNK_VAR list may be a Definition as well as a ParseNode. The pn_defn bit
 * is set for all Definitions, clear otherwise.
 *
 * In an upvars list, defn->resolve() is the outermost definition the
 * name may reference. If a with block or a function that calls eval encloses
 * the use, the name may end up referring to something else at runtime.
 *
 * Note that not all var declarations are definitions: JS allows multiple var
 * declarations in a function or script, but only the first creates the hoisted
 * binding. JS programmers do redeclare variables for good refactoring reasons,
 * for example:
 *
 *   function foo() {
 *       ...
 *       for (var i ...) ...;
 *       ...
 *       for (var i ...) ...;
 *       ...
 *   }
 *
 * Not all definitions bind lexical variables, alas. In global and eval code
 * var may re-declare a pre-existing property having any attributes, with or
 * without JSPROP_PERMANENT. In eval code, indeed, ECMA-262 Editions 1 through
 * 3 require function and var to bind deletable bindings. Global vars thus are
 * properties of the global object, so they can be aliased even if they can't
 * be deleted.
 *
 * Only bindings within function code may be treated as lexical, of course with
 * the caveat that hoisting means use before initialization is allowed. We deal
 * with use before declaration in one pass as follows (error checking elided):
 *
 *   for (each use of unqualified name x in parse order) {
 *       if (this use of x is a declaration) {
 *           if (x in pc->decls) {                          // redeclaring
 *               pn = allocate a PN_NAME ParseNode;
 *           } else {                                       // defining
 *               dn = lookup x in pc->lexdeps;
 *               if (dn)                                    // use before def
 *                   remove x from pc->lexdeps;
 *               else                                       // def before use
 *                   dn = allocate a PN_NAME Definition;
 *               map x to dn via pc->decls;
 *               pn = dn;
 *           }
 *           insert pn into its parent PNK_VAR/PNK_CONST list;
 *       } else {
 *           pn = allocate a ParseNode for this reference to x;
 *           dn = lookup x in pc's lexical scope chain;
 *           if (!dn) {
 *               dn = lookup x in pc->lexdeps;
 *               if (!dn) {
 *                   dn = pre-allocate a Definition for x;
 *                   map x to dn in pc->lexdeps;
 *               }
 *           }
 *           append pn to dn's use chain;
 *       }
 *   }
 *
 * See frontend/BytecodeEmitter.h for js::ParseContext and its top*Stmt,
 * decls, and lexdeps members.
 *
 * Notes:
 *
 *  0. To avoid bloating ParseNode, we steal a bit from pn_arity for pn_defn
 *     and set it on a ParseNode instead of allocating a Definition.
 *
 *  1. Due to hoisting, a definition cannot be eliminated even if its "Variable
 *     statement" (ECMA-262 12.2) can be proven to be dead code. RecycleTree in
 *     ParseNode.cpp will not recycle a node whose pn_defn bit is set.
 *
 *  2. "lookup x in pc's lexical scope chain" gives up on def/use chaining if a
 *     with statement is found along the the scope chain, which includes pc,
 *     pc->parent, etc. Thus we eagerly connect an inner function's use of an
 *     outer's var x if the var x was parsed before the inner function.
 *
 *  3. A use may be eliminated as dead by the constant folder, which therefore
 *     must remove the dead name node from its singly-linked use chain, which
 *     would mean hashing to find the definition node and searching to update
 *     the pn_link pointing at the use to be removed. This is costly, so as for
 *     dead definitions, we do not recycle dead pn_used nodes.
 *
 * At the end of parsing a function body or global or eval program, pc->lexdeps
 * holds the lexical dependencies of the parsed unit. The name to def/use chain
 * mappings are then merged into the parent pc->lexdeps.
 *
 * Thus if a later var x is parsed in the outer function satisfying an earlier
 * inner function's use of x, we will remove dn from pc->lexdeps and re-use it
 * as the new definition node in the outer function's parse tree.
 *
 * When the compiler unwinds from the outermost pc, pc->lexdeps contains the
 * definition nodes with use chains for all free variables. These are either
 * global variables or reference errors.
 */
struct Definition : public ParseNode
{
    bool isFreeVar() const {
        MOZ_ASSERT(isDefn());
        return pn_scopecoord.isFree();
    }

    enum Kind {
        MISSING = 0,
        VAR,
        CONSTANT,
        LET,
        ARG,
        NAMED_LAMBDA,
        PLACEHOLDER,
        IMPORT
    };

    bool canHaveInitializer() { return int(kind()) <= int(ARG); }

    static const char* kindString(Kind kind);

    Kind kind() {
        if (getKind() == PNK_FUNCTION) {
            if (isOp(JSOP_GETARG))
                return ARG;
            return VAR;
        }
        MOZ_ASSERT(getKind() == PNK_NAME);
        if (isOp(JSOP_CALLEE))
            return NAMED_LAMBDA;
        if (isPlaceholder())
            return PLACEHOLDER;
        if (isOp(JSOP_GETARG))
            return ARG;
        if (isImport())
            return IMPORT;
        if (isLexical())
            return isConst() ? CONSTANT : LET;
        return VAR;
    }
};

class ParseNodeAllocator
{
  public:
    explicit ParseNodeAllocator(ExclusiveContext* cx, LifoAlloc& alloc)
      : cx(cx), alloc(alloc), freelist(nullptr)
    {}

    void* allocNode();
    void freeNode(ParseNode* pn);
    ParseNode* freeTree(ParseNode* pn);
    void prepareNodeForMutation(ParseNode* pn);

  private:
    ExclusiveContext* cx;
    LifoAlloc& alloc;
    ParseNode* freelist;
};

inline bool
ParseNode::test(unsigned flag) const
{
    MOZ_ASSERT(pn_defn || pn_arity == PN_CODE || pn_arity == PN_NAME);
#ifdef DEBUG
    if ((flag & PND_ASSIGNED) && pn_defn && !(pn_dflags & flag)) {
        for (ParseNode* pn = ((Definition*) this)->dn_uses; pn; pn = pn->pn_link) {
            MOZ_ASSERT(!pn->pn_defn);
            MOZ_ASSERT(!(pn->pn_dflags & flag));
        }
    }
#endif
    return !!(pn_dflags & flag);
}

inline void
ParseNode::markAsAssigned()
{
    MOZ_ASSERT(CodeSpec[pn_op].format & JOF_NAME);
    if (isUsed())
        pn_lexdef->pn_dflags |= PND_ASSIGNED;
    pn_dflags |= PND_ASSIGNED;
}

inline Definition*
ParseNode::resolve()
{
    if (isDefn())
        return (Definition*)this;
    MOZ_ASSERT(lexdef()->isDefn());
    return (Definition*)lexdef();
}

inline bool
ParseNode::isConstant()
{
    switch (pn_type) {
      case PNK_NUMBER:
      case PNK_STRING:
      case PNK_TEMPLATE_STRING:
      case PNK_NULL:
      case PNK_FALSE:
      case PNK_TRUE:
        return true;
      case PNK_ARRAY:
      case PNK_OBJECT:
        MOZ_ASSERT(isOp(JSOP_NEWINIT));
        return !(pn_xflags & PNX_NONCONST);
      default:
        return false;
    }
}

class ObjectBox
{
  public:
    JSObject* object;

    ObjectBox(JSObject* object, ObjectBox* traceLink);
    bool isFunctionBox() { return object->is<JSFunction>(); }
    FunctionBox* asFunctionBox();
    bool isModuleBox() { return object->is<ModuleObject>(); }
    ModuleBox* asModuleBox();
    void trace(JSTracer* trc);

  protected:
    friend struct CGObjectList;

    ObjectBox* traceLink;
    ObjectBox* emitLink;

    ObjectBox(JSFunction* function, ObjectBox* traceLink);
};

enum ParseReportKind
{
    ParseError,
    ParseWarning,
    ParseExtraWarning,
    ParseStrictError
};

enum FunctionSyntaxKind
{
    Expression,
    Statement,
    Arrow,
    Method,
    ClassConstructor,
    DerivedClassConstructor,
    Getter,
    GetterNoExpressionClosure,
    Setter,
    SetterNoExpressionClosure
};

static inline bool
IsConstructorKind(FunctionSyntaxKind kind)
{
    return kind == ClassConstructor || kind == DerivedClassConstructor;
}

static inline bool
IsGetterKind(FunctionSyntaxKind kind)
{
    return kind == Getter || kind == GetterNoExpressionClosure;
}

static inline bool
IsSetterKind(FunctionSyntaxKind kind)
{
    return kind == Setter || kind == SetterNoExpressionClosure;
}

static inline ParseNode*
FunctionArgsList(ParseNode* fn, unsigned* numFormals)
{
    MOZ_ASSERT(fn->isKind(PNK_FUNCTION));
    ParseNode* argsBody = fn->pn_body;
    MOZ_ASSERT(argsBody->isKind(PNK_ARGSBODY));
    *numFormals = argsBody->pn_count;
    if (*numFormals > 0 && argsBody->last()->isKind(PNK_STATEMENTLIST))
        (*numFormals)--;
    MOZ_ASSERT(argsBody->isArity(PN_LIST));
    return argsBody->pn_head;
}

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_ParseNode_h */
