/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS reflection package. */

#include "mozilla/ArrayUtils.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Move.h"

#include <stdlib.h>

#include "jsarray.h"
#include "jsatom.h"
#include "jsobj.h"
#include "jspubtd.h"

#include "builtin/Reflect.h"
#include "frontend/Parser.h"
#include "frontend/TokenStream.h"
#include "js/CharacterEncoding.h"
#include "vm/RegExpObject.h"

#include "jsobjinlines.h"

#include "frontend/ParseNode-inl.h"

using namespace js;
using namespace js::frontend;

using JS::AutoValueArray;
using mozilla::ArrayLength;
using mozilla::DebugOnly;
using mozilla::Forward;

enum class ParseTarget
{
    Script,
    Module
};

enum ASTType {
    AST_ERROR = -1,
#define ASTDEF(ast, str, method) ast,
#include "jsast.tbl"
#undef ASTDEF
    AST_LIMIT
};

enum AssignmentOperator {
    AOP_ERR = -1,

    /* assign */
    AOP_ASSIGN = 0,
    /* operator-assign */
    AOP_PLUS, AOP_MINUS, AOP_STAR, AOP_DIV, AOP_MOD, AOP_POW,
    /* shift-assign */
    AOP_LSH, AOP_RSH, AOP_URSH,
    /* binary */
    AOP_BITOR, AOP_BITXOR, AOP_BITAND,

    AOP_LIMIT
};

enum BinaryOperator {
    BINOP_ERR = -1,

    /* eq */
    BINOP_EQ = 0, BINOP_NE, BINOP_STRICTEQ, BINOP_STRICTNE,
    /* rel */
    BINOP_LT, BINOP_LE, BINOP_GT, BINOP_GE,
    /* shift */
    BINOP_LSH, BINOP_RSH, BINOP_URSH,
    /* arithmetic */
    BINOP_ADD, BINOP_SUB, BINOP_STAR, BINOP_DIV, BINOP_MOD, BINOP_POW,
    /* binary */
    BINOP_BITOR, BINOP_BITXOR, BINOP_BITAND,
    /* misc */
    BINOP_IN, BINOP_INSTANCEOF,

    BINOP_LIMIT
};

enum UnaryOperator {
    UNOP_ERR = -1,

    UNOP_DELETE = 0,
    UNOP_NEG,
    UNOP_POS,
    UNOP_NOT,
    UNOP_BITNOT,
    UNOP_TYPEOF,
    UNOP_VOID,

    UNOP_LIMIT
};

enum VarDeclKind {
    VARDECL_ERR = -1,
    VARDECL_VAR = 0,
    VARDECL_CONST,
    VARDECL_LET,
    VARDECL_LIMIT
};

enum PropKind {
    PROP_ERR = -1,
    PROP_INIT = 0,
    PROP_GETTER,
    PROP_SETTER,
    PROP_MUTATEPROTO,
    PROP_LIMIT
};

static const char* const aopNames[] = {
    "=",    /* AOP_ASSIGN */
    "+=",   /* AOP_PLUS */
    "-=",   /* AOP_MINUS */
    "*=",   /* AOP_STAR */
    "/=",   /* AOP_DIV */
    "%=",   /* AOP_MOD */
    "**=",  /* AOP_POW */
    "<<=",  /* AOP_LSH */
    ">>=",  /* AOP_RSH */
    ">>>=", /* AOP_URSH */
    "|=",   /* AOP_BITOR */
    "^=",   /* AOP_BITXOR */
    "&="    /* AOP_BITAND */
};

static const char* const binopNames[] = {
    "==",         /* BINOP_EQ */
    "!=",         /* BINOP_NE */
    "===",        /* BINOP_STRICTEQ */
    "!==",        /* BINOP_STRICTNE */
    "<",          /* BINOP_LT */
    "<=",         /* BINOP_LE */
    ">",          /* BINOP_GT */
    ">=",         /* BINOP_GE */
    "<<",         /* BINOP_LSH */
    ">>",         /* BINOP_RSH */
    ">>>",        /* BINOP_URSH */
    "+",          /* BINOP_PLUS */
    "-",          /* BINOP_MINUS */
    "*",          /* BINOP_STAR */
    "/",          /* BINOP_DIV */
    "%",          /* BINOP_MOD */
    "**",         /* BINOP_POW */
    "|",          /* BINOP_BITOR */
    "^",          /* BINOP_BITXOR */
    "&",          /* BINOP_BITAND */
    "in",         /* BINOP_IN */
    "instanceof", /* BINOP_INSTANCEOF */
};

static const char* const unopNames[] = {
    "delete",  /* UNOP_DELETE */
    "-",       /* UNOP_NEG */
    "+",       /* UNOP_POS */
    "!",       /* UNOP_NOT */
    "~",       /* UNOP_BITNOT */
    "typeof",  /* UNOP_TYPEOF */
    "void"     /* UNOP_VOID */
};

static const char* const nodeTypeNames[] = {
#define ASTDEF(ast, str, method) str,
#include "jsast.tbl"
#undef ASTDEF
    nullptr
};

static const char* const callbackNames[] = {
#define ASTDEF(ast, str, method) method,
#include "jsast.tbl"
#undef ASTDEF
    nullptr
};

enum YieldKind { Delegating, NotDelegating };

typedef AutoValueVector NodeVector;

/*
 * ParseNode is a somewhat intricate data structure, and its invariants have
 * evolved, making it more likely that there could be a disconnect between the
 * parser and the AST serializer. We use these macros to check invariants on a
 * parse node and raise a dynamic error on failure.
 */
#define LOCAL_ASSERT(expr)                                                                \
    JS_BEGIN_MACRO                                                                        \
        MOZ_ASSERT(expr);                                                                 \
        if (!(expr)) {                                                                    \
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_PARSE_NODE);     \
            return false;                                                                 \
        }                                                                                 \
    JS_END_MACRO

#define LOCAL_NOT_REACHED(expr)                                                           \
    JS_BEGIN_MACRO                                                                        \
        MOZ_ASSERT(false);                                                                \
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_PARSE_NODE);         \
        return false;                                                                     \
    JS_END_MACRO

namespace {

/* Set 'result' to obj[id] if any such property exists, else defaultValue. */
static bool
GetPropertyDefault(JSContext* cx, HandleObject obj, HandleId id, HandleValue defaultValue,
                   MutableHandleValue result)
{
    bool found;
    if (!HasProperty(cx, obj, id, &found))
        return false;
    if (!found) {
        result.set(defaultValue);
        return true;
    }
    return GetProperty(cx, obj, obj, id, result);
}

enum class GeneratorStyle
{
    None,
    Legacy,
    ES6
};

/*
 * Builder class that constructs JavaScript AST node objects. See:
 *
 *     https://developer.mozilla.org/en/SpiderMonkey/Parser_API
 *
 * Bug 569487: generalize builder interface
 */
class NodeBuilder
{
    typedef AutoValueArray<AST_LIMIT> CallbackArray;

    JSContext*  cx;
    TokenStream* tokenStream;
    bool        saveLoc;               /* save source location information?     */
    char const* src;                  /* source filename or null               */
    RootedValue srcval;                /* source filename JS value or null      */
    CallbackArray callbacks;           /* user-specified callbacks              */
    RootedValue userv;                 /* user-specified builder object or null */

  public:
    NodeBuilder(JSContext* c, bool l, char const* s)
      : cx(c), tokenStream(nullptr), saveLoc(l), src(s), srcval(c), callbacks(cx),
          userv(c)
    {}

    bool init(HandleObject userobj = nullptr) {
        if (src) {
            if (!atomValue(src, &srcval))
                return false;
        } else {
            srcval.setNull();
        }

        if (!userobj) {
            userv.setNull();
            for (unsigned i = 0; i < AST_LIMIT; i++) {
                callbacks[i].setNull();
            }
            return true;
        }

        userv.setObject(*userobj);

        RootedValue nullVal(cx, NullValue());
        RootedValue funv(cx);
        for (unsigned i = 0; i < AST_LIMIT; i++) {
            const char* name = callbackNames[i];
            RootedAtom atom(cx, Atomize(cx, name, strlen(name)));
            if (!atom)
                return false;
            RootedId id(cx, AtomToId(atom));
            if (!GetPropertyDefault(cx, userobj, id, nullVal, &funv))
                return false;

            if (funv.isNullOrUndefined()) {
                callbacks[i].setNull();
                continue;
            }

            if (!funv.isObject() || !funv.toObject().is<JSFunction>()) {
                ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_NOT_FUNCTION,
                                      JSDVG_SEARCH_STACK, funv, nullptr, nullptr, nullptr);
                return false;
            }

            callbacks[i].set(funv);
        }

        return true;
    }

    void setTokenStream(TokenStream* ts) {
        tokenStream = ts;
    }

  private:
    template <size_t N>
    bool callbackHelper(HandleValue fun, AutoValueArray<N>& args, size_t i,
                        TokenPos* pos, MutableHandleValue dst)
    {
        // The end of the implementation of callback(). All arguments except
        // loc have already been stored in range [0, i) or args.
        MOZ_ASSERT(i == N - 1);
        if (saveLoc) {
            RootedValue loc(cx);
            if (!newNodeLoc(pos, &loc))
                return false;
            args[i++].set(loc);
        }
        return Invoke(cx, userv, fun, N, args.begin(), dst);
    }

    // Helper function for callback(). Note that all Arguments must be types
    // that convert to HandleValue, so this is not really as template-y as it
    // seems, just variadic.
    template <size_t N, typename... Arguments>
    bool callbackHelper(HandleValue fun, AutoValueArray<N>& args, size_t i,
                        HandleValue head, Arguments&&... tail)
    {
        // Recursive loop to store the arguments in the array. This eventually
        // bottoms out in a call to the non-template callbackHelper() above.
        args[i].set(head);
        return callbackHelper(fun, args, i + 1, Forward<Arguments>(tail)...);
    }

    // Invoke a user-defined callback. The actual signature is:
    //
    //     bool callback(HandleValue fun, HandleValue... args, TokenPos* pos,
    //                   MutableHandleValue dst);
    template <typename... Arguments>
    bool callback(HandleValue fun, Arguments&&... args) {
        AutoValueArray<sizeof...(args) - 1> argv(cx);
        return callbackHelper(fun, argv, 0, Forward<Arguments>(args)...);
    }

    // WARNING: Returning a Handle is non-standard, but it works in this case
    // because both |v| and |UndefinedHandleValue| are definitely rooted on a
    // previous stack frame (i.e. we're just choosing between two
    // already-rooted values).
    HandleValue opt(HandleValue v) {
        MOZ_ASSERT_IF(v.isMagic(), v.whyMagic() == JS_SERIALIZE_NO_NODE);
        return v.isMagic(JS_SERIALIZE_NO_NODE) ? JS::UndefinedHandleValue : v;
    }

    bool atomValue(const char* s, MutableHandleValue dst) {
        /*
         * Bug 575416: instead of Atomize, lookup constant atoms in tbl file
         */
        RootedAtom atom(cx, Atomize(cx, s, strlen(s)));
        if (!atom)
            return false;

        dst.setString(atom);
        return true;
    }

    bool newObject(MutableHandleObject dst) {
        RootedPlainObject nobj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!nobj)
            return false;

