/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ParseNode_h
#define frontend_ParseNode_h

#include "mozilla/Attributes.h"

#include "builtin/ModuleObject.h"
#include "frontend/TokenStream.h"
#include "vm/Printer.h"

// A few notes on lifetime of ParseNode trees:
//
// - All the `ParseNode` instances MUST BE explicitly allocated in the context's `LifoAlloc`.
//   This is typically implemented by the `FullParseHandler` or it can be reimplemented with
//   a custom `new_`.
//
// - The tree is bulk-deallocated when the parser is deallocated. Consequently, references
//   to a subtree MUST NOT exist once the parser has been deallocated.
//
// - This bulk-deallocation DOES NOT run destructors.
//
// - Instances of `LexicalScope::Data` MUST BE allocated as instances of `ParseNode`, in the same
//   `LifoAlloc`. They are bulk-deallocated alongside the rest of the tree.
//
// - Instances of `JSAtom` used throughout the tree (including instances of `PropertyName`) MUST
//   be kept alive by the parser. This is done through an instance of `AutoKeepAtoms` held by
//   the parser.
//
// - Once the parser is deallocated, the `JSAtom` instances MAY be garbage-collected.


namespace js {
namespace frontend {

class ParseContext;
class FullParseHandler;
class FunctionBox;
class ObjectBox;

#define FOR_EACH_PARSE_NODE_KIND(F) \
    F(EmptyStatement) \
    F(ExpressionStatement) \
    F(Comma) \
    F(Conditional) \
    F(Colon) \
    F(Shorthand) \
    F(Pos) \
    F(Neg) \
    F(PreIncrement) \
    F(PostIncrement) \
    F(PreDecrement) \
    F(PostDecrement) \
    F(Dot) \
    F(Elem) \
    F(Array) \
    F(Elision) \
    F(StatementList) \
    F(Label) \
    F(Object) \
    F(Call) \
    F(Name) \
    F(ObjectPropertyName) \
    F(ComputedName) \
    F(Number) \
    F(String) \
    F(TemplateStringList) \
    F(TemplateString) \
    F(TaggedTemplate) \
    F(CallSiteObj) \
    F(RegExp) \
    F(True) \
    F(False) \
    F(Null) \
    F(RawUndefined) \
    F(This) \
    F(Function) \
    F(Module) \
    F(If) \
    F(Switch) \
    F(Case) \
    F(While) \
    F(DoWhile) \
    F(For) \
    F(Break) \
    F(Continue) \
    F(Var) \
    F(Const) \
    F(With) \
    F(Return) \
    F(New) \
    /* Delete operations.  These must be sequential. */ \
    F(DeleteName) \
    F(DeleteProp) \
    F(DeleteElem) \
    F(DeleteExpr) \
    F(Try) \
    F(Catch) \
    F(Throw) \
    F(Debugger) \
    F(Generator) \
    F(InitialYield) \
    F(Yield) \
    F(YieldStar) \
    F(LexicalScope) \
    F(Let) \
    F(Import) \
    F(ImportSpecList) \
    F(ImportSpec) \
    F(Export) \
    F(ExportFrom) \
    F(ExportDefault) \
    F(ExportSpecList) \
    F(ExportSpec) \
    F(ExportBatchSpec) \
    F(ForIn) \
    F(ForOf) \
    F(ForHead) \
    F(ParamsBody) \
    F(Spread) \
    F(MutateProto) \
    F(Class) \
    F(ClassMethod) \
    F(ClassMethodList) \
    F(ClassNames) \
    F(NewTarget) \
    F(PosHolder) \
    F(SuperBase) \
    F(SuperCall) \
    F(SetThis) \
    \
    /* Unary operators. */ \
    F(TypeOfName) \
    F(TypeOfExpr) \
    F(Void) \
    F(Not) \
    F(BitNot) \
    F(Await) \
    \
    /* \
     * Binary operators. \
     * These must be in the same order as TOK_OR and friends in TokenStream.h. \
     */ \
    F(Pipeline) \
    F(Or) \
    F(And) \
    F(BitOr) \
    F(BitXor) \
    F(BitAnd) \
    F(StrictEq) \
    F(Eq) \
    F(StrictNe) \
    F(Ne) \
    F(Lt) \
    F(Le) \
    F(Gt) \
    F(Ge) \
    F(InstanceOf) \
    F(In) \
    F(Lsh) \
    F(Rsh) \
    F(Ursh) \
    F(Add) \
    F(Sub) \
    F(Star) \
    F(Div) \
    F(Mod) \
    F(Pow) \
    \
    /* Assignment operators (= += -= etc.). */ \
    /* ParseNode::isAssignment assumes all these are consecutive. */ \
    F(Assign) \
    F(AddAssign) \
    F(SubAssign) \
    F(BitOrAssign) \
    F(BitXorAssign) \
    F(BitAndAssign) \
    F(LshAssign) \
    F(RshAssign) \
    F(UrshAssign) \
    F(MulAssign) \
    F(DivAssign) \
    F(ModAssign) \
    F(PowAssign)

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
enum class ParseNodeKind : uint16_t
{
#define EMIT_ENUM(name) name,
    FOR_EACH_PARSE_NODE_KIND(EMIT_ENUM)
#undef EMIT_ENUM
    Limit, /* domain size */
    BinOpFirst = ParseNodeKind::Pipeline,
    BinOpLast = ParseNodeKind::Pow,
    AssignmentStart = ParseNodeKind::Assign,
    AssignmentLast = ParseNodeKind::PowAssign
};

inline bool
IsDeleteKind(ParseNodeKind kind)
{
    return ParseNodeKind::DeleteName <= kind && kind <= ParseNodeKind::DeleteExpr;
}

inline bool
IsTypeofKind(ParseNodeKind kind)
{
    return ParseNodeKind::TypeOfName <= kind && kind <= ParseNodeKind::TypeOfExpr;
}

/*
 * Label        Variant     Members
 * -----        -------     -------
 * <Definitions>
 * Function name        pn_funbox: ptr to js::FunctionBox holding function
 *                            object containing arg and var properties.  We
 *                            create the function object at parse (not emit)
 *                            time to specialize arg and var bytecodes early.
 *                          pn_body: ParamsBody, ordinarily;
 *                            ParseNodeKind::LexicalScope for implicit function in genexpr
 * ParamsBody list      list of formal parameters with
 *                              Name node with non-empty name for
 *                                SingleNameBinding without Initializer
 *                              Assign node for SingleNameBinding with
 *                                Initializer
 *                              Name node with empty name for destructuring
 *                                pn_expr: Array, Object, or Assign
 *                                  Array or Object for BindingPattern
 *                                    without Initializer
 *                                  Assign for BindingPattern with
 *                                    Initializer
 *                          followed by:
 *                              StatementList node for function body
 *                                statements,
 *                              Return for expression closure
 *                          pn_count: 1 + number of formal parameters
 *                          pn_tree: ParamsBody or StatementList node
 * Spread   unary       pn_kid: expression being spread
 *
 * <Statements>
 * StatementList list   pn_head: list of pn_count statements
 * If       ternary     pn_kid1: cond, pn_kid2: then, pn_kid3: else or null.
 * Switch   binary      pn_left: discriminant
 *                          pn_right: list of Case nodes, with at most one
 *                            default node, or if there are let bindings
 *                            in the top level of the switch body's cases, a
 *                            LexicalScope node that contains the list of
 *                            Case nodes.
 * Case     binary      pn_left: case-expression if CaseClause, or
 *                            null if DefaultClause
 *                          pn_right: StatementList node for this case's
 *                            statements
 *                          pn_u.binary.offset: scratch space for the emitter
 * While    binary      pn_left: cond, pn_right: body
 * DoWhile  binary      pn_left: body, pn_right: cond
 * For      binary      pn_left: either ForIn (for-in statement),
 *                            ForOf (for-of) or ForHead (for(;;))
 *                          pn_right: body
 * ForIn    ternary     pn_kid1: declaration or expression to left of 'in'
 *                          pn_kid2: null
 *                          pn_kid3: object expr to right of 'in'
 * ForOf    ternary     pn_kid1: declaration or expression to left of 'of'
 *                          pn_kid2: null
 *                          pn_kid3: expr to right of 'of'
 * ForHead  ternary     pn_kid1:  init expr before first ';' or nullptr
 *                          pn_kid2:  cond expr before second ';' or nullptr
 *                          pn_kid3:  update expr after second ';' or nullptr
 * Throw    unary       pn_kid: exception
 * Try      ternary     pn_kid1: try block
 *                          pn_kid2: null or LexicalScope for catch-block
 *                                   with pn_expr pointing to a Catch node
 *                          pn_kid3: null or finally block
 * Catch    binary      pn_left: Name, Array, or Object catch
 *                                   var node
 *                                   (Array or Object if destructuring),
 *                                   or null if optional catch binding
 *                          pn_right: catch block statements
 * Break    name        pn_atom: label or null
 * Continue name        pn_atom: label or null
 * With     binary      pn_left: head expr; pn_right: body;
 * Var,     list        pn_head: list of Name or Assign nodes
 * Let,                          each name node has either
 * Const                           pn_used: false
 *                                     pn_atom: variable name
 *                                     pn_expr: initializer or null
 *                                   or
 *                                     pn_used: true
 *                                     pn_atom: variable name
 *                                     pn_lexdef: def node
 *                                   each assignment node has
 *                                     pn_left: Name with pn_used true and
 *                                              pn_lexdef (NOT pn_expr) set
 *                                     pn_right: initializer
 * Return   unary       pn_kid: return expr or null
 * ExpressionStatement unary    pn_kid: expr
 *                              pn_prologue: true if Directive Prologue member
 *                                  in original source, not introduced via
 *                                  constant folding or other tree rewriting
 * EmptyStatement nullary      (no fields)
 * Label    name        pn_atom: label, pn_expr: labeled statement
 * Import   binary      pn_left: ImportSpecList import specifiers
 *                          pn_right: String module specifier
 * Export   unary       pn_kid: declaration expression
 * ExportFrom binary   pn_left: ExportSpecList export specifiers
 *                          pn_right: String module specifier
 * ExportDefault unary pn_kid: export default declaration or expression
 *
 * <Expressions>
 * All left-associated binary trees of the same type are optimized into lists
 * to avoid recursion when processing expression chains.
 * Comma    list        pn_head: list of pn_count comma-separated exprs
 * Assign   binary      pn_left: lvalue, pn_right: rvalue
 * AddAssign,   binary  pn_left: lvalue, pn_right: rvalue
 * SubAssign,           pn_op: JSOP_ADD for +=, etc.
 * BitOrAssign,
 * BitXorAssign,
 * BitAndAssign,
 * LshAssign,
 * RshAssign,
 * UrshAssign,
 * MulAssign,
 * DivAssign,
 * ModAssign,
 * PowAssign
 * Conditional ternary  (cond ? trueExpr : falseExpr)
 *                          pn_kid1: cond, pn_kid2: then, pn_kid3: else
 * Or,      list        pn_head; list of pn_count subexpressions
 * And,                 All of these operators are left-associative except (**).
 * BitOr,
 * BitXor,
 * BitAnd,
 * Eq,
 * Ne,
 * StrictEq,
 * StrictNe,
 * Lt,
 * Le,
 * Gt,
 * Ge,
 * Lsh,
 * Rsh,
 * Ursh,
 * Add,
 * Sub,
 * Star,
 * Div,
 * Mod,
 * Pow                  (**) is right-associative, but still forms a list.
 *                          See comments in ParseNode::appendOrCreateList.
 *
 * Pos,     unary       pn_kid: UNARY expr
 * Neg
 * Void,    unary       pn_kid: UNARY expr
 * Not,
 * BitNot
 * TypeOfName, unary    pn_kid: UNARY expr
 * TypeOfExpr
 * PreIncrement, unary  pn_kid: MEMBER expr
 * PostIncrement,
 * PreDecrement,
 * PostDecrement
 * New      list        pn_head: list of ctor, arg1, arg2, ... argN
 *                          pn_count: 1 + N (where N is number of args)
 *                          ctor is a MEMBER expr
 * DeleteName unary     pn_kid: Name expr
 * DeleteProp unary     pn_kid: Dot expr
 * DeleteElem unary     pn_kid: Elem expr
 * DeleteExpr unary     pn_kid: MEMBER expr that's evaluated, then the
 *                          overall delete evaluates to true; can't be a kind
 *                          for a more-specific PNK_DELETE* unless constant
 *                          folding (or a similar parse tree manipulation) has
 *                          occurred
 * Dot      name        pn_expr: MEMBER expr to left of .
 *                          pn_atom: name to right of .
 * Elem     binary      pn_left: MEMBER expr to left of [
 *                          pn_right: expr between [ and ]
 * Call     list        pn_head: list of call, arg1, arg2, ... argN
 *                          pn_count: 1 + N (where N is number of args)
 *                          call is a MEMBER expr naming a callable object
 * Array    list        pn_head: list of pn_count array element exprs
 *                          [,,] holes are represented by Elision nodes
 *                          pn_xflags: PN_ENDCOMMA if extra comma at end
 * Object   list        pn_head: list of pn_count binary Colon nodes
 * Colon    binary      key-value pair in object initializer or
 *                          destructuring lhs
 *                          pn_left: property id, pn_right: value
 * Shorthand binary     Same fields as Colon. This is used for object
 *                          literal properties using shorthand ({x}).
 * ComputedName unary  ES6 ComputedPropertyName.
 *                          pn_kid: the AssignmentExpression inside the square brackets
 * Name,    name        pn_atom: name, string, or object atom
 * String               pn_op: JSOP_GETNAME, JSOP_STRING, or JSOP_OBJECT
 *                          If JSOP_GETNAME, pn_op may be JSOP_*ARG or JSOP_*VAR
 *                          telling const-ness and static analysis results
 * TemplateStringList pn_head: list of alternating expr and template strings
 *              list
 * TemplateString      pn_atom: template string atom
                nullary     pn_op: JSOP_NOP
 * TaggedTemplate      pn_head: list of call, call site object, arg1, arg2, ... argN
 *              list        pn_count: 2 + N (N is the number of substitutions)
 * CallSiteObj list     pn_head: a Array node followed by
 *                          list of pn_count - 1 TemplateString nodes
 * RegExp   nullary     pn_objbox: RegExp model object
 * Number   dval        pn_dval: double value of numeric literal
 * True,    nullary     pn_op: JSOp bytecode
 * False,
 * Null,
 * RawUndefined
 *
 * This,        unary   pn_kid: '.this' Name if function `this`, else nullptr
 * SuperBase    unary   pn_kid: '.this' Name
 *
 * SetThis      binary  pn_left: '.this' Name, pn_right: SuperCall
 *
 * LexicalScope scope   pn_u.scope.bindings: scope bindings
 *                          pn_u.scope.body: scope body
 * Generator    nullary
 * InitialYield unary   pn_kid: generator object
 * Yield,       unary   pn_kid: expr or null
 * YieldStar,
 * Await
 * Nop          nullary
 */
enum ParseNodeArity
{
    PN_NULLARY,                         /* 0 kids, only pn_atom/pn_dval/etc. */
    PN_UNARY,                           /* one kid, plus a couple of scalars */
    PN_BINARY,                          /* two kids, plus a couple of scalars */
    PN_TERNARY,                         /* three kids */
    PN_CODE,                            /* module or function definition node */
    PN_LIST,                            /* generic singly linked list */
    PN_NAME,                            /* name, label, or regexp */
    PN_SCOPE                            /* lexical scope */
};

class LoopControlStatement;
class BreakStatement;
class ContinueStatement;
class ConditionalExpression;
class PropertyAccess;

class ParseNode
{
    ParseNodeKind pn_type;   /* ParseNodeKind::PNK_* type */
    // pn_op and pn_arity are not declared as the correct enum types
    // due to difficulties with MS bitfield layout rules and a GCC
    // bug.  See https://bugzilla.mozilla.org/show_bug.cgi?id=1383157#c4 for
    // details.
    uint8_t pn_op;      /* see JSOp enum and jsopcode.tbl */
    uint8_t pn_arity:4; /* see ParseNodeArity enum */
    bool pn_parens:1;   /* this expr was enclosed in parens */
    bool pn_rhs_anon_fun:1;  /* this expr is anonymous function or class that
                              * is a direct RHS of ParseNodeKind::Assign or ParseNodeKind::Colon of
                              * property, that needs SetFunctionName. */

