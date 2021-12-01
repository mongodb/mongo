/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS reflection package. */

#include "mozilla/DebugOnly.h"
#include "mozilla/Move.h"

#include <stdlib.h>

#include "jsarray.h"
#include "jspubtd.h"

#include "builtin/Reflect.h"
#include "frontend/Parser.h"
#include "frontend/TokenStream.h"
#include "js/CharacterEncoding.h"
#include "vm/JSAtom.h"
#include "vm/JSObject.h"
#include "vm/RegExpObject.h"

#include "frontend/ParseNode-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::frontend;

using JS::AutoValueArray;
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
    BINOP_IN, BINOP_INSTANCEOF, BINOP_PIPELINE,

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
    UNOP_AWAIT,

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
    "|>",         /* BINOP_PIPELINE */
};

static const char* const unopNames[] = {
    "delete",  /* UNOP_DELETE */
    "-",       /* UNOP_NEG */
    "+",       /* UNOP_POS */
    "!",       /* UNOP_NOT */
    "~",       /* UNOP_BITNOT */
    "typeof",  /* UNOP_TYPEOF */
    "void",    /* UNOP_VOID */
    "await"    /* UNOP_AWAIT */
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
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_PARSE_NODE);\
            return false;                                                                 \
        }                                                                                 \
    JS_END_MACRO