        dst.set(nobj);
        return true;
    }

    bool newArray(NodeVector& elts, MutableHandleValue dst);

    bool createNode(ASTType type, TokenPos* pos, MutableHandleObject dst);

    bool newNodeHelper(HandleObject obj, MutableHandleValue dst) {
        // The end of the implementation of newNode().
        MOZ_ASSERT(obj);
        dst.setObject(*obj);
        return true;
    }

    template <typename... Arguments>
    bool newNodeHelper(HandleObject obj, const char *name, HandleValue value,
                       Arguments&&... rest)
    {
        // Recursive loop to define properties. Note that the newNodeHelper()
        // call below passes two fewer arguments than we received, as we omit
        // `name` and `value`. This eventually bottoms out in a call to the
        // non-template newNodeHelper() above.
        return defineProperty(obj, name, value)
               && newNodeHelper(obj, Forward<Arguments>(rest)...);
    }

    // Create a node object with "type" and "loc" properties, as well as zero
    // or more properties passed in as arguments. The signature is really more
    // like:
    //
    //     bool newNode(ASTType type, TokenPos* pos,
    //                  {const char *name0, HandleValue value0,}...
    //                  MutableHandleValue dst);
    template <typename... Arguments>
    bool newNode(ASTType type, TokenPos* pos, Arguments&&... args) {
        RootedObject node(cx);
        return createNode(type, pos, &node) &&
               newNodeHelper(node, Forward<Arguments>(args)...);
    }

    bool listNode(ASTType type, const char* propName, NodeVector& elts, TokenPos* pos,
                  MutableHandleValue dst) {
        RootedValue array(cx);
        if (!newArray(elts, &array))
            return false;

        RootedValue cb(cx, callbacks[type]);
        if (!cb.isNull())
            return callback(cb, array, pos, dst);

        return newNode(type, pos, propName, array, dst);
    }

    bool defineProperty(HandleObject obj, const char* name, HandleValue val) {
        MOZ_ASSERT_IF(val.isMagic(), val.whyMagic() == JS_SERIALIZE_NO_NODE);

        /*
         * Bug 575416: instead of Atomize, lookup constant atoms in tbl file
         */
        RootedAtom atom(cx, Atomize(cx, name, strlen(name)));
        if (!atom)
            return false;

        /* Represent "no node" as null and ensure users are not exposed to magic values. */
        RootedValue optVal(cx, val.isMagic(JS_SERIALIZE_NO_NODE) ? NullValue() : val);
        return DefineProperty(cx, obj, atom->asPropertyName(), optVal);
    }

    bool newNodeLoc(TokenPos* pos, MutableHandleValue dst);

    bool setNodeLoc(HandleObject node, TokenPos* pos);

  public:
    /*
     * All of the public builder methods take as their last two
     * arguments a nullable token position and a non-nullable, rooted
     * outparam.
     *
     * Any Value arguments representing optional subnodes may be a
     * JS_SERIALIZE_NO_NODE magic value.
     */

    /*
     * misc nodes
     */

    bool program(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    bool literal(HandleValue val, TokenPos* pos, MutableHandleValue dst);

    bool identifier(HandleValue name, TokenPos* pos, MutableHandleValue dst);

    bool function(ASTType type, TokenPos* pos,
                  HandleValue id, NodeVector& args, NodeVector& defaults,
                  HandleValue body, HandleValue rest, GeneratorStyle generatorStyle,
                  bool isExpression, MutableHandleValue dst);

    bool variableDeclarator(HandleValue id, HandleValue init, TokenPos* pos,
                            MutableHandleValue dst);

    bool switchCase(HandleValue expr, NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    bool catchClause(HandleValue var, HandleValue guard, HandleValue body, TokenPos* pos,
                     MutableHandleValue dst);

    bool prototypeMutation(HandleValue val, TokenPos* pos, MutableHandleValue dst);
    bool propertyInitializer(HandleValue key, HandleValue val, PropKind kind, bool isShorthand,
                             bool isMethod, TokenPos* pos, MutableHandleValue dst);


    /*
     * statements
     */

    bool blockStatement(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    bool expressionStatement(HandleValue expr, TokenPos* pos, MutableHandleValue dst);

    bool emptyStatement(TokenPos* pos, MutableHandleValue dst);

    bool ifStatement(HandleValue test, HandleValue cons, HandleValue alt, TokenPos* pos,
                     MutableHandleValue dst);

    bool breakStatement(HandleValue label, TokenPos* pos, MutableHandleValue dst);

    bool continueStatement(HandleValue label, TokenPos* pos, MutableHandleValue dst);

    bool labeledStatement(HandleValue label, HandleValue stmt, TokenPos* pos,
                          MutableHandleValue dst);

    bool throwStatement(HandleValue arg, TokenPos* pos, MutableHandleValue dst);

    bool returnStatement(HandleValue arg, TokenPos* pos, MutableHandleValue dst);

    bool forStatement(HandleValue init, HandleValue test, HandleValue update, HandleValue stmt,
                      TokenPos* pos, MutableHandleValue dst);

    bool forInStatement(HandleValue var, HandleValue expr, HandleValue stmt,
                        bool isForEach, TokenPos* pos, MutableHandleValue dst);

    bool forOfStatement(HandleValue var, HandleValue expr, HandleValue stmt, TokenPos* pos,
                        MutableHandleValue dst);

    bool withStatement(HandleValue expr, HandleValue stmt, TokenPos* pos, MutableHandleValue dst);

    bool whileStatement(HandleValue test, HandleValue stmt, TokenPos* pos, MutableHandleValue dst);

    bool doWhileStatement(HandleValue stmt, HandleValue test, TokenPos* pos,
                          MutableHandleValue dst);

    bool switchStatement(HandleValue disc, NodeVector& elts, bool lexical, TokenPos* pos,
                         MutableHandleValue dst);

    bool tryStatement(HandleValue body, NodeVector& guarded, HandleValue unguarded,
                      HandleValue finally, TokenPos* pos, MutableHandleValue dst);

    bool debuggerStatement(TokenPos* pos, MutableHandleValue dst);

    bool letStatement(NodeVector& head, HandleValue stmt, TokenPos* pos, MutableHandleValue dst);

    bool importDeclaration(NodeVector& elts, HandleValue moduleSpec, TokenPos* pos, MutableHandleValue dst);

    bool importSpecifier(HandleValue importName, HandleValue bindingName, TokenPos* pos, MutableHandleValue dst);

    bool exportDeclaration(HandleValue decl, NodeVector& elts, HandleValue moduleSpec,
                           HandleValue isDefault, TokenPos* pos, MutableHandleValue dst);

    bool exportSpecifier(HandleValue bindingName, HandleValue exportName, TokenPos* pos, MutableHandleValue dst);

    bool exportBatchSpecifier(TokenPos* pos, MutableHandleValue dst);

    bool classDefinition(bool expr, HandleValue name, HandleValue heritage, HandleValue block, TokenPos* pos,
                         MutableHandleValue dst);
    bool classMethods(NodeVector& methods, MutableHandleValue dst);
    bool classMethod(HandleValue name, HandleValue body, PropKind kind, bool isStatic, TokenPos* pos, MutableHandleValue dst);

    /*
     * expressions
     */

    bool binaryExpression(BinaryOperator op, HandleValue left, HandleValue right, TokenPos* pos,
                          MutableHandleValue dst);

    bool unaryExpression(UnaryOperator op, HandleValue expr, TokenPos* pos, MutableHandleValue dst);

    bool assignmentExpression(AssignmentOperator op, HandleValue lhs, HandleValue rhs,
                              TokenPos* pos, MutableHandleValue dst);

    bool updateExpression(HandleValue expr, bool incr, bool prefix, TokenPos* pos,
                          MutableHandleValue dst);

    bool logicalExpression(bool lor, HandleValue left, HandleValue right, TokenPos* pos,
                           MutableHandleValue dst);

    bool conditionalExpression(HandleValue test, HandleValue cons, HandleValue alt, TokenPos* pos,
                               MutableHandleValue dst);

    bool sequenceExpression(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    bool newExpression(HandleValue callee, NodeVector& args, TokenPos* pos, MutableHandleValue dst);

    bool callExpression(HandleValue callee, NodeVector& args, TokenPos* pos,
                        MutableHandleValue dst);

    bool memberExpression(bool computed, HandleValue expr, HandleValue member, TokenPos* pos,
                          MutableHandleValue dst);

    bool arrayExpression(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    bool templateLiteral(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    bool taggedTemplate(HandleValue callee, NodeVector& args, TokenPos* pos,
                        MutableHandleValue dst);

    bool callSiteObj(NodeVector& raw, NodeVector& cooked, TokenPos* pos, MutableHandleValue dst);

    bool spreadExpression(HandleValue expr, TokenPos* pos, MutableHandleValue dst);

    bool computedName(HandleValue name, TokenPos* pos, MutableHandleValue dst);

    bool objectExpression(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    bool thisExpression(TokenPos* pos, MutableHandleValue dst);

    bool yieldExpression(HandleValue arg, YieldKind kind, TokenPos* pos, MutableHandleValue dst);

    bool comprehensionBlock(HandleValue patt, HandleValue src, bool isForEach, bool isForOf, TokenPos* pos,
                            MutableHandleValue dst);
    bool comprehensionIf(HandleValue test, TokenPos* pos, MutableHandleValue dst);

    bool comprehensionExpression(HandleValue body, NodeVector& blocks, HandleValue filter,
                                 bool isLegacy, TokenPos* pos, MutableHandleValue dst);

    bool generatorExpression(HandleValue body, NodeVector& blocks, HandleValue filter,
                             bool isLegacy, TokenPos* pos, MutableHandleValue dst);

    bool metaProperty(HandleValue meta, HandleValue property, TokenPos* pos, MutableHandleValue dst);

    bool super(TokenPos* pos, MutableHandleValue dst);

    /*
     * declarations
     */

    bool variableDeclaration(NodeVector& elts, VarDeclKind kind, TokenPos* pos,
                             MutableHandleValue dst);

    /*
     * patterns
     */

    bool arrayPattern(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    bool objectPattern(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    bool propertyPattern(HandleValue key, HandleValue patt, bool isShorthand, TokenPos* pos,
                         MutableHandleValue dst);
};

} /* anonymous namespace */

bool
NodeBuilder::createNode(ASTType type, TokenPos* pos, MutableHandleObject dst)
{
    MOZ_ASSERT(type > AST_ERROR && type < AST_LIMIT);

    RootedValue tv(cx);
    RootedPlainObject node(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!node ||
        !setNodeLoc(node, pos) ||
        !atomValue(nodeTypeNames[type], &tv) ||
        !defineProperty(node, "type", tv)) {
        return false;
    }

    dst.set(node);
    return true;
}

bool
NodeBuilder::newArray(NodeVector& elts, MutableHandleValue dst)
{
    const size_t len = elts.length();
    if (len > UINT32_MAX) {
        ReportAllocationOverflow(cx);
        return false;
    }
    RootedObject array(cx, NewDenseFullyAllocatedArray(cx, uint32_t(len)));
    if (!array)
        return false;

    for (size_t i = 0; i < len; i++) {
        RootedValue val(cx, elts[i]);

        MOZ_ASSERT_IF(val.isMagic(), val.whyMagic() == JS_SERIALIZE_NO_NODE);

        /* Represent "no node" as an array hole by not adding the value. */
        if (val.isMagic(JS_SERIALIZE_NO_NODE))
            continue;

        if (!DefineElement(cx, array, i, val))
            return false;
    }

    dst.setObject(*array);
    return true;
}

bool
NodeBuilder::newNodeLoc(TokenPos* pos, MutableHandleValue dst)
{
    if (!pos) {
        dst.setNull();
        return true;
    }

    RootedObject loc(cx);
    RootedObject to(cx);
    RootedValue val(cx);

    if (!newObject(&loc))
        return false;

    dst.setObject(*loc);

    uint32_t startLineNum, startColumnIndex;
    uint32_t endLineNum, endColumnIndex;
    tokenStream->srcCoords.lineNumAndColumnIndex(pos->begin, &startLineNum, &startColumnIndex);
    tokenStream->srcCoords.lineNumAndColumnIndex(pos->end, &endLineNum, &endColumnIndex);

    if (!newObject(&to))
        return false;
    val.setObject(*to);
    if (!defineProperty(loc, "start", val))
        return false;
    val.setNumber(startLineNum);
    if (!defineProperty(to, "line", val))
        return false;
    val.setNumber(startColumnIndex);
    if (!defineProperty(to, "column", val))
        return false;

    if (!newObject(&to))
        return false;
    val.setObject(*to);
    if (!defineProperty(loc, "end", val))
        return false;
    val.setNumber(endLineNum);
    if (!defineProperty(to, "line", val))
        return false;
    val.setNumber(endColumnIndex);
    if (!defineProperty(to, "column", val))
        return false;

    if (!defineProperty(loc, "source", srcval))
        return false;

    return true;
}

bool
NodeBuilder::setNodeLoc(HandleObject node, TokenPos* pos)
{
    if (!saveLoc) {
        RootedValue nullVal(cx, NullValue());
        defineProperty(node, "loc", nullVal);
        return true;
    }

    RootedValue loc(cx);
    return newNodeLoc(pos, &loc) &&
           defineProperty(node, "loc", loc);
}

bool
NodeBuilder::program(NodeVector& elts, TokenPos* pos, MutableHandleValue dst)
{
    return listNode(AST_PROGRAM, "body", elts, pos, dst);
}

bool
NodeBuilder::blockStatement(NodeVector& elts, TokenPos* pos, MutableHandleValue dst)
{
    return listNode(AST_BLOCK_STMT, "body", elts, pos, dst);
}

bool
NodeBuilder::expressionStatement(HandleValue expr, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_EXPR_STMT]);
    if (!cb.isNull())
        return callback(cb, expr, pos, dst);

    return newNode(AST_EXPR_STMT, pos, "expression", expr, dst);
}

bool
NodeBuilder::emptyStatement(TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_EMPTY_STMT]);
    if (!cb.isNull())
        return callback(cb, pos, dst);

    return newNode(AST_EMPTY_STMT, pos, dst);
}

bool
NodeBuilder::ifStatement(HandleValue test, HandleValue cons, HandleValue alt, TokenPos* pos,
                         MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_IF_STMT]);
    if (!cb.isNull())
        return callback(cb, test, cons, opt(alt), pos, dst);

    return newNode(AST_IF_STMT, pos,
                   "test", test,
                   "consequent", cons,
                   "alternate", alt,
                   dst);
}

bool
NodeBuilder::breakStatement(HandleValue label, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_BREAK_STMT]);
    if (!cb.isNull())
        return callback(cb, opt(label), pos, dst);

    return newNode(AST_BREAK_STMT, pos, "label", label, dst);
}

bool
NodeBuilder::continueStatement(HandleValue label, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_CONTINUE_STMT]);
    if (!cb.isNull())
        return callback(cb, opt(label), pos, dst);

    return newNode(AST_CONTINUE_STMT, pos, "label", label, dst);
}

bool
NodeBuilder::labeledStatement(HandleValue label, HandleValue stmt, TokenPos* pos,
                              MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_LAB_STMT]);
    if (!cb.isNull())
        return callback(cb, label, stmt, pos, dst);

    return newNode(AST_LAB_STMT, pos,
                   "label", label,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::throwStatement(HandleValue arg, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_THROW_STMT]);
    if (!cb.isNull())
        return callback(cb, arg, pos, dst);

    return newNode(AST_THROW_STMT, pos, "argument", arg, dst);
}

bool
NodeBuilder::returnStatement(HandleValue arg, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_RETURN_STMT]);
    if (!cb.isNull())
        return callback(cb, opt(arg), pos, dst);

    return newNode(AST_RETURN_STMT, pos, "argument", arg, dst);
}

bool
NodeBuilder::forStatement(HandleValue init, HandleValue test, HandleValue update, HandleValue stmt,
                          TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_FOR_STMT]);
    if (!cb.isNull())
        return callback(cb, opt(init), opt(test), opt(update), stmt, pos, dst);

    return newNode(AST_FOR_STMT, pos,
                   "init", init,
                   "test", test,
                   "update", update,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::forInStatement(HandleValue var, HandleValue expr, HandleValue stmt, bool isForEach,
                            TokenPos* pos, MutableHandleValue dst)
{
    RootedValue isForEachVal(cx, BooleanValue(isForEach));

    RootedValue cb(cx, callbacks[AST_FOR_IN_STMT]);
    if (!cb.isNull())
        return callback(cb, var, expr, stmt, isForEachVal, pos, dst);

    return newNode(AST_FOR_IN_STMT, pos,
                   "left", var,
                   "right", expr,
                   "body", stmt,
                   "each", isForEachVal,
                   dst);
}

bool
NodeBuilder::forOfStatement(HandleValue var, HandleValue expr, HandleValue stmt, TokenPos* pos,
                            MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_FOR_OF_STMT]);
    if (!cb.isNull())
        return callback(cb, var, expr, stmt, pos, dst);

    return newNode(AST_FOR_OF_STMT, pos,
                   "left", var,
                   "right", expr,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::withStatement(HandleValue expr, HandleValue stmt, TokenPos* pos,
                           MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_WITH_STMT]);
    if (!cb.isNull())
        return callback(cb, expr, stmt, pos, dst);

    return newNode(AST_WITH_STMT, pos,
                   "object", expr,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::whileStatement(HandleValue test, HandleValue stmt, TokenPos* pos,
                            MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_WHILE_STMT]);
    if (!cb.isNull())
        return callback(cb, test, stmt, pos, dst);

    return newNode(AST_WHILE_STMT, pos,
                   "test", test,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::doWhileStatement(HandleValue stmt, HandleValue test, TokenPos* pos,
                              MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_DO_STMT]);
    if (!cb.isNull())
        return callback(cb, stmt, test, pos, dst);

    return newNode(AST_DO_STMT, pos,
                   "body", stmt,
                   "test", test,
                   dst);
}