    ParseNode(const ParseNode& other) = delete;
    void operator=(const ParseNode& other) = delete;

  public:
    ParseNode(ParseNodeKind kind, JSOp op, ParseNodeArity arity)
      : pn_type(kind),
        pn_op(op),
        pn_arity(arity),
        pn_parens(false),
        pn_rhs_anon_fun(false),
        pn_pos(0, 0),
        pn_next(nullptr)
    {
        MOZ_ASSERT(kind < ParseNodeKind::Limit);
        memset(&pn_u, 0, sizeof pn_u);
    }

    ParseNode(ParseNodeKind kind, JSOp op, ParseNodeArity arity, const TokenPos& pos)
      : pn_type(kind),
        pn_op(op),
        pn_arity(arity),
        pn_parens(false),
        pn_rhs_anon_fun(false),
        pn_pos(pos),
        pn_next(nullptr)
    {
        MOZ_ASSERT(kind < ParseNodeKind::Limit);
        memset(&pn_u, 0, sizeof pn_u);
    }

    JSOp getOp() const                     { return JSOp(pn_op); }
    void setOp(JSOp op)                    { pn_op = op; }
    bool isOp(JSOp op) const               { return getOp() == op; }

    ParseNodeKind getKind() const {
        MOZ_ASSERT(pn_type < ParseNodeKind::Limit);
        return pn_type;
    }
    void setKind(ParseNodeKind kind) {
        MOZ_ASSERT(kind < ParseNodeKind::Limit);
        pn_type = kind;
    }
    bool isKind(ParseNodeKind kind) const  { return getKind() == kind; }