#define LOCAL_NOT_REACHED(expr)                                                           \
    JS_BEGIN_MACRO                                                                        \
        MOZ_ASSERT(false);                                                                \
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_PARSE_NODE);    \
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
    TokenStreamAnyChars* tokenStream;
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

    MOZ_MUST_USE bool init(HandleObject userobj = nullptr) {
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

    void setTokenStream(TokenStreamAnyChars* ts) {
        tokenStream = ts;
    }

  private:
    MOZ_MUST_USE bool callbackHelper(HandleValue fun, const InvokeArgs& args, size_t i,
                                     TokenPos* pos, MutableHandleValue dst)
    {
        // The end of the implementation of callback(). All arguments except
        // loc have already been stored in range [0, i).
        if (saveLoc) {
            if (!newNodeLoc(pos, args[i]))
                return false;
        }

        return js::Call(cx, fun, userv, args, dst);
    }

    // Helper function for callback(). Note that all Arguments must be types
    // that convert to HandleValue, so this isn't as template-y as it seems,
    // just variadic.
    template <typename... Arguments>
    MOZ_MUST_USE bool callbackHelper(HandleValue fun, const InvokeArgs& args, size_t i,
                                     HandleValue head, Arguments&&... tail)
    {
        // Recursive loop to store the arguments into args. This eventually
        // bottoms out in a call to the non-template callbackHelper() above.
        args[i].set(head);
        return callbackHelper(fun, args, i + 1, Forward<Arguments>(tail)...);
    }

    // Invoke a user-defined callback. The actual signature is:
    //
    //     bool callback(HandleValue fun, HandleValue... args, TokenPos* pos,
    //                   MutableHandleValue dst);
    template <typename... Arguments>
    MOZ_MUST_USE bool callback(HandleValue fun, Arguments&&... args) {
        InvokeArgs iargs(cx);
        if (!iargs.init(cx, sizeof...(args) - 2 + size_t(saveLoc)))
            return false;

        return callbackHelper(fun, iargs, 0, Forward<Arguments>(args)...);
    }

    // WARNING: Returning a Handle is non-standard, but it works in this case
    // because both |v| and |UndefinedHandleValue| are definitely rooted on a
    // previous stack frame (i.e. we're just choosing between two
    // already-rooted values).
    HandleValue opt(HandleValue v) {
        MOZ_ASSERT_IF(v.isMagic(), v.whyMagic() == JS_SERIALIZE_NO_NODE);
        return v.isMagic(JS_SERIALIZE_NO_NODE) ? JS::UndefinedHandleValue : v;
    }

    MOZ_MUST_USE bool atomValue(const char* s, MutableHandleValue dst) {
        /*
         * Bug 575416: instead of Atomize, lookup constant atoms in tbl file
         */
        RootedAtom atom(cx, Atomize(cx, s, strlen(s)));
        if (!atom)
            return false;

        dst.setString(atom);
        return true;
    }

    MOZ_MUST_USE bool newObject(MutableHandleObject dst) {
        RootedPlainObject nobj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!nobj)
            return false;

        dst.set(nobj);
        return true;
    }

    MOZ_MUST_USE bool newArray(NodeVector& elts, MutableHandleValue dst);

    MOZ_MUST_USE bool createNode(ASTType type, TokenPos* pos, MutableHandleObject dst);

    MOZ_MUST_USE bool newNodeHelper(HandleObject obj, MutableHandleValue dst) {
        // The end of the implementation of newNode().
        MOZ_ASSERT(obj);
        dst.setObject(*obj);
        return true;
    }

    template <typename... Arguments>
    MOZ_MUST_USE bool newNodeHelper(HandleObject obj, const char *name, HandleValue value,
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
    MOZ_MUST_USE bool newNode(ASTType type, TokenPos* pos, Arguments&&... args) {
        RootedObject node(cx);
        return createNode(type, pos, &node) &&
               newNodeHelper(node, Forward<Arguments>(args)...);
    }

    MOZ_MUST_USE bool listNode(ASTType type, const char* propName, NodeVector& elts, TokenPos* pos,
                               MutableHandleValue dst) {
        RootedValue array(cx);
        if (!newArray(elts, &array))
            return false;

        RootedValue cb(cx, callbacks[type]);
        if (!cb.isNull())
            return callback(cb, array, pos, dst);

        return newNode(type, pos, propName, array, dst);
    }

    MOZ_MUST_USE bool defineProperty(HandleObject obj, const char* name, HandleValue val) {
        MOZ_ASSERT_IF(val.isMagic(), val.whyMagic() == JS_SERIALIZE_NO_NODE);

        /*
         * Bug 575416: instead of Atomize, lookup constant atoms in tbl file
         */
        RootedAtom atom(cx, Atomize(cx, name, strlen(name)));
        if (!atom)
            return false;

        /* Represent "no node" as null and ensure users are not exposed to magic values. */
        RootedValue optVal(cx, val.isMagic(JS_SERIALIZE_NO_NODE) ? NullValue() : val);
        return DefineDataProperty(cx, obj, atom->asPropertyName(), optVal);
    }

    MOZ_MUST_USE bool newNodeLoc(TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool setNodeLoc(HandleObject node, TokenPos* pos);

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

    MOZ_MUST_USE bool program(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool literal(HandleValue val, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool identifier(HandleValue name, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool function(ASTType type, TokenPos* pos,
                               HandleValue id, NodeVector& args, NodeVector& defaults,
                               HandleValue body, HandleValue rest, GeneratorStyle generatorStyle,
                               bool isAsync, bool isExpression, MutableHandleValue dst);

    MOZ_MUST_USE bool variableDeclarator(HandleValue id, HandleValue init, TokenPos* pos,
                                         MutableHandleValue dst);

    MOZ_MUST_USE bool switchCase(HandleValue expr, NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool catchClause(HandleValue var, HandleValue body, TokenPos* pos,
                                  MutableHandleValue dst);

    MOZ_MUST_USE bool prototypeMutation(HandleValue val, TokenPos* pos, MutableHandleValue dst);
    MOZ_MUST_USE bool propertyInitializer(HandleValue key, HandleValue val, PropKind kind,
                                          bool isShorthand, bool isMethod, TokenPos* pos,
                                          MutableHandleValue dst);


    /*
     * statements
     */

    MOZ_MUST_USE bool blockStatement(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool expressionStatement(HandleValue expr, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool emptyStatement(TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool ifStatement(HandleValue test, HandleValue cons, HandleValue alt, TokenPos* pos,
                     MutableHandleValue dst);

    MOZ_MUST_USE bool breakStatement(HandleValue label, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool continueStatement(HandleValue label, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool labeledStatement(HandleValue label, HandleValue stmt, TokenPos* pos,
                          MutableHandleValue dst);

    MOZ_MUST_USE bool throwStatement(HandleValue arg, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool returnStatement(HandleValue arg, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool forStatement(HandleValue init, HandleValue test, HandleValue update, HandleValue stmt,
                      TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool forInStatement(HandleValue var, HandleValue expr, HandleValue stmt,
                                     TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool forOfStatement(HandleValue var, HandleValue expr, HandleValue stmt, TokenPos* pos,
                                     MutableHandleValue dst);

    MOZ_MUST_USE bool withStatement(HandleValue expr, HandleValue stmt, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool whileStatement(HandleValue test, HandleValue stmt, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool doWhileStatement(HandleValue stmt, HandleValue test, TokenPos* pos,
                                       MutableHandleValue dst);

    MOZ_MUST_USE bool switchStatement(HandleValue disc, NodeVector& elts, bool lexical, TokenPos* pos,
                                      MutableHandleValue dst);

    MOZ_MUST_USE bool tryStatement(HandleValue body, HandleValue handler,
                                   HandleValue finally, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool debuggerStatement(TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool importDeclaration(NodeVector& elts, HandleValue moduleSpec, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool importSpecifier(HandleValue importName, HandleValue bindingName, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool exportDeclaration(HandleValue decl, NodeVector& elts, HandleValue moduleSpec,
                                        HandleValue isDefault, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool exportSpecifier(HandleValue bindingName, HandleValue exportName, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool exportBatchSpecifier(TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool classDefinition(bool expr, HandleValue name, HandleValue heritage,
                                      HandleValue block, TokenPos* pos, MutableHandleValue dst);
    MOZ_MUST_USE bool classMethods(NodeVector& methods, MutableHandleValue dst);
    MOZ_MUST_USE bool classMethod(HandleValue name, HandleValue body, PropKind kind, bool isStatic,
                                  TokenPos* pos, MutableHandleValue dst);

    /*
     * expressions
     */

    MOZ_MUST_USE bool binaryExpression(BinaryOperator op, HandleValue left, HandleValue right,
                                       TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool unaryExpression(UnaryOperator op, HandleValue expr, TokenPos* pos,
                                      MutableHandleValue dst);

    MOZ_MUST_USE bool assignmentExpression(AssignmentOperator op, HandleValue lhs, HandleValue rhs,
                                           TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool updateExpression(HandleValue expr, bool incr, bool prefix, TokenPos* pos,
                                       MutableHandleValue dst);

    MOZ_MUST_USE bool logicalExpression(bool lor, HandleValue left, HandleValue right, TokenPos* pos,
                                        MutableHandleValue dst);

    MOZ_MUST_USE bool conditionalExpression(HandleValue test, HandleValue cons, HandleValue alt,
                                            TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool sequenceExpression(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool newExpression(HandleValue callee, NodeVector& args, TokenPos* pos,
                                    MutableHandleValue dst);

    MOZ_MUST_USE bool callExpression(HandleValue callee, NodeVector& args, TokenPos* pos,
                                     MutableHandleValue dst);

    MOZ_MUST_USE bool memberExpression(bool computed, HandleValue expr, HandleValue member,
                                       TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool arrayExpression(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool templateLiteral(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool taggedTemplate(HandleValue callee, NodeVector& args, TokenPos* pos,
                                     MutableHandleValue dst);

    MOZ_MUST_USE bool callSiteObj(NodeVector& raw, NodeVector& cooked, TokenPos* pos,
                                  MutableHandleValue dst);

    MOZ_MUST_USE bool spreadExpression(HandleValue expr, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool computedName(HandleValue name, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool objectExpression(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool thisExpression(TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool yieldExpression(HandleValue arg, YieldKind kind, TokenPos* pos,
                                      MutableHandleValue dst);

    MOZ_MUST_USE bool metaProperty(HandleValue meta, HandleValue property, TokenPos* pos,
                                   MutableHandleValue dst);

    MOZ_MUST_USE bool super(TokenPos* pos, MutableHandleValue dst);

    /*
     * declarations
     */

    MOZ_MUST_USE bool variableDeclaration(NodeVector& elts, VarDeclKind kind, TokenPos* pos,
                                          MutableHandleValue dst);

    /*
     * patterns
     */

    MOZ_MUST_USE bool arrayPattern(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool objectPattern(NodeVector& elts, TokenPos* pos, MutableHandleValue dst);

    MOZ_MUST_USE bool propertyPattern(HandleValue key, HandleValue patt, bool isShorthand,
                                      TokenPos* pos, MutableHandleValue dst);
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

        if (!DefineDataElement(cx, array, i, val))
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
        return defineProperty(node, "loc", nullVal);
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
NodeBuilder::forInStatement(HandleValue var, HandleValue expr, HandleValue stmt,
                            TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_FOR_IN_STMT]);
    if (!cb.isNull()) {
        RootedValue isForEach(cx, JS::FalseValue());  // obsolete E4X `for each` statement
        return callback(cb, var, expr, stmt, isForEach, pos, dst);
    }

    return newNode(AST_FOR_IN_STMT, pos,
                   "left", var,
                   "right", expr,
                   "body", stmt,
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
NodeBuilder::tryStatement(HandleValue body, HandleValue handler,
                          HandleValue finally, TokenPos* pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_TRY_STMT]);
    if (!cb.isNull())
        return callback(cb, body, handler, opt(finally), pos, dst);

    return newNode(AST_TRY_STMT, pos,
                   "block", body,
                   "handler", handler,
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
NodeBuilder::catchClause(HandleValue var, HandleValue body, TokenPos* pos,
                         MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_CATCH]);
    if (!cb.isNull())
        return callback(cb, opt(var), body, pos, dst);

    return newNode(AST_CATCH, pos,
                   "param", var,
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
                      GeneratorStyle generatorStyle, bool isAsync, bool isExpression,
                      MutableHandleValue dst)
{
    RootedValue array(cx), defarray(cx);
    if (!newArray(args, &array))
        return false;
    if (!newArray(defaults, &defarray))
        return false;

    bool isGenerator = generatorStyle != GeneratorStyle::None;
    RootedValue isGeneratorVal(cx, BooleanValue(isGenerator));
    RootedValue isAsyncVal(cx, BooleanValue(isAsync));
    RootedValue isExpressionVal(cx, BooleanValue(isExpression));

    RootedValue cb(cx, callbacks[type]);
    if (!cb.isNull()) {
        return callback(cb, opt(id), array, body, isGeneratorVal, isExpressionVal, pos, dst);
    }

    if (isGenerator) {
        MOZ_ASSERT(generatorStyle == GeneratorStyle::ES6);
        JSAtom* styleStr = Atomize(cx, "es6", 3);
        if (!styleStr)
            return false;
        RootedValue styleVal(cx, StringValue(styleStr));
        return newNode(type, pos,
                       "id", id,
                       "params", array,
                       "defaults", defarray,
                       "body", body,
                       "rest", rest,
                       "generator", isGeneratorVal,
                       "async", isAsyncVal,
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
                   "async", isAsyncVal,
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
    Parser<FullParseHandler, char16_t>* parser;
    NodeBuilder         builder;
    DebugOnly<uint32_t> lineno;

    Value unrootedAtomContents(JSAtom* atom) {
        return StringValue(atom ? atom : cx->names().empty);
    }

    BinaryOperator binop(ParseNodeKind kind);
    UnaryOperator unop(ParseNodeKind kind);
    AssignmentOperator aop(ParseNodeKind kind);

    bool statements(ParseNode* pn, NodeVector& elts);
    bool expressions(ParseNode* pn, NodeVector& elts);
    bool leftAssociate(ParseNode* pn, MutableHandleValue dst);
    bool rightAssociate(ParseNode* pn, MutableHandleValue dst);
    bool functionArgs(ParseNode* pn, ParseNode* pnargs,
                      NodeVector& args, NodeVector& defaults, MutableHandleValue rest);

    bool sourceElement(ParseNode* pn, MutableHandleValue dst);

    bool declaration(ParseNode* pn, MutableHandleValue dst);
    bool variableDeclaration(ParseNode* pn, bool lexical, MutableHandleValue dst);
    bool variableDeclarator(ParseNode* pn, MutableHandleValue dst);
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
    bool catchClause(ParseNode* pn, MutableHandleValue dst);

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
    bool literal(ParseNode* pn, MutableHandleValue dst);

    bool optPattern(ParseNode* pn, MutableHandleValue dst) {
        if (!pn) {
            dst.setMagic(JS_SERIALIZE_NO_NODE);
            return true;
        }
        return pattern(pn, dst);
    }

    bool pattern(ParseNode* pn, MutableHandleValue dst);
    bool arrayPattern(ParseNode* pn, MutableHandleValue dst);
    bool objectPattern(ParseNode* pn, MutableHandleValue dst);

    bool function(ParseNode* pn, ASTType type, MutableHandleValue dst);
    bool functionArgsAndBody(ParseNode* pn, NodeVector& args, NodeVector& defaults,
                             bool isAsync, bool isExpression,
                             MutableHandleValue body, MutableHandleValue rest);
    bool functionBody(ParseNode* pn, TokenPos* pos, MutableHandleValue dst);

  public:
    ASTSerializer(JSContext* c, bool l, char const* src, uint32_t ln)
        : cx(c)
        , parser(nullptr)
        , builder(c, l, src)
#ifdef DEBUG
        , lineno(ln)
#endif
    {}

    bool init(HandleObject userobj) {
        return builder.init(userobj);
    }

    void setParser(Parser<FullParseHandler, char16_t>* p) {
        parser = p;
        builder.setTokenStream(&p->anyChars);
    }

    bool program(ParseNode* pn, MutableHandleValue dst);
};

} /* anonymous namespace */

AssignmentOperator
ASTSerializer::aop(ParseNodeKind kind)
{
    switch (kind) {
      case ParseNodeKind::Assign:
        return AOP_ASSIGN;
      case ParseNodeKind::AddAssign:
        return AOP_PLUS;
      case ParseNodeKind::SubAssign:
        return AOP_MINUS;
      case ParseNodeKind::MulAssign:
        return AOP_STAR;
      case ParseNodeKind::DivAssign:
        return AOP_DIV;
      case ParseNodeKind::ModAssign:
        return AOP_MOD;
      case ParseNodeKind::PowAssign:
        return AOP_POW;
      case ParseNodeKind::LshAssign:
        return AOP_LSH;
      case ParseNodeKind::RshAssign:
        return AOP_RSH;
      case ParseNodeKind::UrshAssign:
        return AOP_URSH;
      case ParseNodeKind::BitOrAssign:
        return AOP_BITOR;
      case ParseNodeKind::BitXorAssign:
        return AOP_BITXOR;
      case ParseNodeKind::BitAndAssign:
        return AOP_BITAND;
      default:
        return AOP_ERR;
    }
}

UnaryOperator
ASTSerializer::unop(ParseNodeKind kind)
{
    if (IsDeleteKind(kind))
        return UNOP_DELETE;

    if (IsTypeofKind(kind))
        return UNOP_TYPEOF;

    switch (kind) {
      case ParseNodeKind::Await:
        return UNOP_AWAIT;
      case ParseNodeKind::Neg:
        return UNOP_NEG;
      case ParseNodeKind::Pos:
        return UNOP_POS;
      case ParseNodeKind::Not:
        return UNOP_NOT;
      case ParseNodeKind::BitNot:
        return UNOP_BITNOT;
      case ParseNodeKind::Void:
        return UNOP_VOID;
      default:
        return UNOP_ERR;
    }
}

BinaryOperator
ASTSerializer::binop(ParseNodeKind kind)
{
    switch (kind) {
      case ParseNodeKind::Lsh:
        return BINOP_LSH;
      case ParseNodeKind::Rsh:
        return BINOP_RSH;
      case ParseNodeKind::Ursh:
        return BINOP_URSH;
      case ParseNodeKind::Lt:
        return BINOP_LT;
      case ParseNodeKind::Le:
        return BINOP_LE;
      case ParseNodeKind::Gt:
        return BINOP_GT;
      case ParseNodeKind::Ge:
        return BINOP_GE;
      case ParseNodeKind::Eq:
        return BINOP_EQ;
      case ParseNodeKind::Ne:
        return BINOP_NE;
      case ParseNodeKind::StrictEq:
        return BINOP_STRICTEQ;
      case ParseNodeKind::StrictNe:
        return BINOP_STRICTNE;
      case ParseNodeKind::Add:
        return BINOP_ADD;
      case ParseNodeKind::Sub:
        return BINOP_SUB;
      case ParseNodeKind::Star:
        return BINOP_STAR;
      case ParseNodeKind::Div:
        return BINOP_DIV;
      case ParseNodeKind::Mod:
        return BINOP_MOD;
      case ParseNodeKind::Pow:
        return BINOP_POW;
      case ParseNodeKind::BitOr:
        return BINOP_BITOR;
      case ParseNodeKind::BitXor:
        return BINOP_BITXOR;
      case ParseNodeKind::BitAnd:
        return BINOP_BITAND;
      case ParseNodeKind::In:
        return BINOP_IN;
      case ParseNodeKind::InstanceOf:
        return BINOP_INSTANCEOF;
      case ParseNodeKind::Pipeline:
        return BINOP_PIPELINE;
      default:
        return BINOP_ERR;
    }
}

bool
ASTSerializer::statements(ParseNode* pn, NodeVector& elts)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::StatementList));
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
    MOZ_ASSERT(pn->isKind(ParseNodeKind::StatementList));

    NodeVector stmts(cx);
    return statements(pn, stmts) &&
           builder.blockStatement(stmts, &pn->pn_pos, dst);
}

bool
ASTSerializer::program(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(parser->anyChars.srcCoords.lineNum(pn->pn_pos.begin) == lineno);

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
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Function) ||
               pn->isKind(ParseNodeKind::Var) ||
               pn->isKind(ParseNodeKind::Let) ||
               pn->isKind(ParseNodeKind::Const));

    switch (pn->getKind()) {
      case ParseNodeKind::Function:
        return function(pn, AST_FUNC_DECL, dst);

      case ParseNodeKind::Var:
        return variableDeclaration(pn, false, dst);

      default:
        MOZ_ASSERT(pn->isKind(ParseNodeKind::Let) || pn->isKind(ParseNodeKind::Const));
        return variableDeclaration(pn, true, dst);
    }
}

bool
ASTSerializer::variableDeclaration(ParseNode* pn, bool lexical, MutableHandleValue dst)
{
    MOZ_ASSERT_IF(lexical, pn->isKind(ParseNodeKind::Let) || pn->isKind(ParseNodeKind::Const));
    MOZ_ASSERT_IF(!lexical, pn->isKind(ParseNodeKind::Var));

    VarDeclKind kind = VARDECL_ERR;
    // Treat both the toplevel const binding (secretly var-like) and the lexical const
    // the same way
    if (lexical)
        kind = pn->isKind(ParseNodeKind::Let) ? VARDECL_LET : VARDECL_CONST;
    else
        kind = pn->isKind(ParseNodeKind::Var) ? VARDECL_VAR : VARDECL_CONST;

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

    if (pn->isKind(ParseNodeKind::Name)) {
        pnleft = pn;
        pnright = pn->pn_expr;
        MOZ_ASSERT_IF(pnright, pn->pn_pos.encloses(pnright->pn_pos));
    } else if (pn->isKind(ParseNodeKind::Assign)) {
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
ASTSerializer::importDeclaration(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Import));
    MOZ_ASSERT(pn->isArity(PN_BINARY));
    MOZ_ASSERT(pn->pn_left->isKind(ParseNodeKind::ImportSpecList));
    MOZ_ASSERT(pn->pn_right->isKind(ParseNodeKind::String));

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
    MOZ_ASSERT(pn->isKind(ParseNodeKind::ImportSpec));

    RootedValue importName(cx);
    RootedValue bindingName(cx);
    return identifier(pn->pn_left, &importName) &&
           identifier(pn->pn_right, &bindingName) &&
           builder.importSpecifier(importName, bindingName, &pn->pn_pos, dst);
}

bool
ASTSerializer::exportDeclaration(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Export) ||
               pn->isKind(ParseNodeKind::ExportFrom) ||
               pn->isKind(ParseNodeKind::ExportDefault));
    MOZ_ASSERT(pn->getArity() == (pn->isKind(ParseNodeKind::Export) ? PN_UNARY : PN_BINARY));
    MOZ_ASSERT_IF(pn->isKind(ParseNodeKind::ExportFrom), pn->pn_right->isKind(ParseNodeKind::String));

    RootedValue decl(cx, NullValue());
    NodeVector elts(cx);

    ParseNode* kid = pn->isKind(ParseNodeKind::Export) ? pn->pn_kid : pn->pn_left;
    switch (ParseNodeKind kind = kid->getKind()) {
      case ParseNodeKind::ExportSpecList:
        if (!elts.reserve(pn->pn_left->pn_count))
            return false;

        for (ParseNode* next = pn->pn_left->pn_head; next; next = next->pn_next) {
            RootedValue elt(cx);
            if (next->isKind(ParseNodeKind::ExportSpec)) {
                if (!exportSpecifier(next, &elt))
                    return false;
            } else {
                if (!builder.exportBatchSpecifier(&pn->pn_pos, &elt))
                    return false;
            }
            elts.infallibleAppend(elt);
        }
        break;

      case ParseNodeKind::Function:
        if (!function(kid, AST_FUNC_DECL, &decl))
            return false;
        break;

      case ParseNodeKind::Class:
        if (!classDefinition(kid, false, &decl))
            return false;
        break;

      case ParseNodeKind::Var:
      case ParseNodeKind::Const:
      case ParseNodeKind::Let:
        if (!variableDeclaration(kid, kind != ParseNodeKind::Var, &decl))
            return false;
        break;

      default:
          if (!expression(kid, &decl))
              return false;
          break;
    }

    RootedValue moduleSpec(cx, NullValue());
    if (pn->isKind(ParseNodeKind::ExportFrom) && !literal(pn->pn_right, &moduleSpec))
        return false;

    RootedValue isDefault(cx, BooleanValue(false));
    if (pn->isKind(ParseNodeKind::ExportDefault))
        isDefault.setBoolean(true);

    return builder.exportDeclaration(decl, elts, moduleSpec, isDefault, &pn->pn_pos, dst);
}

bool
ASTSerializer::exportSpecifier(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::ExportSpec));

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

    if (pn->pn_right->isKind(ParseNodeKind::LexicalScope)) {
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
ASTSerializer::catchClause(ParseNode* pn, MutableHandleValue dst)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Catch));
    MOZ_ASSERT_IF(pn->pn_left, pn->pn_pos.encloses(pn->pn_left->pn_pos));
    MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

    RootedValue var(cx), body(cx);

    if (!optPattern(pn->pn_left, &var))
        return false;

    return statement(pn->pn_right, &body) &&
           builder.catchClause(var, body, &pn->pn_pos, dst);
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

    RootedValue handler(cx, NullValue());
    if (ParseNode* catchScope = pn->pn_kid2) {
        MOZ_ASSERT(catchScope->isKind(ParseNodeKind::LexicalScope));
        if (!catchClause(catchScope->scopeBody(), &handler))
            return false;
    }

    RootedValue finally(cx);
    return optStatement(pn->pn_kid3, &finally) &&
           builder.tryStatement(body, handler, finally, &pn->pn_pos, dst);
}

bool
ASTSerializer::forInit(ParseNode* pn, MutableHandleValue dst)
{
    if (!pn) {
        dst.setMagic(JS_SERIALIZE_NO_NODE);
        return true;
    }

    bool lexical = pn->isKind(ParseNodeKind::Let) || pn->isKind(ParseNodeKind::Const);
    return (lexical || pn->isKind(ParseNodeKind::Var))
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

    return expression(head->pn_kid3, &expr) &&
        builder.forInStatement(var, expr, stmt, &loop->pn_pos, dst);
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
    if (!CheckRecursionLimit(cx))
        return false;

    switch (pn->getKind()) {
      case ParseNodeKind::Function:
      case ParseNodeKind::Var:
        return declaration(pn, dst);

      case ParseNodeKind::Let:
      case ParseNodeKind::Const:
        return declaration(pn, dst);

      case ParseNodeKind::Import:
        return importDeclaration(pn, dst);

      case ParseNodeKind::Export:
      case ParseNodeKind::ExportDefault:
      case ParseNodeKind::ExportFrom:
        return exportDeclaration(pn, dst);

      case ParseNodeKind::EmptyStatement:
        return builder.emptyStatement(&pn->pn_pos, dst);

      case ParseNodeKind::ExpressionStatement:
      {
        RootedValue expr(cx);
        return expression(pn->pn_kid, &expr) &&
            builder.expressionStatement(expr, &pn->pn_pos, dst);
      }

      case ParseNodeKind::LexicalScope:
        pn = pn->pn_expr;
        if (!pn->isKind(ParseNodeKind::StatementList))
            return statement(pn, dst);
        MOZ_FALLTHROUGH;

      case ParseNodeKind::StatementList:
        return blockStatement(pn, dst);

      case ParseNodeKind::If:
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

      case ParseNodeKind::Switch:
        return switchStatement(pn, dst);

      case ParseNodeKind::Try:
        return tryStatement(pn, dst);

      case ParseNodeKind::With:
      case ParseNodeKind::While:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        RootedValue expr(cx), stmt(cx);

        return expression(pn->pn_left, &expr) &&
               statement(pn->pn_right, &stmt) &&
               (pn->isKind(ParseNodeKind::With)
                ? builder.withStatement(expr, stmt, &pn->pn_pos, dst)
                : builder.whileStatement(expr, stmt, &pn->pn_pos, dst));
      }

      case ParseNodeKind::DoWhile:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        RootedValue stmt(cx), test(cx);

        return statement(pn->pn_left, &stmt) &&
               expression(pn->pn_right, &test) &&
               builder.doWhileStatement(stmt, test, &pn->pn_pos, dst);
      }

      case ParseNodeKind::For:
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

        if (head->isKind(ParseNodeKind::ForIn) || head->isKind(ParseNodeKind::ForOf)) {
            RootedValue var(cx);
            if (head->pn_kid1->isKind(ParseNodeKind::LexicalScope)) {
                if (!variableDeclaration(head->pn_kid1->pn_expr, true, &var))
                    return false;
            } else if (!head->pn_kid1->isKind(ParseNodeKind::Var) &&
                       !head->pn_kid1->isKind(ParseNodeKind::Let) &&
                       !head->pn_kid1->isKind(ParseNodeKind::Const))
            {
                if (!pattern(head->pn_kid1, &var))
                    return false;
            } else {
                if (!variableDeclaration(head->pn_kid1,
                                         head->pn_kid1->isKind(ParseNodeKind::Let) ||
                                         head->pn_kid1->isKind(ParseNodeKind::Const),
                                         &var))
                {
                    return false;
                }
            }
            if (head->isKind(ParseNodeKind::ForIn))
                return forIn(pn, head, var, stmt, dst);
            return forOf(pn, head, var, stmt, dst);
        }

        RootedValue init(cx), test(cx), update(cx);

        return forInit(head->pn_kid1, &init) &&
               optExpression(head->pn_kid2, &test) &&
               optExpression(head->pn_kid3, &update) &&
               builder.forStatement(init, test, update, stmt, &pn->pn_pos, dst);
      }

      case ParseNodeKind::Break:
      case ParseNodeKind::Continue:
      {
        RootedValue label(cx);
        RootedAtom pnAtom(cx, pn->pn_atom);
        return optIdentifier(pnAtom, nullptr, &label) &&
               (pn->isKind(ParseNodeKind::Break)
                ? builder.breakStatement(label, &pn->pn_pos, dst)
                : builder.continueStatement(label, &pn->pn_pos, dst));
      }

      case ParseNodeKind::Label:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_expr->pn_pos));

        RootedValue label(cx), stmt(cx);
        RootedAtom pnAtom(cx, pn->as<LabeledStatement>().label());
        return identifier(pnAtom, nullptr, &label) &&
               statement(pn->pn_expr, &stmt) &&
               builder.labeledStatement(label, stmt, &pn->pn_pos, dst);
      }

      case ParseNodeKind::Throw:
      {
        MOZ_ASSERT_IF(pn->pn_kid, pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        RootedValue arg(cx);

        return optExpression(pn->pn_kid, &arg) &&
               builder.throwStatement(arg, &pn->pn_pos, dst);
      }

      case ParseNodeKind::Return:
      {
        MOZ_ASSERT_IF(pn->pn_kid, pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        RootedValue arg(cx);

        return optExpression(pn->pn_kid, &arg) &&
               builder.returnStatement(arg, &pn->pn_pos, dst);
      }

      case ParseNodeKind::Debugger:
        return builder.debuggerStatement(&pn->pn_pos, dst);

      case ParseNodeKind::Class:
        return classDefinition(pn, false, dst);

      case ParseNodeKind::ClassMethodList:
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
    bool lor = kind == ParseNodeKind::Or;
    bool logop = lor || (kind == ParseNodeKind::And);

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
            BinaryOperator op = binop(pn->getKind());
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

        BinaryOperator op = binop(pn->getKind());
        LOCAL_ASSERT(op > BINOP_ERR && op < BINOP_LIMIT);

        if (!builder.binaryExpression(op, left, right, &subpos, &right))
            return false;
    }

    dst.set(right);
    return true;
}

bool
ASTSerializer::expression(ParseNode* pn, MutableHandleValue dst)
{
    if (!CheckRecursionLimit(cx))
        return false;

    switch (pn->getKind()) {
      case ParseNodeKind::Function:
      {
        ASTType type = pn->pn_funbox->function()->isArrow() ? AST_ARROW_EXPR : AST_FUNC_EXPR;
        return function(pn, type, dst);
      }

      case ParseNodeKind::Comma:
      {
        NodeVector exprs(cx);
        return expressions(pn, exprs) &&
               builder.sequenceExpression(exprs, &pn->pn_pos, dst);
      }

      case ParseNodeKind::Conditional:
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

      case ParseNodeKind::Or:
      case ParseNodeKind::And:
        return leftAssociate(pn, dst);

      case ParseNodeKind::PreIncrement:
      case ParseNodeKind::PreDecrement:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        bool inc = pn->isKind(ParseNodeKind::PreIncrement);
        RootedValue expr(cx);
        return expression(pn->pn_kid, &expr) &&
               builder.updateExpression(expr, inc, true, &pn->pn_pos, dst);
      }

      case ParseNodeKind::PostIncrement:
      case ParseNodeKind::PostDecrement:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        bool inc = pn->isKind(ParseNodeKind::PostIncrement);
        RootedValue expr(cx);
        return expression(pn->pn_kid, &expr) &&
               builder.updateExpression(expr, inc, false, &pn->pn_pos, dst);
      }

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
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        AssignmentOperator op = aop(pn->getKind());
        LOCAL_ASSERT(op > AOP_ERR && op < AOP_LIMIT);

        RootedValue lhs(cx), rhs(cx);
        return pattern(pn->pn_left, &lhs) &&
               expression(pn->pn_right, &rhs) &&
               builder.assignmentExpression(op, lhs, rhs, &pn->pn_pos, dst);
      }

      case ParseNodeKind::Pipeline:
      case ParseNodeKind::Add:
      case ParseNodeKind::Sub:
      case ParseNodeKind::StrictEq:
      case ParseNodeKind::Eq:
      case ParseNodeKind::StrictNe:
      case ParseNodeKind::Ne:
      case ParseNodeKind::Lt:
      case ParseNodeKind::Le:
      case ParseNodeKind::Gt:
      case ParseNodeKind::Ge:
      case ParseNodeKind::Lsh:
      case ParseNodeKind::Rsh:
      case ParseNodeKind::Ursh:
      case ParseNodeKind::Star:
      case ParseNodeKind::Div:
      case ParseNodeKind::Mod:
      case ParseNodeKind::BitOr:
      case ParseNodeKind::BitXor:
      case ParseNodeKind::BitAnd:
      case ParseNodeKind::In:
      case ParseNodeKind::InstanceOf:
        return leftAssociate(pn, dst);

      case ParseNodeKind::Pow:
        return rightAssociate(pn, dst);

      case ParseNodeKind::DeleteName:
      case ParseNodeKind::DeleteProp:
      case ParseNodeKind::DeleteElem:
      case ParseNodeKind::DeleteExpr:
      case ParseNodeKind::TypeOfName:
      case ParseNodeKind::TypeOfExpr:
      case ParseNodeKind::Void:
      case ParseNodeKind::Not:
      case ParseNodeKind::BitNot:
      case ParseNodeKind::Pos:
      case ParseNodeKind::Await:
      case ParseNodeKind::Neg: {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        UnaryOperator op = unop(pn->getKind());
        LOCAL_ASSERT(op > UNOP_ERR && op < UNOP_LIMIT);

        RootedValue expr(cx);
        return expression(pn->pn_kid, &expr) &&
               builder.unaryExpression(op, expr, &pn->pn_pos, dst);
      }

      case ParseNodeKind::New:
      case ParseNodeKind::TaggedTemplate:
      case ParseNodeKind::Call:
      case ParseNodeKind::SuperCall:
      {
        ParseNode* next = pn->pn_head;
        MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

        RootedValue callee(cx);
        if (pn->isKind(ParseNodeKind::SuperCall)) {
            MOZ_ASSERT(next->isKind(ParseNodeKind::SuperBase));
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

        if (pn->getKind() == ParseNodeKind::TaggedTemplate)
            return builder.taggedTemplate(callee, args, &pn->pn_pos, dst);

        // SUPERCALL is Call(super, args)
        return pn->isKind(ParseNodeKind::New)
               ? builder.newExpression(callee, args, &pn->pn_pos, dst)

            : builder.callExpression(callee, args, &pn->pn_pos, dst);
      }

      case ParseNodeKind::Dot:
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

      case ParseNodeKind::Elem:
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

      case ParseNodeKind::CallSiteObj:
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
            if (next->isKind(ParseNodeKind::RawUndefined)) {
                expr.setUndefined();
            } else {
                MOZ_ASSERT(next->isKind(ParseNodeKind::TemplateString));
                expr.setString(next->pn_atom);
            }
            cooked.infallibleAppend(expr);
        }

        return builder.callSiteObj(raw, cooked, &pn->pn_pos, dst);
      }

      case ParseNodeKind::Array:
      {
        NodeVector elts(cx);
        if (!elts.reserve(pn->pn_count))
            return false;

        for (ParseNode* next = pn->pn_head; next; next = next->pn_next) {
            MOZ_ASSERT(pn->pn_pos.encloses(next->pn_pos));

            if (next->isKind(ParseNodeKind::Elision)) {
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

      case ParseNodeKind::Spread:
      {
          RootedValue expr(cx);
          return expression(pn->pn_kid, &expr) &&
                 builder.spreadExpression(expr, &pn->pn_pos, dst);
      }

      case ParseNodeKind::ComputedName:
      {
         RootedValue name(cx);
         return expression(pn->pn_kid, &name) &&
                builder.computedName(name, &pn->pn_pos, dst);
      }

      case ParseNodeKind::Object:
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

      case ParseNodeKind::Name:
        return identifier(pn, dst);

      case ParseNodeKind::This:
        return builder.thisExpression(&pn->pn_pos, dst);

      case ParseNodeKind::TemplateStringList:
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

      case ParseNodeKind::TemplateString:
      case ParseNodeKind::String:
      case ParseNodeKind::RegExp:
      case ParseNodeKind::Number:
      case ParseNodeKind::True:
      case ParseNodeKind::False:
      case ParseNodeKind::Null:
      case ParseNodeKind::RawUndefined:
        return literal(pn, dst);

      case ParseNodeKind::YieldStar:
      {
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        RootedValue arg(cx);
        return expression(pn->pn_kid, &arg) &&
               builder.yieldExpression(arg, Delegating, &pn->pn_pos, dst);
      }

      case ParseNodeKind::Yield:
      {
        MOZ_ASSERT_IF(pn->pn_kid, pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        RootedValue arg(cx);
        return optExpression(pn->pn_kid, &arg) &&
               builder.yieldExpression(arg, NotDelegating, &pn->pn_pos, dst);
      }

      case ParseNodeKind::Class:
        return classDefinition(pn, true, dst);

      case ParseNodeKind::NewTarget:
      {
        MOZ_ASSERT(pn->pn_left->isKind(ParseNodeKind::PosHolder));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        MOZ_ASSERT(pn->pn_right->isKind(ParseNodeKind::PosHolder));
        MOZ_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        RootedValue newIdent(cx);
        RootedValue targetIdent(cx);

        RootedAtom newStr(cx, cx->names().new_);
        RootedAtom targetStr(cx, cx->names().target);

        return identifier(newStr, &pn->pn_left->pn_pos, &newIdent) &&
               identifier(targetStr, &pn->pn_right->pn_pos, &targetIdent) &&
               builder.metaProperty(newIdent, targetIdent, &pn->pn_pos, dst);
      }

      case ParseNodeKind::SetThis:
        // SETTHIS is used to assign the result of a super() call to |this|.
        // It's not part of the original AST, so just forward to the call.
        MOZ_ASSERT(pn->pn_left->isKind(ParseNodeKind::Name));
        return expression(pn->pn_right, dst);

      default:
        LOCAL_NOT_REACHED("unexpected expression type");
    }
}

bool
ASTSerializer::propertyName(ParseNode* pn, MutableHandleValue dst)
{
    if (pn->isKind(ParseNodeKind::ComputedName))
        return expression(pn, dst);
    if (pn->isKind(ParseNodeKind::ObjectPropertyName))
        return identifier(pn, dst);

    LOCAL_ASSERT(pn->isKind(ParseNodeKind::String) || pn->isKind(ParseNodeKind::Number));

    return literal(pn, dst);
}

bool
ASTSerializer::property(ParseNode* pn, MutableHandleValue dst)
{
    if (pn->isKind(ParseNodeKind::MutateProto)) {
        RootedValue val(cx);
        return expression(pn->pn_kid, &val) &&
               builder.prototypeMutation(val, &pn->pn_pos, dst);
    }
    if (pn->isKind(ParseNodeKind::Spread))
        return expression(pn, dst);

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

    bool isShorthand = pn->isKind(ParseNodeKind::Shorthand);
    bool isMethod =
        pn->pn_right->isKind(ParseNodeKind::Function) &&
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
      case ParseNodeKind::TemplateString:
      case ParseNodeKind::String:
        val.setString(pn->pn_atom);
        break;

      case ParseNodeKind::RegExp:
      {
        RootedObject re1(cx, pn->as<RegExpLiteral>().objbox()->object);
        LOCAL_ASSERT(re1 && re1->is<RegExpObject>());

        RootedObject re2(cx, CloneRegExpObject(cx, re1.as<RegExpObject>()));
        if (!re2)
            return false;

        val.setObject(*re2);
        break;
      }

      case ParseNodeKind::Number:
        val.setNumber(pn->pn_dval);
        break;

      case ParseNodeKind::Null:
        val.setNull();
        break;

      case ParseNodeKind::RawUndefined:
        val.setUndefined();
        break;

      case ParseNodeKind::True:
        val.setBoolean(true);
        break;

      case ParseNodeKind::False:
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
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Array));

    NodeVector elts(cx);
    if (!elts.reserve(pn->pn_count))
        return false;

    for (ParseNode* next = pn->pn_head; next; next = next->pn_next) {
        if (next->isKind(ParseNodeKind::Elision)) {
            elts.infallibleAppend(NullValue());
        } else if (next->isKind(ParseNodeKind::Spread)) {
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
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Object));

    NodeVector elts(cx);
    if (!elts.reserve(pn->pn_count))
        return false;

    for (ParseNode* propdef = pn->pn_head; propdef; propdef = propdef->pn_next) {
        if (propdef->isKind(ParseNodeKind::Spread)) {
            RootedValue target(cx);
            RootedValue spread(cx);
            if (!pattern(propdef->pn_kid, &target))
                return false;
            if(!builder.spreadExpression(target, &propdef->pn_pos, &spread))
                return false;
            elts.infallibleAppend(spread);
            continue;
        }
        LOCAL_ASSERT(propdef->isKind(ParseNodeKind::MutateProto) != propdef->isOp(JSOP_INITPROP));

        RootedValue key(cx);
        ParseNode* target;
        if (propdef->isKind(ParseNodeKind::MutateProto)) {
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
            !builder.propertyPattern(key, patt, propdef->isKind(ParseNodeKind::Shorthand),
                                     &propdef->pn_pos,
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
    if (!CheckRecursionLimit(cx))
        return false;

    switch (pn->getKind()) {
      case ParseNodeKind::Object:
        return objectPattern(pn, dst);

      case ParseNodeKind::Array:
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
ASTSerializer::function(ParseNode* pn, ASTType type, MutableHandleValue dst)
{
    RootedFunction func(cx, pn->pn_funbox->function());

    GeneratorStyle generatorStyle =
        pn->pn_funbox->isGenerator()
        ? GeneratorStyle::ES6
        : GeneratorStyle::None;

    bool isAsync = pn->pn_funbox->isAsync();
    bool isExpression = pn->pn_funbox->isExprBody();

    RootedValue id(cx);
    RootedAtom funcAtom(cx, func->explicitName());
    if (!optIdentifier(funcAtom, nullptr, &id))
        return false;

    NodeVector args(cx);
    NodeVector defaults(cx);

    RootedValue body(cx), rest(cx);
    if (pn->pn_funbox->hasRest())
        rest.setUndefined();
    else
        rest.setNull();
    return functionArgsAndBody(pn->pn_body, args, defaults, isAsync, isExpression, &body, &rest) &&
           builder.function(type, &pn->pn_pos, id, args, defaults, body,
                            rest, generatorStyle, isAsync, isExpression, dst);
}

bool
ASTSerializer::functionArgsAndBody(ParseNode* pn, NodeVector& args, NodeVector& defaults,
                                   bool isAsync, bool isExpression,
                                   MutableHandleValue body, MutableHandleValue rest)
{
    ParseNode* pnargs;
    ParseNode* pnbody;

    /* Extract the args and body separately. */
    if (pn->isKind(ParseNodeKind::ParamsBody)) {
        pnargs = pn;
        pnbody = pn->last();
    } else {
        pnargs = nullptr;
        pnbody = pn;
    }

    if (pnbody->isKind(ParseNodeKind::LexicalScope))
        pnbody = pnbody->scopeBody();

    /* Serialize the arguments and body. */
    switch (pnbody->getKind()) {
      case ParseNodeKind::Return: /* expression closure, no destructured args */
        return functionArgs(pn, pnargs, args, defaults, rest) &&
               expression(pnbody->pn_kid, body);

      case ParseNodeKind::StatementList:     /* statement closure */
      {
        ParseNode* pnstart = pnbody->pn_head;

        // Skip over initial yield in generator.
        if (pnstart && pnstart->isKind(ParseNodeKind::InitialYield))
            pnstart = pnstart->pn_next;

        // Async arrow with expression body is converted into STATEMENTLIST
        // to insert initial yield.
        if (isAsync && isExpression) {
            MOZ_ASSERT(pnstart->getKind() == ParseNodeKind::Return);
            return functionArgs(pn, pnargs, args, defaults, rest) &&
                   expression(pnstart->pn_kid, body);
        }

        return functionArgs(pn, pnargs, args, defaults, rest) &&
               functionBody(pnstart, &pnbody->pn_pos, body);
      }

      default:
        LOCAL_NOT_REACHED("unexpected function contents");
    }
}

bool
ASTSerializer::functionArgs(ParseNode* pn, ParseNode* pnargs,
                            NodeVector& args, NodeVector& defaults,
                            MutableHandleValue rest)
{
    if (!pnargs)
        return true;

    RootedValue node(cx);
    bool defaultsNull = true;
    MOZ_ASSERT(defaults.empty(),
               "must be initially empty for it to be proper to clear this "
               "when there are no defaults");

    for (ParseNode* arg = pnargs->pn_head; arg && arg != pnargs->last(); arg = arg->pn_next) {
        ParseNode* pat;
        ParseNode* defNode;
        if (arg->isKind(ParseNodeKind::Name) ||
            arg->isKind(ParseNodeKind::Array) ||
            arg->isKind(ParseNodeKind::Object))
        {
            pat = arg;
            defNode = nullptr;
        } else {
            MOZ_ASSERT(arg->isKind(ParseNodeKind::Assign));
            pat = arg->pn_left;
            defNode = arg->pn_right;
        }

        // Process the name or pattern.
        MOZ_ASSERT(pat->isKind(ParseNodeKind::Name) ||
                   pat->isKind(ParseNodeKind::Array) ||
                   pat->isKind(ParseNodeKind::Object));
        if (!pattern(pat, &node))
            return false;
        if (rest.isUndefined() && arg->pn_next == pnargs->last()) {
            rest.setObject(node.toObject());
        } else {
            if (!args.append(node))
                return false;
        }

        // Process its default (or lack thereof).
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
    MOZ_ASSERT(!rest.isUndefined(),
               "if a rest argument was present (signified by "
               "|rest.isUndefined()| initially), the rest node was properly "
               "recorded");

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
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
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
            JS_ReportErrorASCII(cx, "Bad target value, expected 'script' or 'module'");
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
    options.allowHTMLComments = target == ParseTarget::Script;
    mozilla::Range<const char16_t> chars = linearChars.twoByteRange();
    UsedNameTracker usedNames(cx);
    if (!usedNames.init())
        return false;
    Parser<FullParseHandler, char16_t> parser(cx, cx->tempLifoAlloc(), options,
                                              chars.begin().get(), chars.length(),
                                              /* foldConstants = */ false, usedNames, nullptr,
                                              nullptr);
    if (!parser.checkOptions())
        return false;

    serialize.setParser(&parser);

    ParseNode* pn;
    if (target == ParseTarget::Script) {
        pn = parser.parse();
        if (!pn)
            return false;
    } else {
        if (!GlobalObject::ensureModulePrototypesCreated(cx, cx->global()))
            return false;

        Rooted<ModuleObject*> module(cx, ModuleObject::create(cx));
        if (!module)
            return false;

        ModuleBuilder builder(cx, module, parser.anyChars);
        if (!builder.init())
            return false;

        ModuleSharedContext modulesc(cx, module, &cx->global()->emptyGlobalScope(), builder);
        pn = parser.moduleBody(&modulesc);
        if (!pn)
            return false;

        MOZ_ASSERT(pn->getKind() == ParseNodeKind::Module);
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
        JS_ReportErrorASCII(cx, "JS_InitReflectParse must be called during global initialization");
        return false;
    }

    RootedObject reflectObj(cx, &reflectVal.toObject());
    return JS_DefineFunction(cx, reflectObj, "parse", reflect_parse, 1, 0);
}