bool
NodeBuilder::switchStatement(HandleValue disc, NodeVector& elts, bool lexical, TokenPos* pos,
                             MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(elts, &array))
        return false;

    RootedValue lexicalVal(cx, BooleanValue(lexical));

    RootedValue cb(cx, callbacks[AST_SWITCH_STMT]);
    if (!cb.isNull())
        return callback(cb, disc, array, lexicalVal, pos, dst);

    return newNode(AST_SWITCH_STMT, pos,
                   "discriminant", disc,
                   "cases", array,
                   "lexical", lexicalVal,
                   dst);
}

bool
NodeBuilder::tryStatement(HandleValue body, NodeVector& guarded, HandleValue unguarded,
                          HandleValue finally, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue guardedHandlers(cx);
    if (!newArray(guarded, &guardedHandlers))
        return false;

    RootedValue cb(cx, callbacks[AST_TRY_STMT]);
    if (!cb.isNull())
        return callback(cb, body, guardedHandlers, unguarded, opt(finally), pos, dst);

    return newNode(AST_TRY_STMT, pos,
                   "block", body,
                   "guardedHandlers", guardedHandlers,
                   "handler", unguarded,
                   "finalizer", finally,
                   dst);
}

bool
NodeBuilder::debuggerStatement(TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_DEBUGGER_STMT]);
    if (!cb.isNull())
        return callback(cb, pos, dst);

    return newNode(AST_DEBUGGER_STMT, pos, dst);
}

bool
NodeBuilder::binaryExpression(BinaryOperator op, HandleValue left, HandleValue right, TokenPos* pos,
                              MutableHandleValue dst)
{
    MOZ_ASSERT(op > BINOP_ERR && op < BINOP_LIMIT);

    RootedValue opName(cx);
    if (!atomValue(binopNames[op], &opName))
        return false;

    RootedValue cb(cx, callbacks[AST_BINARY_EXPR]);
    if (!cb.isNull())
        return callback(cb, opName, left, right, pos, dst);

    return newNode(AST_BINARY_EXPR, pos,
                   "operator", opName,
                   "left", left,
                   "right", right,
                   dst);
}

bool
NodeBuilder::unaryExpression(UnaryOperator unop, HandleValue expr, TokenPos* pos,
                             MutableHandleValue dst)
{
    MOZ_ASSERT(unop > UNOP_ERR && unop < UNOP_LIMIT);

    RootedValue opName(cx);
    if (!atomValue(unopNames[unop], &opName))
        return false;

    RootedValue cb(cx, callbacks[AST_UNARY_EXPR]);
    if (!cb.isNull())
        return callback(cb, opName, expr, pos, dst);

    RootedValue trueVal(cx, BooleanValue(true));
    return newNode(AST_UNARY_EXPR, pos,
                   "operator", opName,
                   "argument", expr,
                   "prefix", trueVal,
                   dst);
}

bool
NodeBuilder::assignmentExpression(AssignmentOperator aop, HandleValue lhs, HandleValue rhs,
                                  TokenPos* pos, MutableHandleValue dst)
{
    MOZ_ASSERT(aop > AOP_ERR && aop < AOP_LIMIT);

    RootedValue opName(cx);
    if (!atomValue(aopNames[aop], &opName))
        return false;

    RootedValue cb(cx, callbacks[AST_ASSIGN_EXPR]);
    if (!cb.isNull())
        return callback(cb, opName, lhs, rhs, pos, dst);

    return newNode(AST_ASSIGN_EXPR, pos,
                   "operator", opName,
                   "left", lhs,
                   "right", rhs,
                   dst);
}

bool
NodeBuilder::updateExpression(HandleValue expr, bool incr, bool prefix, TokenPos* pos,
                              MutableHandleValue dst)
{
    RootedValue opName(cx);
    if (!atomValue(incr ? "++" : "--", &opName))
        return false;

    RootedValue prefixVal(cx, BooleanValue(prefix));

    RootedValue cb(cx, callbacks[AST_UPDATE_EXPR]);
    if (!cb.isNull())
        return callback(cb, expr, opName, prefixVal, pos, dst);

    return newNode(AST_UPDATE_EXPR, pos,
                   "operator", opName,
                   "argument", expr,
                   "prefix", prefixVal,
                   dst);
}

bool
NodeBuilder::logicalExpression(bool lor, HandleValue left, HandleValue right, TokenPos* pos,
                               MutableHandleValue dst)
{
    RootedValue opName(cx);
    if (!atomValue(lor ? "||" : "&&", &opName))
        return false;

    RootedValue cb(cx, callbacks[AST_LOGICAL_EXPR]);
    if (!cb.isNull())
        return callback(cb, opName, left, right, pos, dst);

    return newNode(AST_LOGICAL_EXPR, pos,
                   "operator", opName,
                   "left", left,
                   "right", right,
                   dst);
}

bool
NodeBuilder::conditionalExpression(HandleValue test, HandleValue cons, HandleValue alt,
                                   TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_COND_EXPR]);
    if (!cb.isNull())
        return callback(cb, test, cons, alt, pos, dst);

    return newNode(AST_COND_EXPR, pos,
                   "test", test,
                   "consequent", cons,
                   "alternate", alt,
                   dst);
}

bool
NodeBuilder::sequenceExpression(NodeVector& elts, TokenPos* pos, MutableHandleValue dst)
{
    return listNode(AST_LIST_EXPR, "expressions", elts, pos, dst);
}

bool
NodeBuilder::callExpression(HandleValue callee, NodeVector& args, TokenPos* pos,
                            MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(args, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_CALL_EXPR]);
    if (!cb.isNull())
        return callback(cb, callee, array, pos, dst);

    return newNode(AST_CALL_EXPR, pos,
                   "callee", callee,
                   "arguments", array,
                   dst);
}

bool
NodeBuilder::newExpression(HandleValue callee, NodeVector& args, TokenPos* pos,
                           MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(args, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_NEW_EXPR]);
    if (!cb.isNull())
        return callback(cb, callee, array, pos, dst);

    return newNode(AST_NEW_EXPR, pos,
                   "callee", callee,
                   "arguments", array,
                   dst);
}

bool
NodeBuilder::memberExpression(bool computed, HandleValue expr, HandleValue member, TokenPos* pos,
                              MutableHandleValue dst)
{
    RootedValue computedVal(cx, BooleanValue(computed));

    RootedValue cb(cx, callbacks[AST_MEMBER_EXPR]);
    if (!cb.isNull())
        return callback(cb, computedVal, expr, member, pos, dst);

    return newNode(AST_MEMBER_EXPR, pos,
                   "object", expr,
                   "property", member,
                   "computed", computedVal,
                   dst);
}

bool
NodeBuilder::arrayExpression(NodeVector& elts, TokenPos* pos, MutableHandleValue dst)
{
    return listNode(AST_ARRAY_EXPR, "elements", elts, pos, dst);
}

bool
NodeBuilder::callSiteObj(NodeVector& raw, NodeVector& cooked, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue rawVal(cx);
    if (!newArray(raw, &rawVal))
        return false;

    RootedValue cookedVal(cx);
    if (!newArray(cooked, &cookedVal))
        return false;

    return newNode(AST_CALL_SITE_OBJ, pos,
                   "raw", rawVal,
                   "cooked", cookedVal,
                    dst);
}

bool
NodeBuilder::taggedTemplate(HandleValue callee, NodeVector& args, TokenPos* pos,
                            MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(args, &array))
        return false;

    return newNode(AST_TAGGED_TEMPLATE, pos,
                   "callee", callee,
                   "arguments", array,
                   dst);
}

bool
NodeBuilder::templateLiteral(NodeVector& elts, TokenPos* pos, MutableHandleValue dst)
{
    return listNode(AST_TEMPLATE_LITERAL, "elements", elts, pos, dst);
}

bool
NodeBuilder::computedName(HandleValue name, TokenPos* pos, MutableHandleValue dst)
{
    return newNode(AST_COMPUTED_NAME, pos,
                   "name", name,
                   dst);
}

bool
NodeBuilder::spreadExpression(HandleValue expr, TokenPos* pos, MutableHandleValue dst)
{
    return newNode(AST_SPREAD_EXPR, pos,
                   "expression", expr,
                   dst);
}

bool
NodeBuilder::propertyPattern(HandleValue key, HandleValue patt, bool isShorthand, TokenPos* pos,
                             MutableHandleValue dst)
{
    RootedValue kindName(cx);
    if (!atomValue("init", &kindName))
        return false;

    RootedValue isShorthandVal(cx, BooleanValue(isShorthand));

    RootedValue cb(cx, callbacks[AST_PROP_PATT]);
    if (!cb.isNull())
        return callback(cb, key, patt, pos, dst);

    return newNode(AST_PROP_PATT, pos,
                   "key", key,
                   "value", patt,
                   "kind", kindName,
                   "shorthand", isShorthandVal,
                   dst);
}

bool
NodeBuilder::prototypeMutation(HandleValue val, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_PROTOTYPEMUTATION]);
    if (!cb.isNull())
        return callback(cb, val, pos, dst);

    return newNode(AST_PROTOTYPEMUTATION, pos,
                   "value", val,
                   dst);
}

bool
NodeBuilder::propertyInitializer(HandleValue key, HandleValue val, PropKind kind, bool isShorthand,
                                 bool isMethod, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue kindName(cx);
    if (!atomValue(kind == PROP_INIT
                   ? "init"
                   : kind == PROP_GETTER
                   ? "get"
                   : "set", &kindName)) {
        return false;
    }

    RootedValue isShorthandVal(cx, BooleanValue(isShorthand));
    RootedValue isMethodVal(cx, BooleanValue(isMethod));

    RootedValue cb(cx, callbacks[AST_PROPERTY]);
    if (!cb.isNull())
        return callback(cb, kindName, key, val, pos, dst);

    return newNode(AST_PROPERTY, pos,
                   "key", key,
                   "value", val,
                   "kind", kindName,
                   "method", isMethodVal,
                   "shorthand", isShorthandVal,
                   dst);
}

bool
NodeBuilder::objectExpression(NodeVector& elts, TokenPos* pos, MutableHandleValue dst)
{
    return listNode(AST_OBJECT_EXPR, "properties", elts, pos, dst);
}

bool
NodeBuilder::thisExpression(TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_THIS_EXPR]);
    if (!cb.isNull())
        return callback(cb, pos, dst);

    return newNode(AST_THIS_EXPR, pos, dst);
}

bool
NodeBuilder::yieldExpression(HandleValue arg, YieldKind kind, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_YIELD_EXPR]);
    RootedValue delegateVal(cx);

    switch (kind) {
      case Delegating:
        delegateVal = BooleanValue(true);
        break;
      case NotDelegating:
        delegateVal = BooleanValue(false);
        break;
    }

    if (!cb.isNull())
        return callback(cb, opt(arg), delegateVal, pos, dst);
    return newNode(AST_YIELD_EXPR, pos, "argument", arg, "delegate", delegateVal, dst);
}

bool
NodeBuilder::comprehensionBlock(HandleValue patt, HandleValue src, bool isForEach, bool isForOf, TokenPos* pos,
                                MutableHandleValue dst)
{
    RootedValue isForEachVal(cx, BooleanValue(isForEach));
    RootedValue isForOfVal(cx, BooleanValue(isForOf));

    RootedValue cb(cx, callbacks[AST_COMP_BLOCK]);
    if (!cb.isNull())
        return callback(cb, patt, src, isForEachVal, isForOfVal, pos, dst);

    return newNode(AST_COMP_BLOCK, pos,
                   "left", patt,
                   "right", src,
                   "each", isForEachVal,
                   "of", isForOfVal,
                   dst);
}

bool
NodeBuilder::comprehensionIf(HandleValue test, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_COMP_IF]);
    if (!cb.isNull())
        return callback(cb, test, pos, dst);

    return newNode(AST_COMP_IF, pos,
                   "test", test,
                   dst);
}

bool
NodeBuilder::comprehensionExpression(HandleValue body, NodeVector& blocks, HandleValue filter,
                                     bool isLegacy, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(blocks, &array))
        return false;

    RootedValue style(cx);
    if (!atomValue(isLegacy ? "legacy" : "modern", &style))
        return false;

    RootedValue cb(cx, callbacks[AST_COMP_EXPR]);
    if (!cb.isNull())
        return callback(cb, body, array, opt(filter), style, pos, dst);

    return newNode(AST_COMP_EXPR, pos,
                   "body", body,
                   "blocks", array,
                   "filter", filter,
                   "style", style,
                   dst);
}

bool
NodeBuilder::generatorExpression(HandleValue body, NodeVector& blocks, HandleValue filter,
                                 bool isLegacy, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(blocks, &array))
        return false;

    RootedValue style(cx);
    if (!atomValue(isLegacy ? "legacy" : "modern", &style))
        return false;

    RootedValue cb(cx, callbacks[AST_GENERATOR_EXPR]);
    if (!cb.isNull())
        return callback(cb, body, array, opt(filter), style, pos, dst);

    return newNode(AST_GENERATOR_EXPR, pos,
                   "body", body,
                   "blocks", array,
                   "filter", filter,
                   "style", style,
                   dst);
}

bool
NodeBuilder::letStatement(NodeVector& head, HandleValue stmt, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(head, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_LET_STMT]);
    if (!cb.isNull())
        return callback(cb, array, stmt, pos, dst);

    return newNode(AST_LET_STMT, pos,
                   "head", array,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::importDeclaration(NodeVector& elts, HandleValue moduleSpec, TokenPos* pos,
                               MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(elts, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_IMPORT_DECL]);
    if (!cb.isNull())
        return callback(cb, array, moduleSpec, pos, dst);

    return newNode(AST_IMPORT_DECL, pos,
                   "specifiers", array,
                   "source", moduleSpec,
                   dst);
}