    ParseNodeArity getArity() const        { return ParseNodeArity(pn_arity); }
    bool isArity(ParseNodeArity a) const   { return getArity() == a; }
    void setArity(ParseNodeArity a)        { pn_arity = a; }

    bool isAssignment() const {
        ParseNodeKind kind = getKind();
        return ParseNodeKind::AssignmentStart <= kind && kind <= ParseNodeKind::AssignmentLast;
    }

    bool isBinaryOperation() const {
        ParseNodeKind kind = getKind();
        return ParseNodeKind::BinOpFirst <= kind && kind <= ParseNodeKind::BinOpLast;
    }

    /* Boolean attributes. */
    bool isInParens() const                { return pn_parens; }
    bool isLikelyIIFE() const              { return isInParens(); }
    void setInParens(bool enabled)         { pn_parens = enabled; }

    bool isDirectRHSAnonFunction() const {
        return pn_rhs_anon_fun;
    }
    void setDirectRHSAnonFunction(bool enabled) {
        pn_rhs_anon_fun = enabled;
    }

    TokenPos            pn_pos;         /* two 16-bit pairs here, for 64 bits */
    ParseNode*          pn_next;        /* intrinsic link in parent PN_LIST */

    union {
        struct {                        /* list of next-linked nodes */
            ParseNode*  head;           /* first node in list */
            ParseNode** tail;           /* ptr to ptr to last node in list */
            uint32_t    count;          /* number of nodes in list */
            uint32_t    xflags;         /* see PNX_* below */
        } list;
        struct {                        /* ternary: if, for(;;), ?: */
            ParseNode*  kid1;           /* condition, discriminant, etc. */
            ParseNode*  kid2;           /* then-part, case list, etc. */
            ParseNode*  kid3;           /* else-part, default case, etc. */
        } ternary;
        struct {                        /* two kids if binary */
            ParseNode*  left;
            ParseNode*  right;
            union {
                unsigned iflags;        /* JSITER_* flags for ParseNodeKind::For node */
                bool isStatic;          /* only for ParseNodeKind::ClassMethod */
                uint32_t offset;        /* for the emitter's use on ParseNodeKind::Case nodes */
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
                ObjectBox*   objbox;    /* regexp object */
                FunctionBox* funbox;    /* function object */
            };
            ParseNode*  expr;           /* module or function body, var
                                           initializer, argument default, or
                                           base object of ParseNodeKind::Dot */
        } name;
        struct {
            LexicalScope::Data* bindings;
            ParseNode*          body;
        } scope;
        struct {
            double       value;         /* aligned numeric literal value */
            DecimalPoint decimalPoint;  /* Whether the number has a decimal point */
        } number;
        class {
            friend class LoopControlStatement;
            PropertyName*    label;    /* target of break/continue statement */
        } loopControl;
    } pn_u;

#define pn_objbox       pn_u.name.objbox
#define pn_funbox       pn_u.name.funbox
#define pn_body         pn_u.name.expr
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
#define pn_kid          pn_u.unary.kid
#define pn_prologue     pn_u.unary.prologue
#define pn_atom         pn_u.name.atom
#define pn_objbox       pn_u.name.objbox
#define pn_expr         pn_u.name.expr
#define pn_dval         pn_u.number.value


  public:
    /*
     * If |left| is a list of the given kind/left-associative op, append
     * |right| to it and return |left|.  Otherwise return a [left, right] list.
     */
    static ParseNode*
    appendOrCreateList(ParseNodeKind kind, ParseNode* left, ParseNode* right,
                       FullParseHandler* handler, ParseContext* pc);

    // include "ParseNode-inl.h" for these methods.
    inline PropertyName* name() const;
    inline JSAtom* atom() const;

    ParseNode* expr() const {
        MOZ_ASSERT(pn_arity == PN_NAME || pn_arity == PN_CODE);
        return pn_expr;
    }

    bool isEmptyScope() const {
        MOZ_ASSERT(pn_arity == PN_SCOPE);
        return !pn_u.scope.bindings;
    }

    Handle<LexicalScope::Data*> scopeBindings() const {
        MOZ_ASSERT(!isEmptyScope());
        // Bindings' GC safety depend on the presence of an AutoKeepAtoms that
        // the rest of the frontend also depends on.
        return Handle<LexicalScope::Data*>::fromMarkedLocation(&pn_u.scope.bindings);
    }

    ParseNode* scopeBody() const {
        MOZ_ASSERT(pn_arity == PN_SCOPE);
        return pn_u.scope.body;
    }

    void setScopeBody(ParseNode* body) {
        MOZ_ASSERT(pn_arity == PN_SCOPE);
        pn_u.scope.body = body;
    }

/* PN_LIST pn_xflags bits. */
#define PNX_FUNCDEFS    0x01            /* contains top-level function statements */
#define PNX_ARRAYHOLESPREAD 0x02        /* one or more of
                                           1. array initialiser has holes
                                           2. array initializer has spread node */
#define PNX_NONCONST    0x04            /* initialiser has non-constants */