bool
NodeBuilder::importSpecifier(HandleValue importName, HandleValue bindingName, TokenPos* pos,
                             MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_IMPORT_SPEC]);
    if (!cb.isNull())
        return callback(cb, importName, bindingName, pos, dst);

    return newNode(AST_IMPORT_SPEC, pos,
                   "id", importName,
                   "name", bindingName,
                   dst);
}

bool
NodeBuilder::exportDeclaration(HandleValue decl, NodeVector& elts, HandleValue moduleSpec,
                               HandleValue isDefault, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue array(cx, NullValue());
    if (decl.isNull() && !newArray(elts, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_EXPORT_DECL]);

    if (!cb.isNull())
        return callback(cb, decl, array, moduleSpec, pos, dst);

    return newNode(AST_EXPORT_DECL, pos,
                   "declaration", decl,
                   "specifiers", array,
                   "source", moduleSpec,
                   "isDefault", isDefault,
                   dst);
}

bool
NodeBuilder::exportSpecifier(HandleValue bindingName, HandleValue exportName, TokenPos* pos,
                             MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_EXPORT_SPEC]);
    if (!cb.isNull())
        return callback(cb, bindingName, exportName, pos, dst);

    return newNode(AST_EXPORT_SPEC, pos,
                   "id", bindingName,
                   "name", exportName,
                   dst);
}

bool
NodeBuilder::exportBatchSpecifier(TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_EXPORT_BATCH_SPEC]);
    if (!cb.isNull())
        return callback(cb, pos, dst);

    return newNode(AST_EXPORT_BATCH_SPEC, pos, dst);
}

bool
NodeBuilder::variableDeclaration(NodeVector& elts, VarDeclKind kind, TokenPos* pos,
                                 MutableHandleValue dst)
{
    MOZ_ASSERT(kind > VARDECL_ERR && kind < VARDECL_LIMIT);

    RootedValue array(cx), kindName(cx);
    if (!newArray(elts, &array) ||
        !atomValue(kind == VARDECL_CONST
                   ? "const"
                   : kind == VARDECL_LET
                   ? "let"
                   : "var", &kindName)) {
        return false;
    }

    RootedValue cb(cx, callbacks[AST_VAR_DECL]);
    if (!cb.isNull())
        return callback(cb, kindName, array, pos, dst);

    return newNode(AST_VAR_DECL, pos,
                   "kind", kindName,
                   "declarations", array,
                   dst);
}

bool
NodeBuilder::variableDeclarator(HandleValue id, HandleValue init, TokenPos* pos,
                                MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_VAR_DTOR]);
    if (!cb.isNull())
        return callback(cb, id, opt(init), pos, dst);

    return newNode(AST_VAR_DTOR, pos, "id", id, "init", init, dst);
}

bool
NodeBuilder::switchCase(HandleValue expr, NodeVector& elts, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(elts, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_CASE]);
    if (!cb.isNull())
        return callback(cb, opt(expr), array, pos, dst);

    return newNode(AST_CASE, pos,
                   "test", expr,
                   "consequent", array,
                   dst);
}

bool
NodeBuilder::catchClause(HandleValue var, HandleValue guard, HandleValue body, TokenPos* pos,
                         MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_CATCH]);
    if (!cb.isNull())
        return callback(cb, var, opt(guard), body, pos, dst);

    return newNode(AST_CATCH, pos,
                   "param", var,
                   "guard", guard,
                   "body", body,
                   dst);
}

bool
NodeBuilder::literal(HandleValue val, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_LITERAL]);
    if (!cb.isNull())
        return callback(cb, val, pos, dst);

    return newNode(AST_LITERAL, pos, "value", val, dst);
}

bool
NodeBuilder::identifier(HandleValue name, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_IDENTIFIER]);
    if (!cb.isNull())
        return callback(cb, name, pos, dst);

    return newNode(AST_IDENTIFIER, pos, "name", name, dst);
}

bool
NodeBuilder::objectPattern(NodeVector& elts, TokenPos* pos, MutableHandleValue dst)
{
    return listNode(AST_OBJECT_PATT, "properties", elts, pos, dst);
}

bool
NodeBuilder::arrayPattern(NodeVector& elts, TokenPos* pos, MutableHandleValue dst)
{
    return listNode(AST_ARRAY_PATT, "elements", elts, pos, dst);
}

bool
NodeBuilder::function(ASTType type, TokenPos* pos,
                      HandleValue id, NodeVector& args, NodeVector& defaults,
                      HandleValue body, HandleValue rest,
                      GeneratorStyle generatorStyle, bool isExpression,
                      MutableHandleValue dst)
{
    RootedValue array(cx), defarray(cx);
    if (!newArray(args, &array))
        return false;
    if (!newArray(defaults, &defarray))
        return false;

    bool isGenerator = generatorStyle != GeneratorStyle::None;
    RootedValue isGeneratorVal(cx, BooleanValue(isGenerator));
    RootedValue isExpressionVal(cx, BooleanValue(isExpression));

    RootedValue cb(cx, callbacks[type]);
    if (!cb.isNull()) {
        return callback(cb, opt(id), array, body, isGeneratorVal, isExpressionVal, pos, dst);
    }

    if (isGenerator) {
        // Distinguish ES6 generators from legacy generators.
        RootedValue styleVal(cx);
        JSAtom* styleStr = generatorStyle == GeneratorStyle::ES6
                           ? Atomize(cx, "es6", 3)
                           : Atomize(cx, "legacy", 6);
        if (!styleStr)
            return false;
        styleVal.setString(styleStr);
        return newNode(type, pos,
                       "id", id,
                       "params", array,
                       "defaults", defarray,
                       "body", body,
                       "rest", rest,
                       "generator", isGeneratorVal,
                       "style", styleVal,
                       "expression", isExpressionVal,
                       dst);
    }

    return newNode(type, pos,
                   "id", id,
                   "params", array,
                   "defaults", defarray,
                   "body", body,
                   "rest", rest,
                   "generator", isGeneratorVal,
                   "expression", isExpressionVal,
                   dst);
}

bool
NodeBuilder::classMethod(HandleValue name, HandleValue body, PropKind kind, bool isStatic,
                         TokenPos* pos, MutableHandleValue dst)
{
    RootedValue kindName(cx);
    if (!atomValue(kind == PROP_INIT
                   ? "method"
                   : kind == PROP_GETTER
                   ? "get"
                   : "set", &kindName)) {
        return false;
    }

    RootedValue isStaticVal(cx, BooleanValue(isStatic));
    RootedValue cb(cx, callbacks[AST_CLASS_METHOD]);
    if (!cb.isNull())
        return callback(cb, kindName, name, body, isStaticVal, pos, dst);

    return newNode(AST_CLASS_METHOD, pos,
                   "name", name,
                   "body", body,
                   "kind", kindName,
                   "static", isStaticVal,
                   dst);
}

bool
NodeBuilder::classMethods(NodeVector& methods, MutableHandleValue dst)
{
    return newArray(methods, dst);
}

bool
NodeBuilder::classDefinition(bool expr, HandleValue name, HandleValue heritage, HandleValue block,
                             TokenPos* pos, MutableHandleValue dst)
{
    ASTType type = expr ? AST_CLASS_EXPR : AST_CLASS_STMT;
    RootedValue cb(cx, callbacks[type]);
    if (!cb.isNull())
        return callback(cb, name, heritage, block, pos, dst);

    return newNode(type, pos,
                   "id", name,
                   "superClass", heritage,
                   "body", block,
                   dst);
}

bool
NodeBuilder::metaProperty(HandleValue meta, HandleValue property, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_METAPROPERTY]);
    if (!cb.isNull())
        return callback(cb, meta, property, pos, dst);

    return newNode(AST_METAPROPERTY, pos,
                   "meta", meta,
                   "property", property,
                   dst);
}

bool
NodeBuilder::super(TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_SUPER]);
    if (!cb.isNull())
        return callback(cb, pos, dst);

    return newNode(AST_SUPER, pos, dst);
}

namespace {

/*
 * Serialization of parse nodes to JavaScript objects.
 *
 * All serialization methods take a non-nullable ParseNode pointer.
 */
class ASTSerializer
{
    JSContext*          cx;
    Parser<FullParseHandler>* parser;
    NodeBuilder         builder;
    DebugOnly<uint32_t> lineno;

    Value unrootedAtomContents(JSAtom* atom) {
        return StringValue(atom ? atom : cx->names().empty);
    }

    BinaryOperator binop(ParseNodeKind kind, JSOp op);
    UnaryOperator unop(ParseNodeKind kind, JSOp op);
    AssignmentOperator aop(JSOp op);

    bool statements(ParseNode* pn, NodeVector& elts);
    bool expressions(ParseNode* pn, NodeVector& elts);
    bool leftAssociate(ParseNode* pn, MutableHandleValue dst);
    bool rightAssociate(ParseNode* pn, MutableHandleValue dst);
    bool functionArgs(ParseNode* pn, ParseNode* pnargs, ParseNode* pnbody,
                      NodeVector& args, NodeVector& defaults, MutableHandleValue rest);

    bool sourceElement(ParseNode* pn, MutableHandleValue dst);

    bool declaration(ParseNode* pn, MutableHandleValue dst);
    bool variableDeclaration(ParseNode* pn, bool lexical, MutableHandleValue dst);
    bool variableDeclarator(ParseNode* pn, MutableHandleValue dst);
    bool letBlock(ParseNode* pn, MutableHandleValue dst);
    bool importDeclaration(ParseNode* pn, MutableHandleValue dst);
    bool importSpecifier(ParseNode* pn, MutableHandleValue dst);
    bool exportDeclaration(ParseNode* pn, MutableHandleValue dst);
    bool exportSpecifier(ParseNode* pn, MutableHandleValue dst);
    bool classDefinition(ParseNode* pn, bool expr, MutableHandleValue dst);

    bool optStatement(ParseNode* pn, MutableHandleValue dst) {
        if (!pn) {
            dst.setMagic(JS_SERIALIZE_NO_NODE);
            return true;
        }
        return statement(pn, dst);
    }

    bool forInit(ParseNode* pn, MutableHandleValue dst);
    bool forIn(ParseNode* loop, ParseNode* head, HandleValue var, HandleValue stmt,
               MutableHandleValue dst);
    bool forOf(ParseNode* loop, ParseNode* head, HandleValue var, HandleValue stmt,
               MutableHandleValue dst);
    bool statement(ParseNode* pn, MutableHandleValue dst);
    bool blockStatement(ParseNode* pn, MutableHandleValue dst);
    bool switchStatement(ParseNode* pn, MutableHandleValue dst);
    bool switchCase(ParseNode* pn, MutableHandleValue dst);
    bool tryStatement(ParseNode* pn, MutableHandleValue dst);
    bool catchClause(ParseNode* pn, bool* isGuarded, MutableHandleValue dst);

    bool optExpression(ParseNode* pn, MutableHandleValue dst) {
        if (!pn) {
            dst.setMagic(JS_SERIALIZE_NO_NODE);
            return true;
        }
        return expression(pn, dst);
    }

    bool expression(ParseNode* pn, MutableHandleValue dst);

    bool propertyName(ParseNode* pn, MutableHandleValue dst);
    bool property(ParseNode* pn, MutableHandleValue dst);

    bool classMethod(ParseNode* pn, MutableHandleValue dst);

    bool optIdentifier(HandleAtom atom, TokenPos* pos, MutableHandleValue dst) {
        if (!atom) {
            dst.setMagic(JS_SERIALIZE_NO_NODE);
            return true;
        }
        return identifier(atom, pos, dst);
    }

    bool identifier(HandleAtom atom, TokenPos* pos, MutableHandleValue dst);
    bool identifier(ParseNode* pn, MutableHandleValue dst);
    bool objectPropertyName(ParseNode* pn, MutableHandleValue dst);
    bool literal(ParseNode* pn, MutableHandleValue dst);

    bool pattern(ParseNode* pn, MutableHandleValue dst);
    bool arrayPattern(ParseNode* pn, MutableHandleValue dst);
    bool objectPattern(ParseNode* pn, MutableHandleValue dst);

    bool function(ParseNode* pn, ASTType type, MutableHandleValue dst);
    bool functionArgsAndBody(ParseNode* pn, NodeVector& args, NodeVector& defaults,
                             MutableHandleValue body, MutableHandleValue rest);
    bool functionBody(ParseNode* pn, TokenPos* pos, MutableHandleValue dst);

    bool comprehensionBlock(ParseNode* pn, MutableHandleValue dst);
    bool comprehensionIf(ParseNode* pn, MutableHandleValue dst);
    bool comprehension(ParseNode* pn, MutableHandleValue dst);
    bool generatorExpression(ParseNode* pn, MutableHandleValue dst);

  public:
    ASTSerializer(JSContext* c, bool l, char const* src, uint32_t ln)
        : cx(c)
        , builder(c, l, src)
#ifdef DEBUG
        , lineno(ln)
#endif
    {}

    bool init(HandleObject userobj) {
        return builder.init(userobj);
    }

    void setParser(Parser<FullParseHandler>* p) {
        parser = p;
        builder.setTokenStream(&p->tokenStream);
    }

    bool program(ParseNode* pn, MutableHandleValue dst);
};

} /* anonymous namespace */

AssignmentOperator
ASTSerializer::aop(JSOp op)
{
    switch (op) {
      case JSOP_NOP:
        return AOP_ASSIGN;
      case JSOP_ADD:
        return AOP_PLUS;
      case JSOP_SUB:
        return AOP_MINUS;
      case JSOP_MUL:
        return AOP_STAR;
      case JSOP_DIV:
        return AOP_DIV;
      case JSOP_MOD:
        return AOP_MOD;
      case JSOP_POW:
        return AOP_POW;
      case JSOP_LSH:
        return AOP_LSH;
      case JSOP_RSH:
        return AOP_RSH;
      case JSOP_URSH:
        return AOP_URSH;
      case JSOP_BITOR:
        return AOP_BITOR;
      case JSOP_BITXOR:
        return AOP_BITXOR;
      case JSOP_BITAND:
        return AOP_BITAND;
      default:
        return AOP_ERR;
    }
}