    bool functionIsHoisted() const {
        MOZ_ASSERT(pn_arity == PN_CODE && getKind() == ParseNodeKind::Function);
        MOZ_ASSERT(isOp(JSOP_LAMBDA) ||        // lambda
                   isOp(JSOP_LAMBDA_ARROW) ||  // arrow function
                   isOp(JSOP_DEFFUN) ||        // non-body-level function statement
                   isOp(JSOP_NOP) ||           // body-level function stmt in global code
                   isOp(JSOP_GETLOCAL) ||      // body-level function stmt in function code
                   isOp(JSOP_GETARG) ||        // body-level function redeclaring formal
                   isOp(JSOP_INITLEXICAL));    // block-level function stmt
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
        if (getKind() == ParseNodeKind::ExpressionStatement) {
            MOZ_ASSERT(pn_arity == PN_UNARY);
            ParseNode* kid = pn_kid;
            if (kid->getKind() == ParseNodeKind::String && !kid->pn_parens)
                return kid->pn_atom;
        }
        return nullptr;
    }

    /* True if pn is a parsenode representing a literal constant. */
    bool isLiteral() const {
        return isKind(ParseNodeKind::Number) ||
               isKind(ParseNodeKind::String) ||
               isKind(ParseNodeKind::True) ||
               isKind(ParseNodeKind::False) ||
               isKind(ParseNodeKind::Null) ||
               isKind(ParseNodeKind::RawUndefined);
    }

    /* Return true if this node appears in a Directive Prologue. */
    bool isDirectivePrologueMember() const { return pn_prologue; }

    // True iff this is a for-in/of loop variable declaration (var/let/const).
    bool isForLoopDeclaration() const {
        if (isKind(ParseNodeKind::Var) || isKind(ParseNodeKind::Let) || isKind(ParseNodeKind::Const)) {
            MOZ_ASSERT(isArity(PN_LIST));
            MOZ_ASSERT(pn_count > 0);
            return true;
        }

        return false;
    }

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
        MOZ_ASSERT(getKind() == ParseNodeKind::Number);
        pn_u.number.value = value;
        pn_u.number.decimalPoint = decimalPoint;
    }

    void makeEmpty() {
        MOZ_ASSERT(pn_arity == PN_LIST);
        pn_head = nullptr;
        pn_tail = &pn_head;
        pn_count = 0;
        pn_xflags = 0;
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
    }

    void append(ParseNode* pn) {
        MOZ_ASSERT(pn->pn_pos.begin >= pn_pos.begin);
        appendWithoutOrderAssumption(pn);
    }

    void appendWithoutOrderAssumption(ParseNode* pn) {
        MOZ_ASSERT(pn_arity == PN_LIST);
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

    MOZ_MUST_USE bool getConstantValue(JSContext* cx, AllowConstantObjects allowObjects,
                                       MutableHandleValue vp, Value* compare = nullptr,
                                       size_t ncompare = 0, NewObjectKind newKind = TenuredObject);
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
    void dump(GenericPrinter& out);
    void dump(GenericPrinter& out, int indent);
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
    void dump(GenericPrinter& out);
#endif
};

struct UnaryNode : public ParseNode
{
    UnaryNode(ParseNodeKind kind, const TokenPos& pos, ParseNode* kid)
      : ParseNode(kind, JSOP_NOP, PN_UNARY, pos)
    {
        pn_kid = kid;
    }

#ifdef DEBUG
    void dump(GenericPrinter& out, int indent);
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
    void dump(GenericPrinter& out, int indent);
#endif
};

struct TernaryNode : public ParseNode
{
    TernaryNode(ParseNodeKind kind, ParseNode* kid1, ParseNode* kid2, ParseNode* kid3)
      : ParseNode(kind, JSOP_NOP, PN_TERNARY,
                  TokenPos((kid1 ? kid1 : kid2 ? kid2 : kid3)->pn_pos.begin,
                           (kid3 ? kid3 : kid2 ? kid2 : kid1)->pn_pos.end))
    {
        pn_kid1 = kid1;
        pn_kid2 = kid2;
        pn_kid3 = kid3;
    }

    TernaryNode(ParseNodeKind kind, ParseNode* kid1, ParseNode* kid2, ParseNode* kid3,
                const TokenPos& pos)
      : ParseNode(kind, JSOP_NOP, PN_TERNARY, pos)
    {
        pn_kid1 = kid1;
        pn_kid2 = kid2;
        pn_kid3 = kid3;
    }

#ifdef DEBUG
    void dump(GenericPrinter& out, int indent);
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
    void dump(GenericPrinter& out, int indent);
#endif
};

struct CodeNode : public ParseNode
{
    CodeNode(ParseNodeKind kind, JSOp op, const TokenPos& pos)
      : ParseNode(kind, op, PN_CODE, pos)
    {
        MOZ_ASSERT(kind == ParseNodeKind::Function || kind == ParseNodeKind::Module);
        MOZ_ASSERT_IF(kind == ParseNodeKind::Module, op == JSOP_NOP);
        MOZ_ASSERT(op == JSOP_NOP || // statement, module
                   op == JSOP_LAMBDA_ARROW || // arrow function
                   op == JSOP_LAMBDA); // expression, method, accessor, &c.
        MOZ_ASSERT(!pn_body);
        MOZ_ASSERT(!pn_objbox);
    }

  public:
#ifdef DEBUG
  void dump(GenericPrinter& out, int indent);
#endif
};

struct NameNode : public ParseNode
{
    NameNode(ParseNodeKind kind, JSOp op, JSAtom* atom, const TokenPos& pos)
      : ParseNode(kind, op, PN_NAME, pos)
    {
        pn_atom = atom;
        pn_expr = nullptr;
    }

#ifdef DEBUG
    void dump(GenericPrinter& out, int indent);
#endif
};

struct LexicalScopeNode : public ParseNode
{
    LexicalScopeNode(LexicalScope::Data* bindings, ParseNode* body)
      : ParseNode(ParseNodeKind::LexicalScope, JSOP_NOP, PN_SCOPE, body->pn_pos)
    {
        pn_u.scope.bindings = bindings;
        pn_u.scope.body = body;
    }

    static bool test(const ParseNode& node) {
        return node.isKind(ParseNodeKind::LexicalScope);
    }

#ifdef DEBUG
    void dump(GenericPrinter& out, int indent);
#endif
};

class LabeledStatement : public ParseNode
{
  public:
    LabeledStatement(PropertyName* label, ParseNode* stmt, uint32_t begin)
      : ParseNode(ParseNodeKind::Label, JSOP_NOP, PN_NAME, TokenPos(begin, stmt->pn_pos.end))
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
        bool match = node.isKind(ParseNodeKind::Label);
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
      : BinaryNode(ParseNodeKind::Case, JSOP_NOP, TokenPos(begin, stmts->pn_pos.end), expr, stmts) {}

    ParseNode* caseExpression() const { return pn_left; }
    bool isDefault() const { return !caseExpression(); }
    ParseNode* statementList() const { return pn_right; }

    // The next CaseClause in the same switch statement.
    CaseClause* next() const { return pn_next ? &pn_next->as<CaseClause>() : nullptr; }

    // Scratch space used by the emitter.
    uint32_t offset() const { return pn_u.binary.offset; }
    void setOffset(uint32_t u) { pn_u.binary.offset = u; }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(ParseNodeKind::Case);
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
        MOZ_ASSERT(kind == ParseNodeKind::Break || kind == ParseNodeKind::Continue);
        pn_u.loopControl.label = label;
    }

  public:
    /* Label associated with this break/continue statement, if any. */
    PropertyName* label() const {
        return pn_u.loopControl.label;
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(ParseNodeKind::Break) || node.isKind(ParseNodeKind::Continue);
        MOZ_ASSERT_IF(match, node.isArity(PN_NULLARY));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_NOP));
        return match;
    }
};