UnaryOperator
ASTSerializer::unop(ParseNodeKind kind, JSOp op)
{
    if (IsDeleteKind(kind))
        return UNOP_DELETE;

    if (kind == PNK_TYPEOFNAME || kind == PNK_TYPEOFEXPR)
        return UNOP_TYPEOF;

    switch (op) {
      case JSOP_NEG:
        return UNOP_NEG;
      case JSOP_POS:
        return UNOP_POS;
      case JSOP_NOT:
        return UNOP_NOT;
      case JSOP_BITNOT:
        return UNOP_BITNOT;
      case JSOP_VOID:
        return UNOP_VOID;
      default:
        return UNOP_ERR;
    }
}

BinaryOperator
ASTSerializer::binop(ParseNodeKind kind, JSOp op)
{
    switch (kind) {
      case PNK_LSH:
        return BINOP_LSH;
      case PNK_RSH:
        return BINOP_RSH;
      case PNK_URSH:
        return BINOP_URSH;
      case PNK_LT:
        return BINOP_LT;
      case PNK_LE:
        return BINOP_LE;
      case PNK_GT:
        return BINOP_GT;
      case PNK_GE:
        return BINOP_GE;
      case PNK_EQ:
        return BINOP_EQ;
      case PNK_NE:
        return BINOP_NE;
      case PNK_STRICTEQ:
        return BINOP_STRICTEQ;
      case PNK_STRICTNE:
        return BINOP_STRICTNE;
      case PNK_ADD:
        return BINOP_ADD;
      case PNK_SUB:
        return BINOP_SUB;
      case PNK_STAR:
        return BINOP_STAR;
      case PNK_DIV:
        return BINOP_DIV;
      case PNK_MOD:
        return BINOP_MOD;
      case PNK_POW:
        return BINOP_POW;
      case PNK_BITOR:
        return BINOP_BITOR;
      case PNK_BITXOR:
        return BINOP_BITXOR;
      case PNK_BITAND:
        return BINOP_BITAND;
      case PNK_IN:
        return BINOP_IN;
      case PNK_INSTANCEOF:
        return BINOP_INSTANCEOF;
      default:
        return BINOP_ERR;
    }
}

bool
ASTSerializer::statements(ParseNode* pn, NodeVector& elts)
{
    MOZ_ASSERT(pn->isKind(PNK_STATEMENTLIST));
    MOZ_ASSERT(pn->isArity(PN_LIST));

    if (!elts.reserve(pn->pn_count))
        return false;

    for (ParseNode* next = pn->pn_head; next; next = next->pn_next) {
        MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

        RootedValue elt(cx);
        if (!sourceElement(next, &elt))
            return false;
        elts.infallibleAppend(elt);
    }

    return true;
}

bool
ASTSerializer::expressions(ParseNode* pn, NodeVector& elts)
{
    if (!elts.reserve(pn->pn_count))
        return false;

    for (ParseNode* next = pn->pn_head; next; next = next->pn_next) {
        MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

        RootedValue elt(cx);
        if (!expression(next, &elt))
            return false;
        elts.infallibleAppend(elt);
    }

    return true;
}

bool
ASTSerializer::blockStatement(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(PNK_STATEMENTLIST));

    NodeVector stmts(cx);
    return statements(pn, stmts) &&
           builder.blockStatement(stmts, &pn->pn_pos, dst);
}

bool
ASTSerializer::program(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(parser->tokenStream.srcCoords.lineNum(pn->pn_pos.begin) == lineno);

    NodeVector stmts(cx);
    return statements(pn, stmts) &&
           builder.program(stmts, &pn->pn_pos, dst);
}

bool
ASTSerializer::sourceElement(ParseNode* pn, MutableHandleValue dst)
{
    /* SpiderMonkey allows declarations even in pure statement contexts. */
    return statement(pn, dst);
}

bool
ASTSerializer::declaration(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(PNK_FUNCTION) ||
               pn->isKind(PNK_VAR) ||
               pn->isKind(PNK_LET) ||
               pn->isKind(PNK_CONST));

    switch (pn->getKind()) {
      case PNK_FUNCTION:
        return function(pn, AST_FUNC_DECL, dst);

      case PNK_VAR:
        return variableDeclaration(pn, false, dst);

      default:
        MOZ_ASSERT(pn->isKind(PNK_LET) || pn->isKind(PNK_CONST));
        return variableDeclaration(pn, true, dst);
    }
}

bool
ASTSerializer::variableDeclaration(ParseNode* pn, bool lexical, MutableHandleValue dst)
{
    MOZ_ASSERT_IF(lexical, pn->isKind(PNK_LET) || pn->isKind(PNK_CONST));
    MOZ_ASSERT_IF(!lexical, pn->isKind(PNK_VAR));

    VarDeclKind kind = VARDECL_ERR;
    // Treat both the toplevel const binding (secretly var-like) and the lexical const
    // the same way
    if (lexical)
        kind = pn->isKind(PNK_LET) ? VARDECL_LET : VARDECL_CONST;
    else
        kind = pn->isKind(PNK_VAR) ? VARDECL_VAR : VARDECL_CONST;

    NodeVector dtors(cx);
    if (!dtors.reserve(pn->pn_count))
        return false;
    for (ParseNode* next = pn->pn_head; next; next = next->pn_next) {
        RootedValue child(cx);
        if (!variableDeclarator(next, &child))
            return false;
        dtors.infallibleAppend(child);
    }
    return builder.variableDeclaration(dtors, kind, &pn->pn_pos, dst);
}

bool
ASTSerializer::variableDeclarator(ParseNode* pn, MutableHandleValue dst)
{
    ParseNode* pnleft;
    ParseNode* pnright;

    if (pn->isKind(PNK_NAME)) {
        pnleft = pn;
        pnright = pn->isUsed() ? nullptr : pn->pn_expr;
        MOZ_ASSERT_IF(pnright, pn->pn_pos.encloses(pnright->pn_pos));
    } else if (pn->isKind(PNK_ASSIGN)) {
        pnleft = pn->pn_left;
        pnright = pn->pn_right;
        MOZ_ASSERT(pn->pn_pos.encloses(pnleft->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pnright->pn_pos));
    } else {
        /* This happens for a destructuring declarator in a for-in/of loop. */
        pnleft = pn;
        pnright = nullptr;
    }

    RootedValue left(cx), right(cx);
    return pattern(pnleft, &left) &&
           optExpression(pnright, &right) &&
           builder.variableDeclarator(left, right, &pn->pn_pos, dst);
}

bool
ASTSerializer::letBlock(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
    MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

    ParseNode* letHead = pn->pn_left;
    LOCAL_ASSERT(letHead->isArity(PN_LIST));

    ParseNode* letBody = pn->pn_right;
    LOCAL_ASSERT(letBody->isKind(PNK_LEXICALSCOPE));

    NodeVector dtors(cx);
    if (!dtors.reserve(letHead->pn_count))
        return false;

    for (ParseNode* next = letHead->pn_head; next; next = next->pn_next) {
        RootedValue child(cx);

        if (!variableDeclarator(next, &child))
            return false;
        dtors.infallibleAppend(child);
    }

    RootedValue v(cx);
    return statement(letBody->pn_expr, &v) &&
           builder.letStatement(dtors, v, &pn->pn_pos, dst);
}

bool
ASTSerializer::importDeclaration(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(PNK_IMPORT));
    MOZ_ASSERT(pn->isArity(PN_BINARY));
    MOZ_ASSERT(pn->pn_left->isKind(PNK_IMPORT_SPEC_LIST));
    MOZ_ASSERT(pn->pn_right->isKind(PNK_STRING));

    NodeVector elts(cx);
    if (!elts.reserve(pn->pn_left->pn_count))
        return false;

    for (ParseNode* next = pn->pn_left->pn_head; next; next = next->pn_next) {
        RootedValue elt(cx);
        if (!importSpecifier(next, &elt))
            return false;
        elts.infallibleAppend(elt);
    }

    RootedValue moduleSpec(cx);
    return literal(pn->pn_right, &moduleSpec) &&
           builder.importDeclaration(elts, moduleSpec, &pn->pn_pos, dst);
}

bool
ASTSerializer::importSpecifier(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(PNK_IMPORT_SPEC));

    RootedValue importName(cx);
    RootedValue bindingName(cx);
    return identifier(pn->pn_left, &importName) &&
           identifier(pn->pn_right, &bindingName) &&
           builder.importSpecifier(importName, bindingName, &pn->pn_pos, dst);
}

bool
ASTSerializer::exportDeclaration(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(PNK_EXPORT) ||
               pn->isKind(PNK_EXPORT_FROM) ||
               pn->isKind(PNK_EXPORT_DEFAULT));
    MOZ_ASSERT(pn->getArity() == pn->isKind(PNK_EXPORT) ? PN_UNARY : PN_BINARY);
    MOZ_ASSERT_IF(pn->isKind(PNK_EXPORT_FROM), pn->pn_right->isKind(PNK_STRING));

    RootedValue decl(cx, NullValue());
    NodeVector elts(cx);

    ParseNode* kid = pn->isKind(PNK_EXPORT) ? pn->pn_kid : pn->pn_left;
    switch (ParseNodeKind kind = kid->getKind()) {
      case PNK_EXPORT_SPEC_LIST:
        if (!elts.reserve(pn->pn_left->pn_count))
            return false;

        for (ParseNode* next = pn->pn_left->pn_head; next; next = next->pn_next) {
            RootedValue elt(cx);
            if (next->isKind(PNK_EXPORT_SPEC)) {
                if (!exportSpecifier(next, &elt))
                    return false;
            } else {
                if (!builder.exportBatchSpecifier(&pn->pn_pos, &elt))
                    return false;
            }
            elts.infallibleAppend(elt);
        }
        break;

      case PNK_FUNCTION:
        if (!function(kid, AST_FUNC_DECL, &decl))
            return false;
        break;

      case PNK_CLASS:
        if (!classDefinition(kid, false, &decl))
            return false;
        break;

      case PNK_VAR:
      case PNK_CONST:
      case PNK_LET:
        if (!variableDeclaration(kid, kind != PNK_VAR, &decl))
            return false;
        break;

      default:
          if (!expression(kid, &decl))
              return false;
          break;
    }

    RootedValue moduleSpec(cx, NullValue());
    if (pn->isKind(PNK_EXPORT_FROM) && !literal(pn->pn_right, &moduleSpec))
        return false;

    RootedValue isDefault(cx, BooleanValue(false));
    if (pn->isKind(PNK_EXPORT_DEFAULT))
        isDefault.setBoolean(true);

    return builder.exportDeclaration(decl, elts, moduleSpec, isDefault, &pn->pn_pos, dst);
}

bool
ASTSerializer::exportSpecifier(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(PNK_EXPORT_SPEC));

    RootedValue bindingName(cx);
    RootedValue exportName(cx);
    return identifier(pn->pn_left, &bindingName) &&
           identifier(pn->pn_right, &exportName) &&
           builder.exportSpecifier(bindingName, exportName, &pn->pn_pos, dst);
}

bool
ASTSerializer::switchCase(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT_IF(pn->pn_left, pn->pn_pos.encloses(pn->pn_left->pn_pos));
    MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

    NodeVector stmts(cx);

    RootedValue expr(cx);

    return optExpression(pn->as<CaseClause>().caseExpression(), &expr) &&
           statements(pn->as<CaseClause>().statementList(), stmts) &&
           builder.switchCase(expr, stmts, &pn->pn_pos, dst);
}

bool
ASTSerializer::switchStatement(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
    MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

    RootedValue disc(cx);

    if (!expression(pn->pn_left, &disc))
        return false;

    ParseNode* listNode;
    bool lexical;

    if (pn->pn_right->isKind(PNK_LEXICALSCOPE)) {
        listNode = pn->pn_right->pn_expr;
        lexical = true;
    } else {
        listNode = pn->pn_right;
        lexical = false;
    }

    NodeVector cases(cx);
    if (!cases.reserve(listNode->pn_count))
        return false;

    for (ParseNode* next = listNode->pn_head; next; next = next->pn_next) {
        RootedValue child(cx);
        if (!switchCase(next, &child))
            return false;
        cases.infallibleAppend(child);
    }

    return builder.switchStatement(disc, cases, lexical, &pn->pn_pos, dst);
}

bool
ASTSerializer::catchClause(ParseNode* pn, bool* isGuarded, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid1->pn_pos));
    MOZ_ASSERT_IF(pn->pn_kid2, pn->pn_pos.encloses(pn->pn_kid2->pn_pos));
    MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid3->pn_pos));

    RootedValue var(cx), guard(cx), body(cx);

    if (!pattern(pn->pn_kid1, &var) ||
        !optExpression(pn->pn_kid2, &guard)) {
        return false;
    }

    *isGuarded = !guard.isMagic(JS_SERIALIZE_NO_NODE);

    return statement(pn->pn_kid3, &body) &&
           builder.catchClause(var, guard, body, &pn->pn_pos, dst);
}

bool
ASTSerializer::tryStatement(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid1->pn_pos));
    MOZ_ASSERT_IF(pn->pn_kid2, pn->pn_pos.encloses(pn->pn_kid2->pn_pos));
    MOZ_ASSERT_IF(pn->pn_kid3, pn->pn_pos.encloses(pn->pn_kid3->pn_pos));

    RootedValue body(cx);
    if (!statement(pn->pn_kid1, &body))
        return false;

    NodeVector guarded(cx);
    RootedValue unguarded(cx, NullValue());

    if (ParseNode* catchList = pn->pn_kid2) {
        if (!guarded.reserve(catchList->pn_count))
            return false;

        for (ParseNode* next = catchList->pn_head; next; next = next->pn_next) {
            RootedValue clause(cx);
            bool isGuarded;
            if (!catchClause(next->pn_expr, &isGuarded, &clause))
                return false;
            if (isGuarded)
                guarded.infallibleAppend(clause);
            else
                unguarded = clause;
        }
    }

    RootedValue finally(cx);
    return optStatement(pn->pn_kid3, &finally) &&
           builder.tryStatement(body, guarded, unguarded, finally, &pn->pn_pos, dst);
}