class BreakStatement : public LoopControlStatement
{
  public:
    BreakStatement(PropertyName* label, const TokenPos& pos)
      : LoopControlStatement(ParseNodeKind::Break, label, pos)
    { }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(ParseNodeKind::Break);
        MOZ_ASSERT_IF(match, node.isArity(PN_NULLARY));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_NOP));
        return match;
    }
};

class ContinueStatement : public LoopControlStatement
{
  public:
    ContinueStatement(PropertyName* label, const TokenPos& pos)
      : LoopControlStatement(ParseNodeKind::Continue, label, pos)
    { }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(ParseNodeKind::Continue);
        MOZ_ASSERT_IF(match, node.isArity(PN_NULLARY));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_NOP));
        return match;
    }
};

class DebuggerStatement : public ParseNode
{
  public:
    explicit DebuggerStatement(const TokenPos& pos)
      : ParseNode(ParseNodeKind::Debugger, JSOP_NOP, PN_NULLARY, pos)
    { }
};

class ConditionalExpression : public ParseNode
{
  public:
    ConditionalExpression(ParseNode* condition, ParseNode* thenExpr, ParseNode* elseExpr)
      : ParseNode(ParseNodeKind::Conditional, JSOP_NOP, PN_TERNARY,
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
        bool match = node.isKind(ParseNodeKind::Conditional);
        MOZ_ASSERT_IF(match, node.isArity(PN_TERNARY));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_NOP));
        return match;
    }
};

class ThisLiteral : public UnaryNode
{
  public:
    ThisLiteral(const TokenPos& pos, ParseNode* thisName)
      : UnaryNode(ParseNodeKind::This, pos, thisName)
    { }
};

class NullLiteral : public ParseNode
{
  public:
    explicit NullLiteral(const TokenPos& pos) : ParseNode(ParseNodeKind::Null, JSOP_NULL, PN_NULLARY, pos) { }
};

// This is only used internally, currently just for tagged templates.
// It represents the value 'undefined' (aka `void 0`), like NullLiteral
// represents the value 'null'.
class RawUndefinedLiteral : public ParseNode
{
  public:
    explicit RawUndefinedLiteral(const TokenPos& pos)
      : ParseNode(ParseNodeKind::RawUndefined, JSOP_UNDEFINED, PN_NULLARY, pos) { }
};

class BooleanLiteral : public ParseNode
{
  public:
    BooleanLiteral(bool b, const TokenPos& pos)
      : ParseNode(b ? ParseNodeKind::True : ParseNodeKind::False, b ? JSOP_TRUE : JSOP_FALSE, PN_NULLARY, pos)
    { }
};

class RegExpLiteral : public NullaryNode
{
  public:
    RegExpLiteral(ObjectBox* reobj, const TokenPos& pos)
      : NullaryNode(ParseNodeKind::RegExp, JSOP_REGEXP, pos)
    {
        pn_objbox = reobj;
    }

    ObjectBox* objbox() const { return pn_objbox; }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(ParseNodeKind::RegExp);
        MOZ_ASSERT_IF(match, node.isArity(PN_NULLARY));
        MOZ_ASSERT_IF(match, node.isOp(JSOP_REGEXP));
        return match;
    }
};

class PropertyAccess : public ParseNode
{
  public:
    PropertyAccess(ParseNode* lhs, PropertyName* name, uint32_t begin, uint32_t end)
      : ParseNode(ParseNodeKind::Dot, JSOP_NOP, PN_NAME, TokenPos(begin, end))
    {
        MOZ_ASSERT(lhs != nullptr);
        MOZ_ASSERT(name != nullptr);
        pn_u.name.expr = lhs;
        pn_u.name.atom = name;
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(ParseNodeKind::Dot);
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
        // ParseNodeKind::SuperBase cannot result from any expression syntax.
        return expression().isKind(ParseNodeKind::SuperBase);
    }
};

class PropertyByValue : public ParseNode
{
  public:
    PropertyByValue(ParseNode* lhs, ParseNode* propExpr, uint32_t begin, uint32_t end)
      : ParseNode(ParseNodeKind::Elem, JSOP_NOP, PN_BINARY, TokenPos(begin, end))
    {
        pn_u.binary.left = lhs;
        pn_u.binary.right = propExpr;
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(ParseNodeKind::Elem);
        MOZ_ASSERT_IF(match, node.isArity(PN_BINARY));
        return match;
    }

    bool isSuper() const {
        return pn_left->isKind(ParseNodeKind::SuperBase);
    }
};

/*
 * A CallSiteNode represents the implicit call site object argument in a TaggedTemplate.
 */
struct CallSiteNode : public ListNode {
    explicit CallSiteNode(uint32_t begin): ListNode(ParseNodeKind::CallSiteObj, TokenPos(begin, begin + 1)) {}

    static bool test(const ParseNode& node) {
        return node.isKind(ParseNodeKind::CallSiteObj);
    }

    MOZ_MUST_USE bool getRawArrayValue(JSContext* cx, MutableHandleValue vp) {
        return pn_head->getConstantValue(cx, AllowObjects, vp);
    }
};