bool
ASTSerializer::forInit(ParseNode* pn, MutableHandleValue dst)
{
    if (!pn) {
        dst.setMagic(JS_SERIALIZE_NO_NODE);
        return true;
    }

    bool lexical = pn->isKind(PNK_LET) || pn->isKind(PNK_CONST);
    return (lexical || pn->isKind(PNK_VAR))
           ? variableDeclaration(pn, lexical, dst)
           : expression(pn, dst);
}

bool
ASTSerializer::forOf(ParseNode* loop, ParseNode* head, HandleValue var, HandleValue stmt,
                         MutableHandleValue dst)
{
    RootedValue expr(cx);

    return expression(head->pn_kid3, &expr) &&
        builder.forOfStatement(var, expr, stmt, &loop->pn_pos, dst);
}

bool
ASTSerializer::forIn(ParseNode* loop, ParseNode* head, HandleValue var, HandleValue stmt,
                         MutableHandleValue dst)
{
    RootedValue expr(cx);
    bool isForEach = loop->pn_iflags & JSITER_FOREACH;

    return expression(head->pn_kid3, &expr) &&
        builder.forInStatement(var, expr, stmt, isForEach, &loop->pn_pos, dst);
}

bool
ASTSerializer::classDefinition(ParseNode* pn, bool expr, MutableHandleValue dst)
{
    RootedValue className(cx, MagicValue(JS_SERIALIZE_NO_NODE));
    RootedValue heritage(cx);
    RootedValue classBody(cx);

    if (pn->pn_kid1) {
        if (!identifier(pn->pn_kid1->as<ClassNames>().innerBinding(), &className))
            return false;
    }

    return optExpression(pn->pn_kid2, &heritage) &&
           statement(pn->pn_kid3, &classBody) &&
           builder.classDefinition(expr, className, heritage, classBody, &pn->pn_pos, dst);
}

bool
ASTSerializer::statement(ParseNode* pn, MutableHandleValue dst)
{
    JS_CHECK_RECURSION(cx, return false);
    switch (pn->getKind()) {
      case PNK_FUNCTION:
      case PNK_VAR:
        return declaration(pn, dst);

      case PNK_LETBLOCK:
        return letBlock(pn, dst);

      case PNK_LET:
      case PNK_CONST:
        return declaration(pn, dst);

      case PNK_IMPORT:
        return importDeclaration(pn, dst);

      case PNK_EXPORT:
      case PNK_EXPORT_DEFAULT:
      case PNK_EXPORT_FROM:
        return exportDeclaration(pn, dst);

      case PNK_NAME:
        LOCAL_ASSERT(pn->isUsed());
        return statement(pn->pn_lexdef, dst);

      case PNK_SEMI:
        if (pn->pn_kid) {
            RootedValue expr(cx);
            return expression(pn->pn_kid, &expr) &&
                   builder.expressionStatement(expr, &pn->pn_pos, dst);
        }
        return builder.emptyStatement(&pn->pn_pos, dst);

      case PNK_LEXICALSCOPE:
        pn = pn->pn_expr;
        if (!pn->isKind(PNK_STATEMENTLIST))
            return statement(pn, dst);
        /* FALL THROUGH */

      case PNK_STATEMENTLIST:
        return blockStatement(pn, dst);

      case PNK_IF:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid1->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid2->pn_pos));
        MOZ_ASSERT_IF(pn->pn_kid3, pn->pn_pos.encloses(pn->pn_kid3->pn_pos));

        RootedValue test(cx), cons(cx), alt(cx);

        return expression(pn->pn_kid1, &test) &&
               statement(pn->pn_kid2, &cons) &&
               optStatement(pn->pn_kid3, &alt) &&
               builder.ifStatement(test, cons, alt, &pn->pn_pos, dst);
      }

      case PNK_SWITCH:
        return switchStatement(pn, dst);

      case PNK_TRY:
        return tryStatement(pn, dst);

      case PNK_WITH:
      case PNK_WHILE:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        RootedValue expr(cx), stmt(cx);

        return expression(pn->pn_left, &expr) &&
               statement(pn->pn_right, &stmt) &&
               (pn->isKind(PNK_WITH)
                ? builder.withStatement(expr, stmt, &pn->pn_pos, dst)
                : builder.whileStatement(expr, stmt, &pn->pn_pos, dst));
      }

      case PNK_DOWHILE:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        RootedValue stmt(cx), test(cx);

        return statement(pn->pn_left, &stmt) &&
               expression(pn->pn_right, &test) &&
               builder.doWhileStatement(stmt, test, &pn->pn_pos, dst);
      }

      case PNK_FOR:
      case PNK_COMPREHENSIONFOR:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        ParseNode* head = pn->pn_left;

        MOZ_ASSERT_IF(head->pn_kid1, head->pn_pos.encloses(head->pn_kid1->pn_pos));
        MOZ_ASSERT_IF(head->pn_kid2, head->pn_pos.encloses(head->pn_kid2->pn_pos));
        MOZ_ASSERT_IF(head->pn_kid3, head->pn_pos.encloses(head->pn_kid3->pn_pos));

        RootedValue stmt(cx);
        if (!statement(pn->pn_right, &stmt))
            return false;

        if (head->isKind(PNK_FORIN) || head->isKind(PNK_FOROF)) {
            RootedValue var(cx);
            if (!head->pn_kid1) {
                if (!pattern(head->pn_kid2, &var))
                    return false;
            } else if (head->pn_kid1->isKind(PNK_LEXICALSCOPE)) {
                if (!variableDeclaration(head->pn_kid1->pn_expr, true, &var))
                    return false;
            } else {
                if (!variableDeclaration(head->pn_kid1,
                                         head->pn_kid1->isKind(PNK_LET) ||
                                         head->pn_kid1->isKind(PNK_CONST),
                                         &var))
                {
                    return false;
                }
            }
            if (head->isKind(PNK_FORIN))
                return forIn(pn, head, var, stmt, dst);
            return forOf(pn, head, var, stmt, dst);
        }

        RootedValue init(cx), test(cx), update(cx);

        return forInit(head->pn_kid1, &init) &&
               optExpression(head->pn_kid2, &test) &&
               optExpression(head->pn_kid3, &update) &&
               builder.forStatement(init, test, update, stmt, &pn->pn_pos, dst);
      }

      case PNK_BREAK:
      case PNK_CONTINUE:
      {
        RootedValue label(cx);
        RootedAtom pnAtom(cx, pn->pn_atom);
        return optIdentifier(pnAtom, nullptr, &label) &&
               (pn->isKind(PNK_BREAK)
                ? builder.breakStatement(label, &pn->pn_pos, dst)
                : builder.continueStatement(label, &pn->pn_pos, dst));
      }

      case PNK_LABEL:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_expr->pn_pos));

        RootedValue label(cx), stmt(cx);
        RootedAtom pnAtom(cx, pn->as<LabeledStatement>().label());
        return identifier(pnAtom, nullptr, &label) &&
               statement(pn->pn_expr, &stmt) &&
               builder.labeledStatement(label, stmt, &pn->pn_pos, dst);
      }

      case PNK_THROW:
      {
        MOZ_ASSERT_IF(pn->pn_kid, pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        RootedValue arg(cx);

        return optExpression(pn->pn_kid, &arg) &&
               builder.throwStatement(arg, &pn->pn_pos, dst);
      }

      case PNK_RETURN:
      {
        MOZ_ASSERT_IF(pn->pn_kid, pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        RootedValue arg(cx);

        return optExpression(pn->pn_kid, &arg) &&
               builder.returnStatement(arg, &pn->pn_pos, dst);
      }

      case PNK_DEBUGGER:
        return builder.debuggerStatement(&pn->pn_pos, dst);

      case PNK_CLASS:
        return classDefinition(pn, false, dst);

      case PNK_CLASSMETHODLIST:
      {
        NodeVector methods(cx);
        if (!methods.reserve(pn->pn_count))
            return false;

        for (ParseNode* next = pn->pn_head; next; next = next->pn_next) {
            MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

            RootedValue prop(cx);
            if (!classMethod(next, &prop))
                return false;
            methods.infallibleAppend(prop);
        }

        return builder.classMethods(methods, dst);
      }

      case PNK_NOP:
        return builder.emptyStatement(&pn->pn_pos, dst);

      default:
        LOCAL_NOT_REACHED("unexpected statement type");
    }
}

bool
ASTSerializer::classMethod(ParseNode* pn, MutableHandleValue dst)
{
    PropKind kind;
    switch (pn->getOp()) {
      case JSOP_INITPROP:
        kind = PROP_INIT;
        break;

      case JSOP_INITPROP_GETTER:
        kind = PROP_GETTER;
        break;

      case JSOP_INITPROP_SETTER:
        kind = PROP_SETTER;
        break;

      default:
        LOCAL_NOT_REACHED("unexpected object-literal property");
    }

    RootedValue key(cx), val(cx);
    bool isStatic = pn->as<ClassMethod>().isStatic();
    return propertyName(pn->pn_left, &key) &&
           expression(pn->pn_right, &val) &&
           builder.classMethod(key, val, kind, isStatic, &pn->pn_pos, dst);
}

bool
ASTSerializer::leftAssociate(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));
    MOZ_ASSERT(pn->pn_count >= 1);

    ParseNodeKind kind = pn->getKind();
    bool lor = kind == PNK_OR;
    bool logop = lor || (kind == PNK_AND);

    ParseNode* head = pn->pn_head;
    RootedValue left(cx);
    if (!expression(head, &left))
        return false;
    for (ParseNode* next = head->pn_next; next; next = next->pn_next) {
        RootedValue right(cx);
        if (!expression(next, &right))
            return false;

        TokenPos subpos(pn->pn_pos.begin, next->pn_pos.end);

        if (logop) {
            if (!builder.logicalExpression(lor, left, right, &subpos, &left))
                return false;
        } else {
            BinaryOperator op = binop(pn->getKind(), pn->getOp());
            LOCAL_ASSERT(op > BINOP_ERR && op < BINOP_LIMIT);

            if (!builder.binaryExpression(op, left, right, &subpos, &left))
                return false;
        }
    }

    dst.set(left);
    return true;
}

bool
ASTSerializer::rightAssociate(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));
    MOZ_ASSERT(pn->pn_count >= 1);

    // First, we need to reverse the list, so that we can traverse it in the right order.
    // It's OK to destructively reverse the list, because there are no other consumers.

    ParseNode* head = pn->pn_head;
    ParseNode* prev = nullptr;
    ParseNode* current = head;
    ParseNode* next;
    while (current != nullptr) {
        next = current->pn_next;
        current->pn_next = prev;
        prev = current;
        current = next;
    }

    head = prev;

    RootedValue right(cx);
    if (!expression(head, &right))
        return false;
    for (ParseNode* next = head->pn_next; next; next = next->pn_next) {
        RootedValue left(cx);
        if (!expression(next, &left))
            return false;

        TokenPos subpos(pn->pn_pos.begin, next->pn_pos.end);

        BinaryOperator op = binop(pn->getKind(), pn->getOp());
        LOCAL_ASSERT(op > BINOP_ERR && op < BINOP_LIMIT);

        if (!builder.binaryExpression(op, left, right, &subpos, &right))
            return false;
    }

    dst.set(right);
    return true;
}

bool
ASTSerializer::comprehensionBlock(ParseNode* pn, MutableHandleValue dst)
{
    LOCAL_ASSERT(pn->isArity(PN_BINARY));

    ParseNode* in = pn->pn_left;

    LOCAL_ASSERT(in && (in->isKind(PNK_FORIN) || in->isKind(PNK_FOROF)));

    bool isForEach = in->isKind(PNK_FORIN) && (pn->pn_iflags & JSITER_FOREACH);
    bool isForOf = in->isKind(PNK_FOROF);

    RootedValue patt(cx), src(cx);
    return pattern(in->pn_kid2, &patt) &&
           expression(in->pn_kid3, &src) &&
           builder.comprehensionBlock(patt, src, isForEach, isForOf, &in->pn_pos, dst);
}

bool
ASTSerializer::comprehensionIf(ParseNode* pn, MutableHandleValue dst)
{
    LOCAL_ASSERT(pn->isKind(PNK_IF));
    LOCAL_ASSERT(!pn->pn_kid3);

    RootedValue patt(cx);
    return pattern(pn->pn_kid1, &patt) &&
           builder.comprehensionIf(patt, &pn->pn_pos, dst);
}

bool
ASTSerializer::comprehension(ParseNode* pn, MutableHandleValue dst)
{
    // There are two array comprehension flavors.
    // 1. The kind that was in ES4 for a while: [z for (x in y)]
    // 2. The kind that was in ES6 for a while: [for (x of y) z]
    // They have slightly different parse trees and scoping.
    bool isLegacy = pn->isKind(PNK_LEXICALSCOPE);
    ParseNode* next = isLegacy ? pn->pn_expr : pn;
    LOCAL_ASSERT(next->isKind(PNK_COMPREHENSIONFOR));

    NodeVector blocks(cx);
    RootedValue filter(cx, MagicValue(JS_SERIALIZE_NO_NODE));
    while (true) {
        if (next->isKind(PNK_COMPREHENSIONFOR)) {
            RootedValue block(cx);
            if (!comprehensionBlock(next, &block) || !blocks.append(block))
                return false;
            next = next->pn_right;
        } else if (next->isKind(PNK_IF)) {
            if (isLegacy) {
                MOZ_ASSERT(filter.isMagic(JS_SERIALIZE_NO_NODE));
                if (!optExpression(next->pn_kid1, &filter))
                    return false;
            } else {
                // ES7 comprehension can contain multiple ComprehensionIfs.
                RootedValue compif(cx);
                if (!comprehensionIf(next, &compif) || !blocks.append(compif))
                    return false;
            }
            next = next->pn_kid2;
        } else {
            break;
        }
    }

    LOCAL_ASSERT(next->isKind(PNK_ARRAYPUSH));

    RootedValue body(cx);

    return expression(next->pn_kid, &body) &&
           builder.comprehensionExpression(body, blocks, filter, isLegacy, &pn->pn_pos, dst);
}