struct ClassMethod : public BinaryNode {
    /*
     * Method defintions often keep a name and function body that overlap,
     * so explicitly define the beginning and end here.
     */
    ClassMethod(ParseNode* name, ParseNode* body, JSOp op, bool isStatic)
      : BinaryNode(ParseNodeKind::ClassMethod, op, TokenPos(name->pn_pos.begin, body->pn_pos.end), name, body)
    {
        pn_u.binary.isStatic = isStatic;
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(ParseNodeKind::ClassMethod);
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
      : BinaryNode(ParseNodeKind::ClassNames, JSOP_NOP, pos, outerBinding, innerBinding)
    {
        MOZ_ASSERT_IF(outerBinding, outerBinding->isKind(ParseNodeKind::Name));
        MOZ_ASSERT(innerBinding->isKind(ParseNodeKind::Name));
        MOZ_ASSERT_IF(outerBinding, innerBinding->pn_atom == outerBinding->pn_atom);
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(ParseNodeKind::ClassNames);
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
    ClassNode(ParseNode* names, ParseNode* heritage, ParseNode* methodsOrBlock,
              const TokenPos& pos)
      : TernaryNode(ParseNodeKind::Class, names, heritage, methodsOrBlock, pos)
    {
        MOZ_ASSERT_IF(names, names->is<ClassNames>());
        MOZ_ASSERT(methodsOrBlock->is<LexicalScopeNode>() ||
                   methodsOrBlock->isKind(ParseNodeKind::ClassMethodList));
    }

    static bool test(const ParseNode& node) {
        bool match = node.isKind(ParseNodeKind::Class);
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
        if (pn_kid3->isKind(ParseNodeKind::ClassMethodList))
            return pn_kid3;

        MOZ_ASSERT(pn_kid3->is<LexicalScopeNode>());
        ParseNode* list = pn_kid3->scopeBody();
        MOZ_ASSERT(list->isKind(ParseNodeKind::ClassMethodList));
        return list;
    }
    Handle<LexicalScope::Data*> scopeBindings() const {
        MOZ_ASSERT(pn_kid3->is<LexicalScopeNode>());
        return pn_kid3->scopeBindings();
    }
};

#ifdef DEBUG
void DumpParseTree(ParseNode* pn, GenericPrinter& out, int indent = 0);
#endif

class ParseNodeAllocator
{
  public:
    explicit ParseNodeAllocator(JSContext* cx, LifoAlloc& alloc)
      : cx(cx), alloc(alloc), freelist(nullptr)
    {}

    void* allocNode();
    void freeNode(ParseNode* pn);
    ParseNode* freeTree(ParseNode* pn);
    void prepareNodeForMutation(ParseNode* pn);

  private:
    JSContext* cx;
    LifoAlloc& alloc;
    ParseNode* freelist;
};

inline bool
ParseNode::isConstant()
{
    switch (pn_type) {
      case ParseNodeKind::Number:
      case ParseNodeKind::String:
      case ParseNodeKind::TemplateString:
      case ParseNodeKind::Null:
      case ParseNodeKind::RawUndefined:
      case ParseNodeKind::False:
      case ParseNodeKind::True:
        return true;
      case ParseNodeKind::Array:
      case ParseNodeKind::Object:
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
    virtual void trace(JSTracer* trc);

    static void TraceList(JSTracer* trc, ObjectBox* listHead);

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

enum class AccessorType {
    None,
    Getter,
    Setter
};

inline JSOp
AccessorTypeToJSOp(AccessorType atype)
{
    switch (atype) {
      case AccessorType::None:
        return JSOP_INITPROP;
      case AccessorType::Getter:
        return JSOP_INITPROP_GETTER;
      case AccessorType::Setter:
        return JSOP_INITPROP_SETTER;
      default:
        MOZ_CRASH("unexpected accessor type");
    }
}

enum FunctionSyntaxKind
{
    // A non-arrow function expression that is a PrimaryExpression and *also* a
    // complete AssignmentExpression.  For example, in
    //
    //   var x = (function y() {});
    //
    // |y| is such a function expression.
    AssignmentExpression,

    // A non-arrow function expression that is a PrimaryExpression but *not* a
    // complete AssignmentExpression.  For example, in
    //
    //   var x = (1 + function y() {});
    //
    // |y| is such a function expression.
    PrimaryExpression,

    // A named function appearing as a Statement.
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
IsFunctionExpression(FunctionSyntaxKind kind)
{
    return kind == AssignmentExpression || kind == PrimaryExpression;
}

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

static inline bool
IsMethodDefinitionKind(FunctionSyntaxKind kind)
{
    return kind == Method || IsConstructorKind(kind) ||
           IsGetterKind(kind) || IsSetterKind(kind);
}

static inline ParseNode*
FunctionFormalParametersList(ParseNode* fn, unsigned* numFormals)
{
    MOZ_ASSERT(fn->isKind(ParseNodeKind::Function));
    ParseNode* argsBody = fn->pn_body;
    MOZ_ASSERT(argsBody->isKind(ParseNodeKind::ParamsBody));
    *numFormals = argsBody->pn_count;
    if (*numFormals > 0 &&
        argsBody->last()->isKind(ParseNodeKind::LexicalScope) &&
        argsBody->last()->scopeBody()->isKind(ParseNodeKind::StatementList))
    {
        (*numFormals)--;
    }
    MOZ_ASSERT(argsBody->isArity(PN_LIST));
    return argsBody->pn_head;
}

bool
IsAnonymousFunctionDefinition(ParseNode* pn);

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_ParseNode_h */