bool
ASTSerializer::generatorExpression(ParseNode* pn, MutableHandleValue dst)
{
    // Just as there are two kinds of array comprehension (see
    // ASTSerializer::comprehension), there are legacy and modern generator
    // expression.
    bool isLegacy = pn->isKind(PNK_LEXICALSCOPE);
    ParseNode* next = isLegacy ? pn->pn_expr : pn;
    LOCAL_ASSERT(next->isKind(PNK_COMPREHENSIONFOR));

    NodeVector blocks(cx);
    RootedValue filter(cx, MagicValue(JS_SERIALIZE_NO_NODE));
    while (true) {
        if (next->isKind(PNK_COMPREHENSIONFOR)) {
            RootedValue block(cx);
            if (!comprehensionBlock(next, &block) || !blocks.append(block))
                return false;
            next = next->pn_right;
        } else if (next->isKind(PNK_IF)) {
            if (isLegacy) {
                MOZ_ASSERT(filter.isMagic(JS_SERIALIZE_NO_NODE));
                if (!optExpression(next->pn_kid1, &filter))
                    return false;
            } else {
                // ES7 comprehension can contain multiple ComprehensionIfs.
                RootedValue compif(cx);
                if (!comprehensionIf(next, &compif) || !blocks.append(compif))
                    return false;
            }
            next = next->pn_kid2;
        } else {
            break;
        }
    }

    LOCAL_ASSERT(next->isKind(PNK_SEMI) &&
                 next->pn_kid->isKind(PNK_YIELD) &&
                 next->pn_kid->pn_left);

    RootedValue body(cx);

    return expression(next->pn_kid->pn_left, &body) &&
           builder.generatorExpression(body, blocks, filter, isLegacy, &pn->pn_pos, dst);
}

bool
ASTSerializer::expression(ParseNode* pn, MutableHandleValue dst)
{
    JS_CHECK_RECURSION(cx, return false);
    switch (pn->getKind()) {
      case PNK_FUNCTION:
      {
        ASTType type = pn->pn_funbox->function()->isArrow() ? AST_ARROW_EXPR : AST_FUNC_EXPR;
        return function(pn, type, dst);
      }

      case PNK_COMMA:
      {
        NodeVector exprs(cx);
        return expressions(pn, exprs) &&
               builder.sequenceExpression(exprs, &pn->pn_pos, dst);
      }

      case PNK_CONDITIONAL:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid1->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid2->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid3->pn_pos));

        RootedValue test(cx), cons(cx), alt(cx);

        return expression(pn->pn_kid1, &test) &&
               expression(pn->pn_kid2, &cons) &&
               expression(pn->pn_kid3, &alt) &&
               builder.conditionalExpression(test, cons, alt, &pn->pn_pos, dst);
      }

      case PNK_OR:
      case PNK_AND:
        return leftAssociate(pn, dst);

      case PNK_PREINCREMENT:
      case PNK_PREDECREMENT:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        bool inc = pn->isKind(PNK_PREINCREMENT);
        RootedValue expr(cx);
        return expression(pn->pn_kid, &expr) &&
               builder.updateExpression(expr, inc, true, &pn->pn_pos, dst);
      }

      case PNK_POSTINCREMENT:
      case PNK_POSTDECREMENT:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        bool inc = pn->isKind(PNK_POSTINCREMENT);
        RootedValue expr(cx);
        return expression(pn->pn_kid, &expr) &&
               builder.updateExpression(expr, inc, false, &pn->pn_pos, dst);
      }

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
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        AssignmentOperator op = aop(pn->getOp());
        LOCAL_ASSERT(op > AOP_ERR && op < AOP_LIMIT);

        RootedValue lhs(cx), rhs(cx);
        return pattern(pn->pn_left, &lhs) &&
               expression(pn->pn_right, &rhs) &&
               builder.assignmentExpression(op, lhs, rhs, &pn->pn_pos, dst);
      }

      case PNK_ADD:
      case PNK_SUB:
      case PNK_STRICTEQ:
      case PNK_EQ:
      case PNK_STRICTNE:
      case PNK_NE:
      case PNK_LT:
      case PNK_LE:
      case PNK_GT:
      case PNK_GE:
      case PNK_LSH:
      case PNK_RSH:
      case PNK_URSH:
      case PNK_STAR:
      case PNK_DIV:
      case PNK_MOD:
      case PNK_BITOR:
      case PNK_BITXOR:
      case PNK_BITAND:
      case PNK_IN:
      case PNK_INSTANCEOF:
        return leftAssociate(pn, dst);

      case PNK_POW:
	return rightAssociate(pn, dst);

      case PNK_DELETENAME:
      case PNK_DELETEPROP:
      case PNK_DELETEELEM:
      case PNK_DELETEEXPR:
      case PNK_TYPEOFNAME:
      case PNK_TYPEOFEXPR:
      case PNK_VOID:
      case PNK_NOT:
      case PNK_BITNOT:
      case PNK_POS:
      case PNK_NEG: {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        UnaryOperator op = unop(pn->getKind(), pn->getOp());
        LOCAL_ASSERT(op > UNOP_ERR && op < UNOP_LIMIT);

        RootedValue expr(cx);
        return expression(pn->pn_kid, &expr) &&
               builder.unaryExpression(op, expr, &pn->pn_pos, dst);
      }

#if JS_HAS_GENERATOR_EXPRS
      case PNK_GENEXP:
        return generatorExpression(pn->generatorExpr(), dst);
#endif

      case PNK_NEW:
      case PNK_TAGGED_TEMPLATE:
      case PNK_CALL:
      case PNK_SUPERCALL:
      {
        ParseNode* next = pn->pn_head;
        MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

        RootedValue callee(cx);
        if (pn->isKind(PNK_SUPERCALL)) {
            MOZ_ASSERT(next->isKind(PNK_SUPERBASE));
            if (!builder.super(&next->pn_pos, &callee))
                return false;
        } else {
            if (!expression(next, &callee))
                return false;
        }

        NodeVector args(cx);
        if (!args.reserve(pn->pn_count - 1))
            return false;

        for (next = next->pn_next; next; next = next->pn_next) {
            MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

            RootedValue arg(cx);
            if (!expression(next, &arg))
                return false;
            args.infallibleAppend(arg);
        }

        if (pn->getKind() == PNK_TAGGED_TEMPLATE)
            return builder.taggedTemplate(callee, args, &pn->pn_pos, dst);

        // SUPERCALL is Call(super, args)
        return pn->isKind(PNK_NEW)
               ? builder.newExpression(callee, args, &pn->pn_pos, dst)

            : builder.callExpression(callee, args, &pn->pn_pos, dst);
      }

      case PNK_DOT:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_expr->pn_pos));

        RootedValue expr(cx);
        RootedValue propname(cx);
        RootedAtom pnAtom(cx, pn->pn_atom);

        if (pn->as<PropertyAccess>().isSuper()) {
            if (!builder.super(&pn->pn_expr->pn_pos, &expr))
                return false;
        } else {
            if (!expression(pn->pn_expr, &expr))
                return false;
        }

        return identifier(pnAtom, nullptr, &propname) &&
               builder.memberExpression(false, expr, propname, &pn->pn_pos, dst);
      }

      case PNK_ELEM:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        RootedValue left(cx), right(cx);

        if (pn->as<PropertyByValue>().isSuper()) {
            if (!builder.super(&pn->pn_left->pn_pos, &left))
                return false;
        } else {
            if (!expression(pn->pn_left, &left))
                return false;
        }

        return expression(pn->pn_right, &right) &&
               builder.memberExpression(true, left, right, &pn->pn_pos, dst);
      }

      case PNK_CALLSITEOBJ:
      {
        NodeVector raw(cx);
        if (!raw.reserve(pn->pn_head->pn_count))
            return false;
        for (ParseNode* next = pn->pn_head->pn_head; next; next = next->pn_next) {
            MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

            RootedValue expr(cx);
            expr.setString(next->pn_atom);
            raw.infallibleAppend(expr);
        }

        NodeVector cooked(cx);
        if (!cooked.reserve(pn->pn_count - 1))
            return false;

        for (ParseNode* next = pn->pn_head->pn_next; next; next = next->pn_next) {
            MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

            RootedValue expr(cx);
            expr.setString(next->pn_atom);
            cooked.infallibleAppend(expr);
        }

        return builder.callSiteObj(raw, cooked, &pn->pn_pos, dst);
      }

      case PNK_ARRAY:
      {
        NodeVector elts(cx);
        if (!elts.reserve(pn->pn_count))
            return false;

        for (ParseNode* next = pn->pn_head; next; next = next->pn_next) {
            MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

            if (next->isKind(PNK_ELISION)) {
                elts.infallibleAppend(NullValue());
            } else {
                RootedValue expr(cx);
                if (!expression(next, &expr))
                    return false;
                elts.infallibleAppend(expr);
            }
        }

        return builder.arrayExpression(elts, &pn->pn_pos, dst);
      }

      case PNK_SPREAD:
      {
          RootedValue expr(cx);
          return expression(pn->pn_kid, &expr) &&
                 builder.spreadExpression(expr, &pn->pn_pos, dst);
      }

      case PNK_COMPUTED_NAME:
      {
         RootedValue name(cx);
         return expression(pn->pn_kid, &name) &&
                builder.computedName(name, &pn->pn_pos, dst);
      }

      case PNK_OBJECT:
      {
        NodeVector elts(cx);
        if (!elts.reserve(pn->pn_count))
            return false;

        for (ParseNode* next = pn->pn_head; next; next = next->pn_next) {
            MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

            RootedValue prop(cx);
            if (!property(next, &prop))
                return false;
            elts.infallibleAppend(prop);
        }

        return builder.objectExpression(elts, &pn->pn_pos, dst);
      }

      case PNK_NAME:
        return identifier(pn, dst);

      case PNK_THIS:
        return builder.thisExpression(&pn->pn_pos, dst);

      case PNK_TEMPLATE_STRING_LIST:
      {
        NodeVector elts(cx);
        if (!elts.reserve(pn->pn_count))
            return false;

        for (ParseNode* next = pn->pn_head; next; next = next->pn_next) {
            MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

            RootedValue expr(cx);
            if (!expression(next, &expr))
                return false;
            elts.infallibleAppend(expr);
        }

        return builder.templateLiteral(elts, &pn->pn_pos, dst);
      }

      case PNK_TEMPLATE_STRING:
      case PNK_STRING:
      case PNK_REGEXP:
      case PNK_NUMBER:
      case PNK_TRUE:
      case PNK_FALSE:
      case PNK_NULL:
        return literal(pn, dst);

      case PNK_YIELD_STAR:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));

        RootedValue arg(cx);
        return expression(pn->pn_left, &arg) &&
               builder.yieldExpression(arg, Delegating, &pn->pn_pos, dst);
      }

      case PNK_YIELD:
      {
        MOZ_ASSERT_IF(pn->pn_left, pn->pn_pos.encloses(pn->pn_left->pn_pos));

        RootedValue arg(cx);
        return optExpression(pn->pn_left, &arg) &&
               builder.yieldExpression(arg, NotDelegating, &pn->pn_pos, dst);
      }

      case PNK_ARRAYCOMP:
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_head->pn_pos));

        /* NB: it's no longer the case that pn_count could be 2. */
        LOCAL_ASSERT(pn->pn_count == 1);
        return comprehension(pn->pn_head, dst);

      case PNK_CLASS:
        return classDefinition(pn, true, dst);

      case PNK_NEWTARGET:
      {
        MOZ_ASSERT(pn->pn_left->isKind(PNK_POSHOLDER));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        MOZ_ASSERT(pn->pn_right->isKind(PNK_POSHOLDER));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        RootedValue newIdent(cx);
        RootedValue targetIdent(cx);

        RootedAtom newStr(cx, cx->names().new_);
        RootedAtom targetStr(cx, cx->names().target);

        return identifier(newStr, &pn->pn_left->pn_pos, &newIdent) &&
               identifier(targetStr, &pn->pn_right->pn_pos, &targetIdent) &&
               builder.metaProperty(newIdent, targetIdent, &pn->pn_pos, dst);
      }

      case PNK_SETTHIS:
        // SETTHIS is used to assign the result of a super() call to |this|.
        // It's not part of the original AST, so just forward to the call.
        MOZ_ASSERT(pn->pn_left->isKind(PNK_NAME));
        return expression(pn->pn_right, dst);

      default:
        LOCAL_NOT_REACHED("unexpected expression type");
    }
}

bool
ASTSerializer::propertyName(ParseNode* pn, MutableHandleValue dst)
{
    if (pn->isKind(PNK_COMPUTED_NAME))
        return expression(pn, dst);
    if (pn->isKind(PNK_OBJECT_PROPERTY_NAME))
        return identifier(pn, dst);

    LOCAL_ASSERT(pn->isKind(PNK_STRING) || pn->isKind(PNK_NUMBER));

    return literal(pn, dst);
}

bool
ASTSerializer::property(ParseNode* pn, MutableHandleValue dst)
{
    if (pn->isKind(PNK_MUTATEPROTO)) {
        RootedValue val(cx);
        return expression(pn->pn_kid, &val) &&
               builder.prototypeMutation(val, &pn->pn_pos, dst);
    }

    PropKind kind;
    switch (pn->getOp()) {
      case JSOP_INITPROP:
        kind = PROP_INIT;
        break;

      case JSOP_INITPROP_GETTER:
        kind = PROP_GETTER;
        break;

      case JSOP_INITPROP_SETTER:
        kind = PROP_SETTER;
        break;

      default:
        LOCAL_NOT_REACHED("unexpected object-literal property");
    }

    bool isShorthand = pn->isKind(PNK_SHORTHAND);
    bool isMethod =
        pn->pn_right->isKind(PNK_FUNCTION) &&
        pn->pn_right->pn_funbox->function()->kind() == JSFunction::Method;
    RootedValue key(cx), val(cx);
    return propertyName(pn->pn_left, &key) &&
           expression(pn->pn_right, &val) &&
           builder.propertyInitializer(key, val, kind, isShorthand, isMethod, &pn->pn_pos, dst);
}

bool
ASTSerializer::literal(ParseNode* pn, MutableHandleValue dst)
{
    RootedValue val(cx);
    switch (pn->getKind()) {
      case PNK_TEMPLATE_STRING:
      case PNK_STRING:
        val.setString(pn->pn_atom);
        break;

      case PNK_REGEXP:
      {
        RootedObject re1(cx, pn->as<RegExpLiteral>().objbox()->object);
        LOCAL_ASSERT(re1 && re1->is<RegExpObject>());

        RootedObject re2(cx, CloneRegExpObject(cx, re1));
        if (!re2)
            return false;

        val.setObject(*re2);
        break;
      }

      case PNK_NUMBER:
        val.setNumber(pn->pn_dval);
        break;

      case PNK_NULL:
        val.setNull();
        break;

      case PNK_TRUE:
        val.setBoolean(true);
        break;

      case PNK_FALSE:
        val.setBoolean(false);
        break;

      default:
        LOCAL_NOT_REACHED("unexpected literal type");
    }

    return builder.literal(val, &pn->pn_pos, dst);
}

bool
ASTSerializer::arrayPattern(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(PNK_ARRAY));

    NodeVector elts(cx);
    if (!elts.reserve(pn->pn_count))
        return false;

    for (ParseNode* next = pn->pn_head; next; next = next->pn_next) {
        if (next->isKind(PNK_ELISION)) {
            elts.infallibleAppend(NullValue());
        } else if (next->isKind(PNK_SPREAD)) {
            RootedValue target(cx);
            RootedValue spread(cx);
            if (!pattern(next->pn_kid, &target))
                return false;
            if(!builder.spreadExpression(target, &next->pn_pos, &spread))
                return false;
            elts.infallibleAppend(spread);
        } else {
            RootedValue patt(cx);
            if (!pattern(next, &patt))
                return false;
            elts.infallibleAppend(patt);
        }
    }

    return builder.arrayPattern(elts, &pn->pn_pos, dst);
}

bool
ASTSerializer::objectPattern(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(PNK_OBJECT));

    NodeVector elts(cx);
    if (!elts.reserve(pn->pn_count))
        return false;

    for (ParseNode* propdef = pn->pn_head; propdef; propdef = propdef->pn_next) {
        LOCAL_ASSERT(propdef->isKind(PNK_MUTATEPROTO) != propdef->isOp(JSOP_INITPROP));

        RootedValue key(cx);
        ParseNode* target;
        if (propdef->isKind(PNK_MUTATEPROTO)) {
            RootedValue pname(cx, StringValue(cx->names().proto));
            if (!builder.literal(pname, &propdef->pn_pos, &key))
                return false;
            target = propdef->pn_kid;
        } else {
            if (!propertyName(propdef->pn_left, &key))
                return false;
            target = propdef->pn_right;
        }

        RootedValue patt(cx), prop(cx);
        if (!pattern(target, &patt) ||
            !builder.propertyPattern(key, patt, propdef->isKind(PNK_SHORTHAND), &propdef->pn_pos,
                                     &prop))
        {
            return false;
        }

        elts.infallibleAppend(prop);
    }

    return builder.objectPattern(elts, &pn->pn_pos, dst);
}

bool
ASTSerializer::pattern(ParseNode* pn, MutableHandleValue dst)
{
    JS_CHECK_RECURSION(cx, return false);
    switch (pn->getKind()) {
      case PNK_OBJECT:
        return objectPattern(pn, dst);

      case PNK_ARRAY:
        return arrayPattern(pn, dst);

      default:
        return expression(pn, dst);
    }
}

bool
ASTSerializer::identifier(HandleAtom atom, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue atomContentsVal(cx, unrootedAtomContents(atom));
    return builder.identifier(atomContentsVal, pos, dst);
}

bool
ASTSerializer::identifier(ParseNode* pn, MutableHandleValue dst)
{
    LOCAL_ASSERT(pn->isArity(PN_NAME) || pn->isArity(PN_NULLARY));
    LOCAL_ASSERT(pn->pn_atom);

    RootedAtom pnAtom(cx, pn->pn_atom);
    return identifier(pnAtom, &pn->pn_pos, dst);
}

bool
ASTSerializer::objectPropertyName(ParseNode* pn, MutableHandleValue dst)
{
    LOCAL_ASSERT(pn->isKind(PNK_OBJECT_PROPERTY_NAME));
    LOCAL_ASSERT(pn->isArity(PN_NULLARY));
    LOCAL_ASSERT(pn->pn_atom);

    RootedAtom pnAtom(cx, pn->pn_atom);
    return identifier(pnAtom, &pn->pn_pos, dst);
}

bool
ASTSerializer::function(ParseNode* pn, ASTType type, MutableHandleValue dst)
{
    RootedFunction func(cx, pn->pn_funbox->function());

    GeneratorStyle generatorStyle =
        pn->pn_funbox->isGenerator()
        ? (pn->pn_funbox->isLegacyGenerator()
           ? GeneratorStyle::Legacy
           : GeneratorStyle::ES6)
        : GeneratorStyle::None;

    bool isExpression =
#if JS_HAS_EXPR_CLOSURES
        func->isExprBody();
#else
        false;
#endif

    RootedValue id(cx);
    RootedAtom funcAtom(cx, func->atom());
    if (!optIdentifier(funcAtom, nullptr, &id))
        return false;

    NodeVector args(cx);
    NodeVector defaults(cx);

    RootedValue body(cx), rest(cx);
    if (func->hasRest())
        rest.setUndefined();
    else
        rest.setNull();
    return functionArgsAndBody(pn->pn_body, args, defaults, &body, &rest) &&
        builder.function(type, &pn->pn_pos, id, args, defaults, body,
                         rest, generatorStyle, isExpression, dst);
}

bool
ASTSerializer::functionArgsAndBody(ParseNode* pn, NodeVector& args, NodeVector& defaults,
                                   MutableHandleValue body, MutableHandleValue rest)
{
    ParseNode* pnargs;
    ParseNode* pnbody;

    /* Extract the args and body separately. */
    if (pn->isKind(PNK_ARGSBODY)) {
        pnargs = pn;
        pnbody = pn->last();
    } else {
        pnargs = nullptr;
        pnbody = pn;
    }

    /* Serialize the arguments and body. */
    switch (pnbody->getKind()) {
      case PNK_RETURN: /* expression closure, no destructured args */
        return functionArgs(pn, pnargs, pnbody, args, defaults, rest) &&
               expression(pnbody->pn_kid, body);

      case PNK_STATEMENTLIST:     /* statement closure */
      {
        ParseNode* pnstart = pnbody->pn_head;

        // Skip over initial yield in generator.
        if (pnstart && pnstart->isKind(PNK_YIELD)) {
            MOZ_ASSERT(pnstart->getOp() == JSOP_INITIALYIELD);
            pnstart = pnstart->pn_next;
        }

        return functionArgs(pn, pnargs, pnbody, args, defaults, rest) &&
               functionBody(pnstart, &pnbody->pn_pos, body);
      }

      default:
        LOCAL_NOT_REACHED("unexpected function contents");
    }
}

bool
ASTSerializer::functionArgs(ParseNode* pn, ParseNode* pnargs,
                            ParseNode* pnbody, NodeVector& args, NodeVector& defaults,
                            MutableHandleValue rest)
{
    if (!pnargs)
        return true;

    RootedValue node(cx);
    bool defaultsNull = true;
    MOZ_ASSERT(defaults.empty(),
               "must be initially empty for it to be proper to clear this "
               "when there are no defaults");

    for (ParseNode* arg = pnargs->pn_head; arg && arg != pnbody; arg = arg->pn_next) {
        MOZ_ASSERT(arg->isKind(PNK_NAME) || arg->isKind(PNK_ASSIGN));
        ParseNode* argName = nullptr;
        ParseNode* defNode = nullptr;
        if (arg->isKind(PNK_ASSIGN)) {
            argName = arg->pn_left;
            defNode = arg->pn_right;
        } else if (arg->pn_atom == cx->names().empty) {
            ParseNode* destruct = arg->expr();
            if (destruct->isKind(PNK_ASSIGN)) {
                defNode = destruct->pn_right;
                destruct = destruct->pn_left;
            }
            if (!pattern(destruct, &node) || !args.append(node))
                return false;
        } else {
            argName = arg;
        }
        if (argName) {
            if (!identifier(argName, &node))
                return false;
            if (rest.isUndefined() && arg->pn_next == pnbody)
                rest.setObject(node.toObject());
            else if (!args.append(node))
                return false;
        }
        if (defNode) {
            defaultsNull = false;
            RootedValue def(cx);
            if (!expression(defNode, &def) || !defaults.append(def))
                return false;
        } else {
            if (!defaults.append(NullValue()))
                return false;
        }
    }
    MOZ_ASSERT(!rest.isUndefined());

    if (defaultsNull)
        defaults.clear();

    return true;
}

bool
ASTSerializer::functionBody(ParseNode* pn, TokenPos* pos, MutableHandleValue dst)
{
    NodeVector elts(cx);

    /* We aren't sure how many elements there are up front, so we'll check each append. */
    for (ParseNode* next = pn; next; next = next->pn_next) {
        RootedValue child(cx);
        if (!sourceElement(next, &child) || !elts.append(child))
            return false;
    }

    return builder.blockStatement(elts, pos, dst);
}

static bool
reflect_parse(JSContext* cx, uint32_t argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() < 1) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                             "Reflect.parse", "0", "s");
        return false;
    }

    RootedString src(cx, ToString<CanGC>(cx, args[0]));
    if (!src)
        return false;

    ScopedJSFreePtr<char> filename;
    uint32_t lineno = 1;
    bool loc = true;
    RootedObject builder(cx);
    ParseTarget target = ParseTarget::Script;

    RootedValue arg(cx, args.get(1));

    if (!arg.isNullOrUndefined()) {
        if (!arg.isObject()) {
            ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                                  JSDVG_SEARCH_STACK, arg, nullptr,
                                  "not an object", nullptr);
            return false;
        }

        RootedObject config(cx, &arg.toObject());

        RootedValue prop(cx);

        /* config.loc */
        RootedId locId(cx, NameToId(cx->names().loc));
        RootedValue trueVal(cx, BooleanValue(true));
        if (!GetPropertyDefault(cx, config, locId, trueVal, &prop))
            return false;

        loc = ToBoolean(prop);

        if (loc) {
            /* config.source */
            RootedId sourceId(cx, NameToId(cx->names().source));
            RootedValue nullVal(cx, NullValue());
            if (!GetPropertyDefault(cx, config, sourceId, nullVal, &prop))
                return false;

            if (!prop.isNullOrUndefined()) {
                RootedString str(cx, ToString<CanGC>(cx, prop));
                if (!str)
                    return false;

                filename = JS_EncodeString(cx, str);
                if (!filename)
                    return false;
            }

            /* config.line */
            RootedId lineId(cx, NameToId(cx->names().line));
            RootedValue oneValue(cx, Int32Value(1));
            if (!GetPropertyDefault(cx, config, lineId, oneValue, &prop) ||
                !ToUint32(cx, prop, &lineno)) {
                return false;
            }
        }

        /* config.builder */
        RootedId builderId(cx, NameToId(cx->names().builder));
        RootedValue nullVal(cx, NullValue());
        if (!GetPropertyDefault(cx, config, builderId, nullVal, &prop))
            return false;

        if (!prop.isNullOrUndefined()) {
            if (!prop.isObject()) {
                ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                                      JSDVG_SEARCH_STACK, prop, nullptr,
                                      "not an object", nullptr);
                return false;
            }
            builder = &prop.toObject();
        }

        /* config.target */
        RootedId targetId(cx, NameToId(cx->names().target));
        RootedValue scriptVal(cx, StringValue(cx->names().script));
        if (!GetPropertyDefault(cx, config, targetId, scriptVal, &prop))
            return false;

        if (!prop.isString()) {
            ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK,
                                  prop, nullptr, "not 'script' or 'module'", nullptr);
            return false;
        }

        RootedString stringProp(cx, prop.toString());
        bool isScript = false;
        bool isModule = false;
        if (!EqualStrings(cx, stringProp, cx->names().script, &isScript))
            return false;

        if (!EqualStrings(cx, stringProp, cx->names().module, &isModule))
            return false;

        if (isScript) {
            target = ParseTarget::Script;
        } else if (isModule) {
            target = ParseTarget::Module;
        } else {
            JS_ReportError(cx, "Bad target value, expected 'script' or 'module'");
            return false;
        }
    }

    /* Extract the builder methods first to report errors before parsing. */
    ASTSerializer serialize(cx, loc, filename, lineno);
    if (!serialize.init(builder))
        return false;

    JSLinearString* linear = src->ensureLinear(cx);
    if (!linear)
        return false;

    AutoStableStringChars linearChars(cx);
    if (!linearChars.initTwoByte(cx, linear))
        return false;

    CompileOptions options(cx);
    options.setFileAndLine(filename, lineno);
    options.setCanLazilyParse(false);
    mozilla::Range<const char16_t> chars = linearChars.twoByteRange();
    Parser<FullParseHandler> parser(cx, &cx->tempLifoAlloc(), options, chars.start().get(),
                                    chars.length(), /* foldConstants = */ false, nullptr, nullptr);
    if (!parser.checkOptions())
        return false;

    serialize.setParser(&parser);

    ParseNode* pn;
    if (target == ParseTarget::Script) {
        pn = parser.parse();
        if (!pn)
            return false;
    } else {
        Rooted<ModuleObject*> module(cx, ModuleObject::create(cx, nullptr));
        if (!module)
            return false;

        pn = parser.standaloneModule(module);
        if (!pn)
            return false;

        MOZ_ASSERT(pn->getKind() == PNK_MODULE);
        pn = pn->pn_body;
    }

    RootedValue val(cx);
    if (!serialize.program(pn, &val)) {
        args.rval().setNull();
        return false;
    }

    args.rval().set(val);
    return true;
}

JS_PUBLIC_API(bool)
JS_InitReflectParse(JSContext* cx, HandleObject global)
{
    RootedValue reflectVal(cx);
    if (!GetProperty(cx, global, global, cx->names().Reflect, &reflectVal))
        return false;
    if (!reflectVal.isObject()) {
        JS_ReportError(cx, "JS_InitReflectParse must be called during global initialization");
        return false;
    }

    RootedObject reflectObj(cx, &reflectVal.toObject());
    return JS_DefineFunction(cx, reflectObj, "parse", reflect_parse, 1, 0);
}
