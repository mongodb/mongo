/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS reflection package. */

#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"

#include <stdlib.h>
#include <utility>

#include "jspubtd.h"

#include "builtin/Array.h"
#include "builtin/Reflect.h"
#include "frontend/CompilationStencil.h"
#include "frontend/ModuleSharedContext.h"
#include "frontend/ParseNode.h"
#include "frontend/Parser.h"
#include "js/CharacterEncoding.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "js/StableStringChars.h"
#include "vm/BigIntType.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/JSAtom.h"
#include "vm/JSObject.h"
#include "vm/ModuleBuilder.h"  // js::ModuleBuilder
#include "vm/PlainObject.h"    // js::PlainObject
#include "vm/RegExpObject.h"

#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::frontend;

using JS::AutoStableStringChars;
using JS::CompileOptions;
using JS::RootedValueArray;
using mozilla::DebugOnly;

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
  AOP_PLUS,
  AOP_MINUS,
  AOP_STAR,
  AOP_DIV,
  AOP_MOD,
  AOP_POW,
  /* shift-assign */
  AOP_LSH,
  AOP_RSH,
  AOP_URSH,
  /* binary */
  AOP_BITOR,
  AOP_BITXOR,
  AOP_BITAND,
  /* short-circuit */
  AOP_COALESCE,
  AOP_OR,
  AOP_AND,

  AOP_LIMIT
};

enum BinaryOperator {
  BINOP_ERR = -1,

  /* eq */
  BINOP_EQ = 0,
  BINOP_NE,
  BINOP_STRICTEQ,
  BINOP_STRICTNE,
  /* rel */
  BINOP_LT,
  BINOP_LE,
  BINOP_GT,
  BINOP_GE,
  /* shift */
  BINOP_LSH,
  BINOP_RSH,
  BINOP_URSH,
  /* arithmetic */
  BINOP_ADD,
  BINOP_SUB,
  BINOP_STAR,
  BINOP_DIV,
  BINOP_MOD,
  BINOP_POW,
  /* binary */
  BINOP_BITOR,
  BINOP_BITXOR,
  BINOP_BITAND,
  /* misc */
  BINOP_IN,
  BINOP_INSTANCEOF,
  BINOP_COALESCE,

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
    "=",     /* AOP_ASSIGN */
    "+=",    /* AOP_PLUS */
    "-=",    /* AOP_MINUS */
    "*=",    /* AOP_STAR */
    "/=",    /* AOP_DIV */
    "%=",    /* AOP_MOD */
    "**=",   /* AOP_POW */
    "<<=",   /* AOP_LSH */
    ">>=",   /* AOP_RSH */
    ">>>=",  /* AOP_URSH */
    "|=",    /* AOP_BITOR */
    "^=",    /* AOP_BITXOR */
    "&=",    /* AOP_BITAND */
    "\?\?=", /* AOP_COALESCE */
    "||=",   /* AOP_OR */
    "&&=",   /* AOP_AND */
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
    "??",         /* BINOP_COALESCE */
};

static const char* const unopNames[] = {
    "delete", /* UNOP_DELETE */
    "-",      /* UNOP_NEG */
    "+",      /* UNOP_POS */
    "!",      /* UNOP_NOT */
    "~",      /* UNOP_BITNOT */
    "typeof", /* UNOP_TYPEOF */
    "void",   /* UNOP_VOID */
    "await",  /* UNOP_AWAIT */
};

static const char* const nodeTypeNames[] = {
#define ASTDEF(ast, str, method) str,
#include "jsast.tbl"
#undef ASTDEF
    nullptr};

static const char* const callbackNames[] = {
#define ASTDEF(ast, str, method) method,
#include "jsast.tbl"
#undef ASTDEF
    nullptr};

enum YieldKind { Delegating, NotDelegating };

using NodeVector = RootedValueVector;

/*
 * ParseNode is a somewhat intricate data structure, and its invariants have
 * evolved, making it more likely that there could be a disconnect between the
 * parser and the AST serializer. We use these macros to check invariants on a
 * parse node and raise a dynamic error on failure.
 */
#define LOCAL_ASSERT(expr)                                    \
  JS_BEGIN_MACRO                                              \
    MOZ_ASSERT(expr);                                         \
    if (!(expr)) {                                            \
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, \
                                JSMSG_BAD_PARSE_NODE);        \
      return false;                                           \
    }                                                         \
  JS_END_MACRO

#define LOCAL_NOT_REACHED(expr)                             \
  JS_BEGIN_MACRO                                            \
    MOZ_ASSERT(false);                                      \
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, \
                              JSMSG_BAD_PARSE_NODE);        \
    return false;                                           \
  JS_END_MACRO

namespace {

/* Set 'result' to obj[id] if any such property exists, else defaultValue. */
static bool GetPropertyDefault(JSContext* cx, HandleObject obj, HandleId id,
                               HandleValue defaultValue,
                               MutableHandleValue result) {
  bool found;
  if (!HasProperty(cx, obj, id, &found)) {
    return false;
  }
  if (!found) {
    result.set(defaultValue);
    return true;
  }
  return GetProperty(cx, obj, obj, id, result);
}

enum class GeneratorStyle { None, ES6 };

/*
 * Builder class that constructs JavaScript AST node objects. See:
 *
 *     https://developer.mozilla.org/en/SpiderMonkey/Parser_API
 *
 * Bug 569487: generalize builder interface
 */
class NodeBuilder {
  using CallbackArray = RootedValueArray<AST_LIMIT>;

  JSContext* cx;
  frontend::Parser<frontend::FullParseHandler, char16_t>* parser;
  bool saveLoc;            /* save source location information?     */
  char const* src;         /* source filename or null               */
  RootedValue srcval;      /* source filename JS value or null      */
  CallbackArray callbacks; /* user-specified callbacks              */
  RootedValue userv;       /* user-specified builder object or null */

 public:
  NodeBuilder(JSContext* c, bool l, char const* s)
      : cx(c),
        parser(nullptr),
        saveLoc(l),
        src(s),
        srcval(c),
        callbacks(cx),
        userv(c) {}

  [[nodiscard]] bool init(HandleObject userobj = nullptr) {
    if (src) {
      if (!atomValue(src, &srcval)) {
        return false;
      }
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
      if (!atom) {
        return false;
      }
      RootedId id(cx, AtomToId(atom));
      if (!GetPropertyDefault(cx, userobj, id, nullVal, &funv)) {
        return false;
      }

      if (funv.isNullOrUndefined()) {
        callbacks[i].setNull();
        continue;
      }

      if (!funv.isObject() || !funv.toObject().is<JSFunction>()) {
        ReportValueError(cx, JSMSG_NOT_FUNCTION, JSDVG_SEARCH_STACK, funv,
                         nullptr);
        return false;
      }

      callbacks[i].set(funv);
    }

    return true;
  }

  void setParser(frontend::Parser<frontend::FullParseHandler, char16_t>* p) {
    parser = p;
  }

 private:
  [[nodiscard]] bool callbackHelper(HandleValue fun, const InvokeArgs& args,
                                    size_t i, TokenPos* pos,
                                    MutableHandleValue dst) {
    // The end of the implementation of callback(). All arguments except
    // loc have already been stored in range [0, i).
    if (saveLoc) {
      if (!newNodeLoc(pos, args[i])) {
        return false;
      }
    }

    return js::Call(cx, fun, userv, args, dst);
  }

  // Helper function for callback(). Note that all Arguments must be types
  // that convert to HandleValue, so this isn't as template-y as it seems,
  // just variadic.
  template <typename... Arguments>
  [[nodiscard]] bool callbackHelper(HandleValue fun, const InvokeArgs& args,
                                    size_t i, HandleValue head,
                                    Arguments&&... tail) {
    // Recursive loop to store the arguments into args. This eventually
    // bottoms out in a call to the non-template callbackHelper() above.
    args[i].set(head);
    return callbackHelper(fun, args, i + 1, std::forward<Arguments>(tail)...);
  }

  // Invoke a user-defined callback. The actual signature is:
  //
  //     bool callback(HandleValue fun, HandleValue... args, TokenPos* pos,
  //                   MutableHandleValue dst);
  template <typename... Arguments>
  [[nodiscard]] bool callback(HandleValue fun, Arguments&&... args) {
    InvokeArgs iargs(cx);
    if (!iargs.init(cx, sizeof...(args) - 2 + size_t(saveLoc))) {
      return false;
    }

    return callbackHelper(fun, iargs, 0, std::forward<Arguments>(args)...);
  }

  // WARNING: Returning a Handle is non-standard, but it works in this case
  // because both |v| and |UndefinedHandleValue| are definitely rooted on a
  // previous stack frame (i.e. we're just choosing between two
  // already-rooted values).
  HandleValue opt(HandleValue v) {
    MOZ_ASSERT_IF(v.isMagic(), v.whyMagic() == JS_SERIALIZE_NO_NODE);
    return v.isMagic(JS_SERIALIZE_NO_NODE) ? JS::UndefinedHandleValue : v;
  }

  [[nodiscard]] bool atomValue(const char* s, MutableHandleValue dst) {
    /*
     * Bug 575416: instead of Atomize, lookup constant atoms in tbl file
     */
    RootedAtom atom(cx, Atomize(cx, s, strlen(s)));
    if (!atom) {
      return false;
    }

    dst.setString(atom);
    return true;
  }

  [[nodiscard]] bool newObject(MutableHandleObject dst) {
    RootedPlainObject nobj(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!nobj) {
      return false;
    }

    dst.set(nobj);
    return true;
  }

  [[nodiscard]] bool newArray(NodeVector& elts, MutableHandleValue dst);

  [[nodiscard]] bool createNode(ASTType type, TokenPos* pos,
                                MutableHandleObject dst);

  [[nodiscard]] bool newNodeHelper(HandleObject obj, MutableHandleValue dst) {
    // The end of the implementation of newNode().
    MOZ_ASSERT(obj);
    dst.setObject(*obj);
    return true;
  }

  template <typename... Arguments>
  [[nodiscard]] bool newNodeHelper(HandleObject obj, const char* name,
                                   HandleValue value, Arguments&&... rest) {
    // Recursive loop to define properties. Note that the newNodeHelper()
    // call below passes two fewer arguments than we received, as we omit
    // `name` and `value`. This eventually bottoms out in a call to the
    // non-template newNodeHelper() above.
    return defineProperty(obj, name, value) &&
           newNodeHelper(obj, std::forward<Arguments>(rest)...);
  }

  // Create a node object with "type" and "loc" properties, as well as zero
  // or more properties passed in as arguments. The signature is really more
  // like:
  //
  //     bool newNode(ASTType type, TokenPos* pos,
  //                  {const char *name0, HandleValue value0,}...
  //                  MutableHandleValue dst);
  template <typename... Arguments>
  [[nodiscard]] bool newNode(ASTType type, TokenPos* pos, Arguments&&... args) {
    RootedObject node(cx);
    return createNode(type, pos, &node) &&
           newNodeHelper(node, std::forward<Arguments>(args)...);
  }

  [[nodiscard]] bool listNode(ASTType type, const char* propName,
                              NodeVector& elts, TokenPos* pos,
                              MutableHandleValue dst) {
    RootedValue array(cx);
    if (!newArray(elts, &array)) {
      return false;
    }

    RootedValue cb(cx, callbacks[type]);
    if (!cb.isNull()) {
      return callback(cb, array, pos, dst);
    }

    return newNode(type, pos, propName, array, dst);
  }

  [[nodiscard]] bool defineProperty(HandleObject obj, const char* name,
                                    HandleValue val) {
    MOZ_ASSERT_IF(val.isMagic(), val.whyMagic() == JS_SERIALIZE_NO_NODE);

    /*
     * Bug 575416: instead of Atomize, lookup constant atoms in tbl file
     */
    RootedAtom atom(cx, Atomize(cx, name, strlen(name)));
    if (!atom) {
      return false;
    }

    // Represent "no node" as null and ensure users are not exposed to magic
    // values.
    RootedValue optVal(cx,
                       val.isMagic(JS_SERIALIZE_NO_NODE) ? NullValue() : val);
    return DefineDataProperty(cx, obj, atom->asPropertyName(), optVal);
  }

  [[nodiscard]] bool newNodeLoc(TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool setNodeLoc(HandleObject node, TokenPos* pos);

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

  [[nodiscard]] bool program(NodeVector& elts, TokenPos* pos,
                             MutableHandleValue dst);

  [[nodiscard]] bool literal(HandleValue val, TokenPos* pos,
                             MutableHandleValue dst);

  [[nodiscard]] bool identifier(HandleValue name, TokenPos* pos,
                                MutableHandleValue dst);

  [[nodiscard]] bool function(ASTType type, TokenPos* pos, HandleValue id,
                              NodeVector& args, NodeVector& defaults,
                              HandleValue body, HandleValue rest,
                              GeneratorStyle generatorStyle, bool isAsync,
                              bool isExpression, MutableHandleValue dst);

  [[nodiscard]] bool variableDeclarator(HandleValue id, HandleValue init,
                                        TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool switchCase(HandleValue expr, NodeVector& elts,
                                TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool catchClause(HandleValue var, HandleValue body,
                                 TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool prototypeMutation(HandleValue val, TokenPos* pos,
                                       MutableHandleValue dst);
  [[nodiscard]] bool propertyInitializer(HandleValue key, HandleValue val,
                                         PropKind kind, bool isShorthand,
                                         bool isMethod, TokenPos* pos,
                                         MutableHandleValue dst);

  /*
   * statements
   */

  [[nodiscard]] bool blockStatement(NodeVector& elts, TokenPos* pos,
                                    MutableHandleValue dst);

  [[nodiscard]] bool expressionStatement(HandleValue expr, TokenPos* pos,
                                         MutableHandleValue dst);

  [[nodiscard]] bool emptyStatement(TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool ifStatement(HandleValue test, HandleValue cons,
                                 HandleValue alt, TokenPos* pos,
                                 MutableHandleValue dst);

  [[nodiscard]] bool breakStatement(HandleValue label, TokenPos* pos,
                                    MutableHandleValue dst);

  [[nodiscard]] bool continueStatement(HandleValue label, TokenPos* pos,
                                       MutableHandleValue dst);

  [[nodiscard]] bool labeledStatement(HandleValue label, HandleValue stmt,
                                      TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool throwStatement(HandleValue arg, TokenPos* pos,
                                    MutableHandleValue dst);

  [[nodiscard]] bool returnStatement(HandleValue arg, TokenPos* pos,
                                     MutableHandleValue dst);

  [[nodiscard]] bool forStatement(HandleValue init, HandleValue test,
                                  HandleValue update, HandleValue stmt,
                                  TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool forInStatement(HandleValue var, HandleValue expr,
                                    HandleValue stmt, TokenPos* pos,
                                    MutableHandleValue dst);

  [[nodiscard]] bool forOfStatement(HandleValue var, HandleValue expr,
                                    HandleValue stmt, TokenPos* pos,
                                    MutableHandleValue dst);

  [[nodiscard]] bool withStatement(HandleValue expr, HandleValue stmt,
                                   TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool whileStatement(HandleValue test, HandleValue stmt,
                                    TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool doWhileStatement(HandleValue stmt, HandleValue test,
                                      TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool switchStatement(HandleValue disc, NodeVector& elts,
                                     bool lexical, TokenPos* pos,
                                     MutableHandleValue dst);

  [[nodiscard]] bool tryStatement(HandleValue body, HandleValue handler,
                                  HandleValue finally, TokenPos* pos,
                                  MutableHandleValue dst);

  [[nodiscard]] bool debuggerStatement(TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool importDeclaration(NodeVector& elts, HandleValue moduleSpec,
                                       TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool importSpecifier(HandleValue importName,
                                     HandleValue bindingName, TokenPos* pos,
                                     MutableHandleValue dst);

  [[nodiscard]] bool importNamespaceSpecifier(HandleValue bindingName,
                                              TokenPos* pos,
                                              MutableHandleValue dst);

  [[nodiscard]] bool exportDeclaration(HandleValue decl, NodeVector& elts,
                                       HandleValue moduleSpec,
                                       HandleValue isDefault, TokenPos* pos,
                                       MutableHandleValue dst);

  [[nodiscard]] bool exportSpecifier(HandleValue bindingName,
                                     HandleValue exportName, TokenPos* pos,
                                     MutableHandleValue dst);

  [[nodiscard]] bool exportNamespaceSpecifier(HandleValue exportName,
                                              TokenPos* pos,
                                              MutableHandleValue dst);

  [[nodiscard]] bool exportBatchSpecifier(TokenPos* pos,
                                          MutableHandleValue dst);

  [[nodiscard]] bool classDefinition(bool expr, HandleValue name,
                                     HandleValue heritage, HandleValue block,
                                     TokenPos* pos, MutableHandleValue dst);
  [[nodiscard]] bool classMembers(NodeVector& members, MutableHandleValue dst);
  [[nodiscard]] bool classMethod(HandleValue name, HandleValue body,
                                 PropKind kind, bool isStatic, TokenPos* pos,
                                 MutableHandleValue dst);
  [[nodiscard]] bool classField(HandleValue name, HandleValue initializer,
                                TokenPos* pos, MutableHandleValue dst);
  [[nodiscard]] bool staticClassBlock(HandleValue body, TokenPos* pos,
                                      MutableHandleValue dst);

  /*
   * expressions
   */

  [[nodiscard]] bool binaryExpression(BinaryOperator op, HandleValue left,
                                      HandleValue right, TokenPos* pos,
                                      MutableHandleValue dst);

  [[nodiscard]] bool unaryExpression(UnaryOperator op, HandleValue expr,
                                     TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool assignmentExpression(AssignmentOperator op,
                                          HandleValue lhs, HandleValue rhs,
                                          TokenPos* pos,
                                          MutableHandleValue dst);

  [[nodiscard]] bool updateExpression(HandleValue expr, bool incr, bool prefix,
                                      TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool logicalExpression(ParseNodeKind pnk, HandleValue left,
                                       HandleValue right, TokenPos* pos,
                                       MutableHandleValue dst);

  [[nodiscard]] bool conditionalExpression(HandleValue test, HandleValue cons,
                                           HandleValue alt, TokenPos* pos,
                                           MutableHandleValue dst);

  [[nodiscard]] bool sequenceExpression(NodeVector& elts, TokenPos* pos,
                                        MutableHandleValue dst);

  [[nodiscard]] bool newExpression(HandleValue callee, NodeVector& args,
                                   TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool callExpression(HandleValue callee, NodeVector& args,
                                    TokenPos* pos, MutableHandleValue dst,
                                    bool isOptional = false);

  [[nodiscard]] bool memberExpression(bool computed, HandleValue expr,
                                      HandleValue member, TokenPos* pos,
                                      MutableHandleValue dst,
                                      bool isOptional = false);

  [[nodiscard]] bool arrayExpression(NodeVector& elts, TokenPos* pos,
                                     MutableHandleValue dst);

  [[nodiscard]] bool templateLiteral(NodeVector& elts, TokenPos* pos,
                                     MutableHandleValue dst);

  [[nodiscard]] bool taggedTemplate(HandleValue callee, NodeVector& args,
                                    TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool callSiteObj(NodeVector& raw, NodeVector& cooked,
                                 TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool spreadExpression(HandleValue expr, TokenPos* pos,
                                      MutableHandleValue dst);

  [[nodiscard]] bool optionalExpression(HandleValue expr, TokenPos* pos,
                                        MutableHandleValue dst);

  [[nodiscard]] bool deleteOptionalExpression(HandleValue expr, TokenPos* pos,
                                              MutableHandleValue dst);

  [[nodiscard]] bool computedName(HandleValue name, TokenPos* pos,
                                  MutableHandleValue dst);

  [[nodiscard]] bool objectExpression(NodeVector& elts, TokenPos* pos,
                                      MutableHandleValue dst);

  [[nodiscard]] bool thisExpression(TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool yieldExpression(HandleValue arg, YieldKind kind,
                                     TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool metaProperty(HandleValue meta, HandleValue property,
                                  TokenPos* pos, MutableHandleValue dst);

  [[nodiscard]] bool callImportExpression(HandleValue ident, HandleValue arg,
                                          TokenPos* pos,
                                          MutableHandleValue dst);

  [[nodiscard]] bool super(TokenPos* pos, MutableHandleValue dst);

  /*
   * declarations
   */

  [[nodiscard]] bool variableDeclaration(NodeVector& elts, VarDeclKind kind,
                                         TokenPos* pos, MutableHandleValue dst);

  /*
   * patterns
   */

  [[nodiscard]] bool arrayPattern(NodeVector& elts, TokenPos* pos,
                                  MutableHandleValue dst);

  [[nodiscard]] bool objectPattern(NodeVector& elts, TokenPos* pos,
                                   MutableHandleValue dst);

  [[nodiscard]] bool propertyPattern(HandleValue key, HandleValue patt,
                                     bool isShorthand, TokenPos* pos,
                                     MutableHandleValue dst);
};

} /* anonymous namespace */

bool NodeBuilder::createNode(ASTType type, TokenPos* pos,
                             MutableHandleObject dst) {
  MOZ_ASSERT(type > AST_ERROR && type < AST_LIMIT);

  RootedValue tv(cx);
  RootedPlainObject node(cx, NewBuiltinClassInstance<PlainObject>(cx));
  if (!node || !setNodeLoc(node, pos) || !atomValue(nodeTypeNames[type], &tv) ||
      !defineProperty(node, "type", tv)) {
    return false;
  }

  dst.set(node);
  return true;
}

bool NodeBuilder::newArray(NodeVector& elts, MutableHandleValue dst) {
  const size_t len = elts.length();
  if (len > UINT32_MAX) {
    ReportAllocationOverflow(cx);
    return false;
  }
  RootedObject array(cx, NewDenseFullyAllocatedArray(cx, uint32_t(len)));
  if (!array) {
    return false;
  }

  for (size_t i = 0; i < len; i++) {
    RootedValue val(cx, elts[i]);

    MOZ_ASSERT_IF(val.isMagic(), val.whyMagic() == JS_SERIALIZE_NO_NODE);

    /* Represent "no node" as an array hole by not adding the value. */
    if (val.isMagic(JS_SERIALIZE_NO_NODE)) {
      continue;
    }

    if (!DefineDataElement(cx, array, i, val)) {
      return false;
    }
  }

  dst.setObject(*array);
  return true;
}

bool NodeBuilder::newNodeLoc(TokenPos* pos, MutableHandleValue dst) {
  if (!pos) {
    dst.setNull();
    return true;
  }

  RootedObject loc(cx);
  RootedObject to(cx);
  RootedValue val(cx);

  if (!newObject(&loc)) {
    return false;
  }

  dst.setObject(*loc);

  uint32_t startLineNum, startColumnIndex;
  uint32_t endLineNum, endColumnIndex;
  parser->tokenStream.computeLineAndColumn(pos->begin, &startLineNum,
                                           &startColumnIndex);
  parser->tokenStream.computeLineAndColumn(pos->end, &endLineNum,
                                           &endColumnIndex);

  if (!newObject(&to)) {
    return false;
  }
  val.setObject(*to);
  if (!defineProperty(loc, "start", val)) {
    return false;
  }
  val.setNumber(startLineNum);
  if (!defineProperty(to, "line", val)) {
    return false;
  }
  val.setNumber(startColumnIndex);
  if (!defineProperty(to, "column", val)) {
    return false;
  }

  if (!newObject(&to)) {
    return false;
  }
  val.setObject(*to);
  if (!defineProperty(loc, "end", val)) {
    return false;
  }
  val.setNumber(endLineNum);
  if (!defineProperty(to, "line", val)) {
    return false;
  }
  val.setNumber(endColumnIndex);
  if (!defineProperty(to, "column", val)) {
    return false;
  }

  if (!defineProperty(loc, "source", srcval)) {
    return false;
  }

  return true;
}

bool NodeBuilder::setNodeLoc(HandleObject node, TokenPos* pos) {
  if (!saveLoc) {
    return true;
  }

  RootedValue loc(cx);
  return newNodeLoc(pos, &loc) && defineProperty(node, "loc", loc);
}

bool NodeBuilder::program(NodeVector& elts, TokenPos* pos,
                          MutableHandleValue dst) {
  return listNode(AST_PROGRAM, "body", elts, pos, dst);
}

bool NodeBuilder::blockStatement(NodeVector& elts, TokenPos* pos,
                                 MutableHandleValue dst) {
  return listNode(AST_BLOCK_STMT, "body", elts, pos, dst);
}

bool NodeBuilder::expressionStatement(HandleValue expr, TokenPos* pos,
                                      MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_EXPR_STMT]);
  if (!cb.isNull()) {
    return callback(cb, expr, pos, dst);
  }

  return newNode(AST_EXPR_STMT, pos, "expression", expr, dst);
}

bool NodeBuilder::emptyStatement(TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_EMPTY_STMT]);
  if (!cb.isNull()) {
    return callback(cb, pos, dst);
  }

  return newNode(AST_EMPTY_STMT, pos, dst);
}

bool NodeBuilder::ifStatement(HandleValue test, HandleValue cons,
                              HandleValue alt, TokenPos* pos,
                              MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_IF_STMT]);
  if (!cb.isNull()) {
    return callback(cb, test, cons, opt(alt), pos, dst);
  }

  return newNode(AST_IF_STMT, pos, "test", test, "consequent", cons,
                 "alternate", alt, dst);
}

bool NodeBuilder::breakStatement(HandleValue label, TokenPos* pos,
                                 MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_BREAK_STMT]);
  if (!cb.isNull()) {
    return callback(cb, opt(label), pos, dst);
  }

  return newNode(AST_BREAK_STMT, pos, "label", label, dst);
}

bool NodeBuilder::continueStatement(HandleValue label, TokenPos* pos,
                                    MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_CONTINUE_STMT]);
  if (!cb.isNull()) {
    return callback(cb, opt(label), pos, dst);
  }

  return newNode(AST_CONTINUE_STMT, pos, "label", label, dst);
}

bool NodeBuilder::labeledStatement(HandleValue label, HandleValue stmt,
                                   TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_LAB_STMT]);
  if (!cb.isNull()) {
    return callback(cb, label, stmt, pos, dst);
  }

  return newNode(AST_LAB_STMT, pos, "label", label, "body", stmt, dst);
}

bool NodeBuilder::throwStatement(HandleValue arg, TokenPos* pos,
                                 MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_THROW_STMT]);
  if (!cb.isNull()) {
    return callback(cb, arg, pos, dst);
  }

  return newNode(AST_THROW_STMT, pos, "argument", arg, dst);
}

bool NodeBuilder::returnStatement(HandleValue arg, TokenPos* pos,
                                  MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_RETURN_STMT]);
  if (!cb.isNull()) {
    return callback(cb, opt(arg), pos, dst);
  }

  return newNode(AST_RETURN_STMT, pos, "argument", arg, dst);
}

bool NodeBuilder::forStatement(HandleValue init, HandleValue test,
                               HandleValue update, HandleValue stmt,
                               TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_FOR_STMT]);
  if (!cb.isNull()) {
    return callback(cb, opt(init), opt(test), opt(update), stmt, pos, dst);
  }

  return newNode(AST_FOR_STMT, pos, "init", init, "test", test, "update",
                 update, "body", stmt, dst);
}

bool NodeBuilder::forInStatement(HandleValue var, HandleValue expr,
                                 HandleValue stmt, TokenPos* pos,
                                 MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_FOR_IN_STMT]);
  if (!cb.isNull()) {
    RootedValue isForEach(
        cx, JS::FalseValue());  // obsolete E4X `for each` statement
    return callback(cb, var, expr, stmt, isForEach, pos, dst);
  }

  return newNode(AST_FOR_IN_STMT, pos, "left", var, "right", expr, "body", stmt,
                 dst);
}

bool NodeBuilder::forOfStatement(HandleValue var, HandleValue expr,
                                 HandleValue stmt, TokenPos* pos,
                                 MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_FOR_OF_STMT]);
  if (!cb.isNull()) {
    return callback(cb, var, expr, stmt, pos, dst);
  }

  return newNode(AST_FOR_OF_STMT, pos, "left", var, "right", expr, "body", stmt,
                 dst);
}

bool NodeBuilder::withStatement(HandleValue expr, HandleValue stmt,
                                TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_WITH_STMT]);
  if (!cb.isNull()) {
    return callback(cb, expr, stmt, pos, dst);
  }

  return newNode(AST_WITH_STMT, pos, "object", expr, "body", stmt, dst);
}

bool NodeBuilder::whileStatement(HandleValue test, HandleValue stmt,
                                 TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_WHILE_STMT]);
  if (!cb.isNull()) {
    return callback(cb, test, stmt, pos, dst);
  }

  return newNode(AST_WHILE_STMT, pos, "test", test, "body", stmt, dst);
}

bool NodeBuilder::doWhileStatement(HandleValue stmt, HandleValue test,
                                   TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_DO_STMT]);
  if (!cb.isNull()) {
    return callback(cb, stmt, test, pos, dst);
  }

  return newNode(AST_DO_STMT, pos, "body", stmt, "test", test, dst);
}

bool NodeBuilder::switchStatement(HandleValue disc, NodeVector& elts,
                                  bool lexical, TokenPos* pos,
                                  MutableHandleValue dst) {
  RootedValue array(cx);
  if (!newArray(elts, &array)) {
    return false;
  }

  RootedValue lexicalVal(cx, BooleanValue(lexical));

  RootedValue cb(cx, callbacks[AST_SWITCH_STMT]);
  if (!cb.isNull()) {
    return callback(cb, disc, array, lexicalVal, pos, dst);
  }

  return newNode(AST_SWITCH_STMT, pos, "discriminant", disc, "cases", array,
                 "lexical", lexicalVal, dst);
}

bool NodeBuilder::tryStatement(HandleValue body, HandleValue handler,
                               HandleValue finally, TokenPos* pos,
                               MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_TRY_STMT]);
  if (!cb.isNull()) {
    return callback(cb, body, handler, opt(finally), pos, dst);
  }

  return newNode(AST_TRY_STMT, pos, "block", body, "handler", handler,
                 "finalizer", finally, dst);
}

bool NodeBuilder::debuggerStatement(TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_DEBUGGER_STMT]);
  if (!cb.isNull()) {
    return callback(cb, pos, dst);
  }

  return newNode(AST_DEBUGGER_STMT, pos, dst);
}

bool NodeBuilder::binaryExpression(BinaryOperator op, HandleValue left,
                                   HandleValue right, TokenPos* pos,
                                   MutableHandleValue dst) {
  MOZ_ASSERT(op > BINOP_ERR && op < BINOP_LIMIT);

  RootedValue opName(cx);
  if (!atomValue(binopNames[op], &opName)) {
    return false;
  }

  RootedValue cb(cx, callbacks[AST_BINARY_EXPR]);
  if (!cb.isNull()) {
    return callback(cb, opName, left, right, pos, dst);
  }

  return newNode(AST_BINARY_EXPR, pos, "operator", opName, "left", left,
                 "right", right, dst);
}

bool NodeBuilder::unaryExpression(UnaryOperator unop, HandleValue expr,
                                  TokenPos* pos, MutableHandleValue dst) {
  MOZ_ASSERT(unop > UNOP_ERR && unop < UNOP_LIMIT);

  RootedValue opName(cx);
  if (!atomValue(unopNames[unop], &opName)) {
    return false;
  }

  RootedValue cb(cx, callbacks[AST_UNARY_EXPR]);
  if (!cb.isNull()) {
    return callback(cb, opName, expr, pos, dst);
  }

  RootedValue trueVal(cx, BooleanValue(true));
  return newNode(AST_UNARY_EXPR, pos, "operator", opName, "argument", expr,
                 "prefix", trueVal, dst);
}

bool NodeBuilder::assignmentExpression(AssignmentOperator aop, HandleValue lhs,
                                       HandleValue rhs, TokenPos* pos,
                                       MutableHandleValue dst) {
  MOZ_ASSERT(aop > AOP_ERR && aop < AOP_LIMIT);

  RootedValue opName(cx);
  if (!atomValue(aopNames[aop], &opName)) {
    return false;
  }

  RootedValue cb(cx, callbacks[AST_ASSIGN_EXPR]);
  if (!cb.isNull()) {
    return callback(cb, opName, lhs, rhs, pos, dst);
  }

  return newNode(AST_ASSIGN_EXPR, pos, "operator", opName, "left", lhs, "right",
                 rhs, dst);
}

bool NodeBuilder::updateExpression(HandleValue expr, bool incr, bool prefix,
                                   TokenPos* pos, MutableHandleValue dst) {
  RootedValue opName(cx);
  if (!atomValue(incr ? "++" : "--", &opName)) {
    return false;
  }

  RootedValue prefixVal(cx, BooleanValue(prefix));

  RootedValue cb(cx, callbacks[AST_UPDATE_EXPR]);
  if (!cb.isNull()) {
    return callback(cb, expr, opName, prefixVal, pos, dst);
  }

  return newNode(AST_UPDATE_EXPR, pos, "operator", opName, "argument", expr,
                 "prefix", prefixVal, dst);
}

bool NodeBuilder::logicalExpression(ParseNodeKind pnk, HandleValue left,
                                    HandleValue right, TokenPos* pos,
                                    MutableHandleValue dst) {
  RootedValue opName(cx);
  switch (pnk) {
    case ParseNodeKind::OrExpr:
      if (!atomValue("||", &opName)) {
        return false;
      }
      break;
    case ParseNodeKind::CoalesceExpr:
      if (!atomValue("??", &opName)) {
        return false;
      }
      break;
    case ParseNodeKind::AndExpr:
      if (!atomValue("&&", &opName)) {
        return false;
      }
      break;
    default:
      MOZ_CRASH("Unexpected ParseNodeKind: Must be `Or`, `And`, or `Coalesce`");
  }

  RootedValue cb(cx, callbacks[AST_LOGICAL_EXPR]);
  if (!cb.isNull()) {
    return callback(cb, opName, left, right, pos, dst);
  }

  return newNode(AST_LOGICAL_EXPR, pos, "operator", opName, "left", left,
                 "right", right, dst);
}

bool NodeBuilder::conditionalExpression(HandleValue test, HandleValue cons,
                                        HandleValue alt, TokenPos* pos,
                                        MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_COND_EXPR]);
  if (!cb.isNull()) {
    return callback(cb, test, cons, alt, pos, dst);
  }

  return newNode(AST_COND_EXPR, pos, "test", test, "consequent", cons,
                 "alternate", alt, dst);
}

bool NodeBuilder::sequenceExpression(NodeVector& elts, TokenPos* pos,
                                     MutableHandleValue dst) {
  return listNode(AST_LIST_EXPR, "expressions", elts, pos, dst);
}

bool NodeBuilder::callExpression(HandleValue callee, NodeVector& args,
                                 TokenPos* pos, MutableHandleValue dst,
                                 bool isOptional) {
  RootedValue array(cx);
  if (!newArray(args, &array)) {
    return false;
  }

  RootedValue cb(cx, callbacks[AST_CALL_EXPR]);
  if (!cb.isNull()) {
    return callback(cb, callee, array, pos, dst);
  }

  return newNode(isOptional ? AST_OPT_CALL_EXPR : AST_CALL_EXPR, pos, "callee",
                 callee, "arguments", array, dst);
}

bool NodeBuilder::newExpression(HandleValue callee, NodeVector& args,
                                TokenPos* pos, MutableHandleValue dst) {
  RootedValue array(cx);
  if (!newArray(args, &array)) {
    return false;
  }

  RootedValue cb(cx, callbacks[AST_NEW_EXPR]);
  if (!cb.isNull()) {
    return callback(cb, callee, array, pos, dst);
  }

  return newNode(AST_NEW_EXPR, pos, "callee", callee, "arguments", array, dst);
}

bool NodeBuilder::memberExpression(bool computed, HandleValue expr,
                                   HandleValue member, TokenPos* pos,
                                   MutableHandleValue dst,
                                   bool isOptional /* = false */) {
  RootedValue computedVal(cx, BooleanValue(computed));

  RootedValue cb(cx, callbacks[AST_MEMBER_EXPR]);
  if (!cb.isNull()) {
    return callback(cb, computedVal, expr, member, pos, dst);
  }

  return newNode(isOptional ? AST_OPT_MEMBER_EXPR : AST_MEMBER_EXPR, pos,
                 "object", expr, "property", member, "computed", computedVal,
                 dst);
}

bool NodeBuilder::arrayExpression(NodeVector& elts, TokenPos* pos,
                                  MutableHandleValue dst) {
  return listNode(AST_ARRAY_EXPR, "elements", elts, pos, dst);
}

bool NodeBuilder::callSiteObj(NodeVector& raw, NodeVector& cooked,
                              TokenPos* pos, MutableHandleValue dst) {
  RootedValue rawVal(cx);
  if (!newArray(raw, &rawVal)) {
    return false;
  }

  RootedValue cookedVal(cx);
  if (!newArray(cooked, &cookedVal)) {
    return false;
  }

  return newNode(AST_CALL_SITE_OBJ, pos, "raw", rawVal, "cooked", cookedVal,
                 dst);
}

bool NodeBuilder::taggedTemplate(HandleValue callee, NodeVector& args,
                                 TokenPos* pos, MutableHandleValue dst) {
  RootedValue array(cx);
  if (!newArray(args, &array)) {
    return false;
  }

  return newNode(AST_TAGGED_TEMPLATE, pos, "callee", callee, "arguments", array,
                 dst);
}

bool NodeBuilder::templateLiteral(NodeVector& elts, TokenPos* pos,
                                  MutableHandleValue dst) {
  return listNode(AST_TEMPLATE_LITERAL, "elements", elts, pos, dst);
}

bool NodeBuilder::computedName(HandleValue name, TokenPos* pos,
                               MutableHandleValue dst) {
  return newNode(AST_COMPUTED_NAME, pos, "name", name, dst);
}

bool NodeBuilder::spreadExpression(HandleValue expr, TokenPos* pos,
                                   MutableHandleValue dst) {
  return newNode(AST_SPREAD_EXPR, pos, "expression", expr, dst);
}

bool NodeBuilder::optionalExpression(HandleValue expr, TokenPos* pos,
                                     MutableHandleValue dst) {
  return newNode(AST_OPTIONAL_EXPR, pos, "expression", expr, dst);
}

bool NodeBuilder::deleteOptionalExpression(HandleValue expr, TokenPos* pos,
                                           MutableHandleValue dst) {
  return newNode(AST_DELETE_OPTIONAL_EXPR, pos, "expression", expr, dst);
}

bool NodeBuilder::propertyPattern(HandleValue key, HandleValue patt,
                                  bool isShorthand, TokenPos* pos,
                                  MutableHandleValue dst) {
  RootedValue kindName(cx);
  if (!atomValue("init", &kindName)) {
    return false;
  }

  RootedValue isShorthandVal(cx, BooleanValue(isShorthand));

  RootedValue cb(cx, callbacks[AST_PROP_PATT]);
  if (!cb.isNull()) {
    return callback(cb, key, patt, pos, dst);
  }

  return newNode(AST_PROP_PATT, pos, "key", key, "value", patt, "kind",
                 kindName, "shorthand", isShorthandVal, dst);
}

bool NodeBuilder::prototypeMutation(HandleValue val, TokenPos* pos,
                                    MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_PROTOTYPEMUTATION]);
  if (!cb.isNull()) {
    return callback(cb, val, pos, dst);
  }

  return newNode(AST_PROTOTYPEMUTATION, pos, "value", val, dst);
}

bool NodeBuilder::propertyInitializer(HandleValue key, HandleValue val,
                                      PropKind kind, bool isShorthand,
                                      bool isMethod, TokenPos* pos,
                                      MutableHandleValue dst) {
  RootedValue kindName(cx);
  if (!atomValue(kind == PROP_INIT     ? "init"
                 : kind == PROP_GETTER ? "get"
                                       : "set",
                 &kindName)) {
    return false;
  }

  RootedValue isShorthandVal(cx, BooleanValue(isShorthand));
  RootedValue isMethodVal(cx, BooleanValue(isMethod));

  RootedValue cb(cx, callbacks[AST_PROPERTY]);
  if (!cb.isNull()) {
    return callback(cb, kindName, key, val, pos, dst);
  }

  return newNode(AST_PROPERTY, pos, "key", key, "value", val, "kind", kindName,
                 "method", isMethodVal, "shorthand", isShorthandVal, dst);
}

bool NodeBuilder::objectExpression(NodeVector& elts, TokenPos* pos,
                                   MutableHandleValue dst) {
  return listNode(AST_OBJECT_EXPR, "properties", elts, pos, dst);
}

bool NodeBuilder::thisExpression(TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_THIS_EXPR]);
  if (!cb.isNull()) {
    return callback(cb, pos, dst);
  }

  return newNode(AST_THIS_EXPR, pos, dst);
}

bool NodeBuilder::yieldExpression(HandleValue arg, YieldKind kind,
                                  TokenPos* pos, MutableHandleValue dst) {
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

  if (!cb.isNull()) {
    return callback(cb, opt(arg), delegateVal, pos, dst);
  }
  return newNode(AST_YIELD_EXPR, pos, "argument", arg, "delegate", delegateVal,
                 dst);
}

bool NodeBuilder::importDeclaration(NodeVector& elts, HandleValue moduleSpec,
                                    TokenPos* pos, MutableHandleValue dst) {
  RootedValue array(cx);
  if (!newArray(elts, &array)) {
    return false;
  }

  RootedValue cb(cx, callbacks[AST_IMPORT_DECL]);
  if (!cb.isNull()) {
    return callback(cb, array, moduleSpec, pos, dst);
  }

  return newNode(AST_IMPORT_DECL, pos, "specifiers", array, "source",
                 moduleSpec, dst);
}

bool NodeBuilder::importSpecifier(HandleValue importName,
                                  HandleValue bindingName, TokenPos* pos,
                                  MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_IMPORT_SPEC]);
  if (!cb.isNull()) {
    return callback(cb, importName, bindingName, pos, dst);
  }

  return newNode(AST_IMPORT_SPEC, pos, "id", importName, "name", bindingName,
                 dst);
}

bool NodeBuilder::importNamespaceSpecifier(HandleValue bindingName,
                                           TokenPos* pos,
                                           MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_IMPORT_NAMESPACE_SPEC]);
  if (!cb.isNull()) {
    return callback(cb, bindingName, pos, dst);
  }

  return newNode(AST_IMPORT_NAMESPACE_SPEC, pos, "name", bindingName, dst);
}

bool NodeBuilder::exportDeclaration(HandleValue decl, NodeVector& elts,
                                    HandleValue moduleSpec,
                                    HandleValue isDefault, TokenPos* pos,
                                    MutableHandleValue dst) {
  RootedValue array(cx, NullValue());
  if (decl.isNull() && !newArray(elts, &array)) {
    return false;
  }

  RootedValue cb(cx, callbacks[AST_EXPORT_DECL]);

  if (!cb.isNull()) {
    return callback(cb, decl, array, moduleSpec, pos, dst);
  }

  return newNode(AST_EXPORT_DECL, pos, "declaration", decl, "specifiers", array,
                 "source", moduleSpec, "isDefault", isDefault, dst);
}

bool NodeBuilder::exportSpecifier(HandleValue bindingName,
                                  HandleValue exportName, TokenPos* pos,
                                  MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_EXPORT_SPEC]);
  if (!cb.isNull()) {
    return callback(cb, bindingName, exportName, pos, dst);
  }

  return newNode(AST_EXPORT_SPEC, pos, "id", bindingName, "name", exportName,
                 dst);
}

bool NodeBuilder::exportNamespaceSpecifier(HandleValue exportName,
                                           TokenPos* pos,
                                           MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_EXPORT_NAMESPACE_SPEC]);
  if (!cb.isNull()) {
    return callback(cb, exportName, pos, dst);
  }

  return newNode(AST_EXPORT_NAMESPACE_SPEC, pos, "name", exportName, dst);
}

bool NodeBuilder::exportBatchSpecifier(TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_EXPORT_BATCH_SPEC]);
  if (!cb.isNull()) {
    return callback(cb, pos, dst);
  }

  return newNode(AST_EXPORT_BATCH_SPEC, pos, dst);
}

bool NodeBuilder::variableDeclaration(NodeVector& elts, VarDeclKind kind,
                                      TokenPos* pos, MutableHandleValue dst) {
  MOZ_ASSERT(kind > VARDECL_ERR && kind < VARDECL_LIMIT);

  RootedValue array(cx), kindName(cx);
  if (!newArray(elts, &array) || !atomValue(kind == VARDECL_CONST ? "const"
                                            : kind == VARDECL_LET ? "let"
                                                                  : "var",
                                            &kindName)) {
    return false;
  }

  RootedValue cb(cx, callbacks[AST_VAR_DECL]);
  if (!cb.isNull()) {
    return callback(cb, kindName, array, pos, dst);
  }

  return newNode(AST_VAR_DECL, pos, "kind", kindName, "declarations", array,
                 dst);
}

bool NodeBuilder::variableDeclarator(HandleValue id, HandleValue init,
                                     TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_VAR_DTOR]);
  if (!cb.isNull()) {
    return callback(cb, id, opt(init), pos, dst);
  }

  return newNode(AST_VAR_DTOR, pos, "id", id, "init", init, dst);
}

bool NodeBuilder::switchCase(HandleValue expr, NodeVector& elts, TokenPos* pos,
                             MutableHandleValue dst) {
  RootedValue array(cx);
  if (!newArray(elts, &array)) {
    return false;
  }

  RootedValue cb(cx, callbacks[AST_CASE]);
  if (!cb.isNull()) {
    return callback(cb, opt(expr), array, pos, dst);
  }

  return newNode(AST_CASE, pos, "test", expr, "consequent", array, dst);
}

bool NodeBuilder::catchClause(HandleValue var, HandleValue body, TokenPos* pos,
                              MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_CATCH]);
  if (!cb.isNull()) {
    return callback(cb, opt(var), body, pos, dst);
  }

  return newNode(AST_CATCH, pos, "param", var, "body", body, dst);
}

bool NodeBuilder::literal(HandleValue val, TokenPos* pos,
                          MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_LITERAL]);
  if (!cb.isNull()) {
    return callback(cb, val, pos, dst);
  }

  return newNode(AST_LITERAL, pos, "value", val, dst);
}

bool NodeBuilder::identifier(HandleValue name, TokenPos* pos,
                             MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_IDENTIFIER]);
  if (!cb.isNull()) {
    return callback(cb, name, pos, dst);
  }

  return newNode(AST_IDENTIFIER, pos, "name", name, dst);
}

bool NodeBuilder::objectPattern(NodeVector& elts, TokenPos* pos,
                                MutableHandleValue dst) {
  return listNode(AST_OBJECT_PATT, "properties", elts, pos, dst);
}

bool NodeBuilder::arrayPattern(NodeVector& elts, TokenPos* pos,
                               MutableHandleValue dst) {
  return listNode(AST_ARRAY_PATT, "elements", elts, pos, dst);
}

bool NodeBuilder::function(ASTType type, TokenPos* pos, HandleValue id,
                           NodeVector& args, NodeVector& defaults,
                           HandleValue body, HandleValue rest,
                           GeneratorStyle generatorStyle, bool isAsync,
                           bool isExpression, MutableHandleValue dst) {
  RootedValue array(cx), defarray(cx);
  if (!newArray(args, &array)) {
    return false;
  }
  if (!newArray(defaults, &defarray)) {
    return false;
  }

  bool isGenerator = generatorStyle != GeneratorStyle::None;
  RootedValue isGeneratorVal(cx, BooleanValue(isGenerator));
  RootedValue isAsyncVal(cx, BooleanValue(isAsync));
  RootedValue isExpressionVal(cx, BooleanValue(isExpression));

  RootedValue cb(cx, callbacks[type]);
  if (!cb.isNull()) {
    return callback(cb, opt(id), array, body, isGeneratorVal, isExpressionVal,
                    pos, dst);
  }

  if (isGenerator) {
    MOZ_ASSERT(generatorStyle == GeneratorStyle::ES6);
    JSAtom* styleStr = Atomize(cx, "es6", 3);
    if (!styleStr) {
      return false;
    }
    RootedValue styleVal(cx, StringValue(styleStr));
    return newNode(type, pos, "id", id, "params", array, "defaults", defarray,
                   "body", body, "rest", rest, "generator", isGeneratorVal,
                   "async", isAsyncVal, "style", styleVal, "expression",
                   isExpressionVal, dst);
  }

  return newNode(type, pos, "id", id, "params", array, "defaults", defarray,
                 "body", body, "rest", rest, "generator", isGeneratorVal,
                 "async", isAsyncVal, "expression", isExpressionVal, dst);
}

bool NodeBuilder::classMethod(HandleValue name, HandleValue body, PropKind kind,
                              bool isStatic, TokenPos* pos,
                              MutableHandleValue dst) {
  RootedValue kindName(cx);
  if (!atomValue(kind == PROP_INIT     ? "method"
                 : kind == PROP_GETTER ? "get"
                                       : "set",
                 &kindName)) {
    return false;
  }

  RootedValue isStaticVal(cx, BooleanValue(isStatic));
  RootedValue cb(cx, callbacks[AST_CLASS_METHOD]);
  if (!cb.isNull()) {
    return callback(cb, kindName, name, body, isStaticVal, pos, dst);
  }

  return newNode(AST_CLASS_METHOD, pos, "name", name, "body", body, "kind",
                 kindName, "static", isStaticVal, dst);
}

bool NodeBuilder::classField(HandleValue name, HandleValue initializer,
                             TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_CLASS_FIELD]);
  if (!cb.isNull()) {
    return callback(cb, name, initializer, pos, dst);
  }

  return newNode(AST_CLASS_FIELD, pos, "name", name, "init", initializer, dst);
}

bool NodeBuilder::staticClassBlock(HandleValue body, TokenPos* pos,
                                   MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_STATIC_CLASS_BLOCK]);
  if (!cb.isNull()) {
    return callback(cb, body, pos, dst);
  }

  return newNode(AST_STATIC_CLASS_BLOCK, pos, "body", body, dst);
}

bool NodeBuilder::classMembers(NodeVector& members, MutableHandleValue dst) {
  return newArray(members, dst);
}

bool NodeBuilder::classDefinition(bool expr, HandleValue name,
                                  HandleValue heritage, HandleValue block,
                                  TokenPos* pos, MutableHandleValue dst) {
  ASTType type = expr ? AST_CLASS_EXPR : AST_CLASS_STMT;
  RootedValue cb(cx, callbacks[type]);
  if (!cb.isNull()) {
    return callback(cb, name, heritage, block, pos, dst);
  }

  return newNode(type, pos, "id", name, "superClass", heritage, "body", block,
                 dst);
}

bool NodeBuilder::metaProperty(HandleValue meta, HandleValue property,
                               TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_METAPROPERTY]);
  if (!cb.isNull()) {
    return callback(cb, meta, property, pos, dst);
  }

  return newNode(AST_METAPROPERTY, pos, "meta", meta, "property", property,
                 dst);
}

bool NodeBuilder::callImportExpression(HandleValue ident, HandleValue arg,
                                       TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_CALL_IMPORT]);
  if (!cb.isNull()) {
    return callback(cb, arg, pos, dst);
  }

  return newNode(AST_CALL_IMPORT, pos, "ident", ident, "arg", arg, dst);
}

bool NodeBuilder::super(TokenPos* pos, MutableHandleValue dst) {
  RootedValue cb(cx, callbacks[AST_SUPER]);
  if (!cb.isNull()) {
    return callback(cb, pos, dst);
  }

  return newNode(AST_SUPER, pos, dst);
}

namespace {

/*
 * Serialization of parse nodes to JavaScript objects.
 *
 * All serialization methods take a non-nullable ParseNode pointer.
 */
class ASTSerializer {
  JSContext* cx;
  Parser<FullParseHandler, char16_t>* parser;
  NodeBuilder builder;
  DebugOnly<uint32_t> lineno;

  Value unrootedAtomContents(JSAtom* atom) {
    return StringValue(atom ? atom : cx->names().empty);
  }

  BinaryOperator binop(ParseNodeKind kind);
  UnaryOperator unop(ParseNodeKind kind);
  AssignmentOperator aop(ParseNodeKind kind);

  bool statements(ListNode* stmtList, NodeVector& elts);
  bool expressions(ListNode* exprList, NodeVector& elts);
  bool leftAssociate(ListNode* node, MutableHandleValue dst);
  bool rightAssociate(ListNode* node, MutableHandleValue dst);
  bool functionArgs(ParseNode* pn, ListNode* argsList, NodeVector& args,
                    NodeVector& defaults, MutableHandleValue rest);

  bool sourceElement(ParseNode* pn, MutableHandleValue dst);

  bool declaration(ParseNode* pn, MutableHandleValue dst);
  bool variableDeclaration(ListNode* declList, bool lexical,
                           MutableHandleValue dst);
  bool variableDeclarator(ParseNode* pn, MutableHandleValue dst);
  bool importDeclaration(BinaryNode* importNode, MutableHandleValue dst);
  bool importSpecifier(BinaryNode* importSpec, MutableHandleValue dst);
  bool importNamespaceSpecifier(UnaryNode* importSpec, MutableHandleValue dst);
  bool exportDeclaration(ParseNode* exportNode, MutableHandleValue dst);
  bool exportSpecifier(BinaryNode* exportSpec, MutableHandleValue dst);
  bool exportNamespaceSpecifier(UnaryNode* exportSpec, MutableHandleValue dst);
  bool classDefinition(ClassNode* pn, bool expr, MutableHandleValue dst);

  bool optStatement(ParseNode* pn, MutableHandleValue dst) {
    if (!pn) {
      dst.setMagic(JS_SERIALIZE_NO_NODE);
      return true;
    }
    return statement(pn, dst);
  }

  bool forInit(ParseNode* pn, MutableHandleValue dst);
  bool forIn(ForNode* loop, ParseNode* iterExpr, HandleValue var,
             HandleValue stmt, MutableHandleValue dst);
  bool forOf(ForNode* loop, ParseNode* iterExpr, HandleValue var,
             HandleValue stmt, MutableHandleValue dst);
  bool statement(ParseNode* pn, MutableHandleValue dst);
  bool blockStatement(ListNode* node, MutableHandleValue dst);
  bool switchStatement(SwitchStatement* switchStmt, MutableHandleValue dst);
  bool switchCase(CaseClause* caseClause, MutableHandleValue dst);
  bool tryStatement(TryNode* tryNode, MutableHandleValue dst);
  bool catchClause(BinaryNode* catchClause, MutableHandleValue dst);

  bool optExpression(ParseNode* pn, MutableHandleValue dst) {
    if (!pn) {
      dst.setMagic(JS_SERIALIZE_NO_NODE);
      return true;
    }
    return expression(pn, dst);
  }

  bool expression(ParseNode* pn, MutableHandleValue dst);

  bool propertyName(ParseNode* key, MutableHandleValue dst);
  bool property(ParseNode* pn, MutableHandleValue dst);

  bool classMethod(ClassMethod* classMethod, MutableHandleValue dst);
  bool classField(ClassField* classField, MutableHandleValue dst);
  bool staticClassBlock(StaticClassBlock* staticClassBlock,
                        MutableHandleValue dst);

  bool optIdentifier(HandleAtom atom, TokenPos* pos, MutableHandleValue dst) {
    if (!atom) {
      dst.setMagic(JS_SERIALIZE_NO_NODE);
      return true;
    }
    return identifier(atom, pos, dst);
  }

  bool identifier(HandleAtom atom, TokenPos* pos, MutableHandleValue dst);
  bool identifier(NameNode* id, MutableHandleValue dst);
  bool identifierOrLiteral(ParseNode* id, MutableHandleValue dst);
  bool literal(ParseNode* pn, MutableHandleValue dst);

  bool optPattern(ParseNode* pn, MutableHandleValue dst) {
    if (!pn) {
      dst.setMagic(JS_SERIALIZE_NO_NODE);
      return true;
    }
    return pattern(pn, dst);
  }

  bool pattern(ParseNode* pn, MutableHandleValue dst);
  bool arrayPattern(ListNode* array, MutableHandleValue dst);
  bool objectPattern(ListNode* obj, MutableHandleValue dst);

  bool function(FunctionNode* funNode, ASTType type, MutableHandleValue dst);
  bool functionArgsAndBody(ParseNode* pn, NodeVector& args,
                           NodeVector& defaults, bool isAsync,
                           bool isExpression, MutableHandleValue body,
                           MutableHandleValue rest);
  bool functionBody(ParseNode* pn, TokenPos* pos, MutableHandleValue dst);

 public:
  ASTSerializer(JSContext* c, bool l, char const* src, uint32_t ln)
      : cx(c),
        parser(nullptr),
        builder(c, l, src)
#ifdef DEBUG
        ,
        lineno(ln)
#endif
  {
  }

  bool init(HandleObject userobj) { return builder.init(userobj); }

  void setParser(frontend::Parser<frontend::FullParseHandler, char16_t>* p) {
    parser = p;
    builder.setParser(p);
  }

  bool program(ListNode* node, MutableHandleValue dst);
};

} /* anonymous namespace */

AssignmentOperator ASTSerializer::aop(ParseNodeKind kind) {
  switch (kind) {
    case ParseNodeKind::AssignExpr:
      return AOP_ASSIGN;
    case ParseNodeKind::AddAssignExpr:
      return AOP_PLUS;
    case ParseNodeKind::SubAssignExpr:
      return AOP_MINUS;
    case ParseNodeKind::MulAssignExpr:
      return AOP_STAR;
    case ParseNodeKind::DivAssignExpr:
      return AOP_DIV;
    case ParseNodeKind::ModAssignExpr:
      return AOP_MOD;
    case ParseNodeKind::PowAssignExpr:
      return AOP_POW;
    case ParseNodeKind::LshAssignExpr:
      return AOP_LSH;
    case ParseNodeKind::RshAssignExpr:
      return AOP_RSH;
    case ParseNodeKind::UrshAssignExpr:
      return AOP_URSH;
    case ParseNodeKind::BitOrAssignExpr:
      return AOP_BITOR;
    case ParseNodeKind::BitXorAssignExpr:
      return AOP_BITXOR;
    case ParseNodeKind::BitAndAssignExpr:
      return AOP_BITAND;
    case ParseNodeKind::CoalesceAssignExpr:
      return AOP_COALESCE;
    case ParseNodeKind::OrAssignExpr:
      return AOP_OR;
    case ParseNodeKind::AndAssignExpr:
      return AOP_AND;
    default:
      return AOP_ERR;
  }
}

UnaryOperator ASTSerializer::unop(ParseNodeKind kind) {
  if (IsDeleteKind(kind)) {
    return UNOP_DELETE;
  }

  if (IsTypeofKind(kind)) {
    return UNOP_TYPEOF;
  }

  switch (kind) {
    case ParseNodeKind::AwaitExpr:
      return UNOP_AWAIT;
    case ParseNodeKind::NegExpr:
      return UNOP_NEG;
    case ParseNodeKind::PosExpr:
      return UNOP_POS;
    case ParseNodeKind::NotExpr:
      return UNOP_NOT;
    case ParseNodeKind::BitNotExpr:
      return UNOP_BITNOT;
    case ParseNodeKind::VoidExpr:
      return UNOP_VOID;
    default:
      return UNOP_ERR;
  }
}

BinaryOperator ASTSerializer::binop(ParseNodeKind kind) {
  switch (kind) {
    case ParseNodeKind::LshExpr:
      return BINOP_LSH;
    case ParseNodeKind::RshExpr:
      return BINOP_RSH;
    case ParseNodeKind::UrshExpr:
      return BINOP_URSH;
    case ParseNodeKind::LtExpr:
      return BINOP_LT;
    case ParseNodeKind::LeExpr:
      return BINOP_LE;
    case ParseNodeKind::GtExpr:
      return BINOP_GT;
    case ParseNodeKind::GeExpr:
      return BINOP_GE;
    case ParseNodeKind::EqExpr:
      return BINOP_EQ;
    case ParseNodeKind::NeExpr:
      return BINOP_NE;
    case ParseNodeKind::StrictEqExpr:
      return BINOP_STRICTEQ;
    case ParseNodeKind::StrictNeExpr:
      return BINOP_STRICTNE;
    case ParseNodeKind::AddExpr:
      return BINOP_ADD;
    case ParseNodeKind::SubExpr:
      return BINOP_SUB;
    case ParseNodeKind::MulExpr:
      return BINOP_STAR;
    case ParseNodeKind::DivExpr:
      return BINOP_DIV;
    case ParseNodeKind::ModExpr:
      return BINOP_MOD;
    case ParseNodeKind::PowExpr:
      return BINOP_POW;
    case ParseNodeKind::BitOrExpr:
      return BINOP_BITOR;
    case ParseNodeKind::BitXorExpr:
      return BINOP_BITXOR;
    case ParseNodeKind::BitAndExpr:
      return BINOP_BITAND;
    case ParseNodeKind::InExpr:
    case ParseNodeKind::PrivateInExpr:
      return BINOP_IN;
    case ParseNodeKind::InstanceOfExpr:
      return BINOP_INSTANCEOF;
    case ParseNodeKind::CoalesceExpr:
      return BINOP_COALESCE;
    default:
      return BINOP_ERR;
  }
}

bool ASTSerializer::statements(ListNode* stmtList, NodeVector& elts) {
  MOZ_ASSERT(stmtList->isKind(ParseNodeKind::StatementList));

  if (!elts.reserve(stmtList->count())) {
    return false;
  }

  for (ParseNode* stmt : stmtList->contents()) {
    MOZ_ASSERT(stmtList->pn_pos.encloses(stmt->pn_pos));

    RootedValue elt(cx);
    if (!sourceElement(stmt, &elt)) {
      return false;
    }
    elts.infallibleAppend(elt);
  }

  return true;
}

bool ASTSerializer::expressions(ListNode* exprList, NodeVector& elts) {
  if (!elts.reserve(exprList->count())) {
    return false;
  }

  for (ParseNode* expr : exprList->contents()) {
    MOZ_ASSERT(exprList->pn_pos.encloses(expr->pn_pos));

    RootedValue elt(cx);
    if (!expression(expr, &elt)) {
      return false;
    }
    elts.infallibleAppend(elt);
  }

  return true;
}

bool ASTSerializer::blockStatement(ListNode* node, MutableHandleValue dst) {
  MOZ_ASSERT(node->isKind(ParseNodeKind::StatementList));

  NodeVector stmts(cx);
  return statements(node, stmts) &&
         builder.blockStatement(stmts, &node->pn_pos, dst);
}

bool ASTSerializer::program(ListNode* node, MutableHandleValue dst) {
#ifdef DEBUG
  {
    const TokenStreamAnyChars& anyChars = parser->anyChars;
    auto lineToken = anyChars.lineToken(node->pn_pos.begin);
    MOZ_ASSERT(anyChars.lineNumber(lineToken) == lineno);
  }
#endif

  NodeVector stmts(cx);
  return statements(node, stmts) && builder.program(stmts, &node->pn_pos, dst);
}

bool ASTSerializer::sourceElement(ParseNode* pn, MutableHandleValue dst) {
  /* SpiderMonkey allows declarations even in pure statement contexts. */
  return statement(pn, dst);
}

bool ASTSerializer::declaration(ParseNode* pn, MutableHandleValue dst) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::Function) ||
             pn->isKind(ParseNodeKind::VarStmt) ||
             pn->isKind(ParseNodeKind::LetDecl) ||
             pn->isKind(ParseNodeKind::ConstDecl));

  switch (pn->getKind()) {
    case ParseNodeKind::Function:
      return function(&pn->as<FunctionNode>(), AST_FUNC_DECL, dst);

    case ParseNodeKind::VarStmt:
      return variableDeclaration(&pn->as<ListNode>(), false, dst);

    default:
      MOZ_ASSERT(pn->isKind(ParseNodeKind::LetDecl) ||
                 pn->isKind(ParseNodeKind::ConstDecl));
      return variableDeclaration(&pn->as<ListNode>(), true, dst);
  }
}

bool ASTSerializer::variableDeclaration(ListNode* declList, bool lexical,
                                        MutableHandleValue dst) {
  MOZ_ASSERT_IF(lexical, declList->isKind(ParseNodeKind::LetDecl) ||
                             declList->isKind(ParseNodeKind::ConstDecl));
  MOZ_ASSERT_IF(!lexical, declList->isKind(ParseNodeKind::VarStmt));

  VarDeclKind kind = VARDECL_ERR;
  // Treat both the toplevel const binding (secretly var-like) and the lexical
  // const the same way
  if (lexical) {
    kind =
        declList->isKind(ParseNodeKind::LetDecl) ? VARDECL_LET : VARDECL_CONST;
  } else {
    kind =
        declList->isKind(ParseNodeKind::VarStmt) ? VARDECL_VAR : VARDECL_CONST;
  }

  NodeVector dtors(cx);
  if (!dtors.reserve(declList->count())) {
    return false;
  }
  for (ParseNode* decl : declList->contents()) {
    RootedValue child(cx);
    if (!variableDeclarator(decl, &child)) {
      return false;
    }
    dtors.infallibleAppend(child);
  }
  return builder.variableDeclaration(dtors, kind, &declList->pn_pos, dst);
}

bool ASTSerializer::variableDeclarator(ParseNode* pn, MutableHandleValue dst) {
  ParseNode* patternNode;
  ParseNode* initNode;

  if (pn->isKind(ParseNodeKind::Name)) {
    patternNode = pn;
    initNode = nullptr;
  } else if (pn->isKind(ParseNodeKind::AssignExpr)) {
    AssignmentNode* assignNode = &pn->as<AssignmentNode>();
    patternNode = assignNode->left();
    initNode = assignNode->right();
    MOZ_ASSERT(pn->pn_pos.encloses(patternNode->pn_pos));
    MOZ_ASSERT(pn->pn_pos.encloses(initNode->pn_pos));
  } else {
    /* This happens for a destructuring declarator in a for-in/of loop. */
    patternNode = pn;
    initNode = nullptr;
  }

  RootedValue patternVal(cx), init(cx);
  return pattern(patternNode, &patternVal) && optExpression(initNode, &init) &&
         builder.variableDeclarator(patternVal, init, &pn->pn_pos, dst);
}

bool ASTSerializer::importDeclaration(BinaryNode* importNode,
                                      MutableHandleValue dst) {
  MOZ_ASSERT(importNode->isKind(ParseNodeKind::ImportDecl));

  ListNode* specList = &importNode->left()->as<ListNode>();
  MOZ_ASSERT(specList->isKind(ParseNodeKind::ImportSpecList));

  ParseNode* moduleSpecNode = importNode->right();
  MOZ_ASSERT(moduleSpecNode->isKind(ParseNodeKind::StringExpr));

  NodeVector elts(cx);
  if (!elts.reserve(specList->count())) {
    return false;
  }

  for (ParseNode* item : specList->contents()) {
    RootedValue elt(cx);
    if (item->is<UnaryNode>()) {
      auto* spec = &item->as<UnaryNode>();
      if (!importNamespaceSpecifier(spec, &elt)) {
        return false;
      }
    } else {
      auto* spec = &item->as<BinaryNode>();
      if (!importSpecifier(spec, &elt)) {
        return false;
      }
    }
    elts.infallibleAppend(elt);
  }

  RootedValue moduleSpec(cx);
  return literal(moduleSpecNode, &moduleSpec) &&
         builder.importDeclaration(elts, moduleSpec, &importNode->pn_pos, dst);
}

bool ASTSerializer::importSpecifier(BinaryNode* importSpec,
                                    MutableHandleValue dst) {
  MOZ_ASSERT(importSpec->isKind(ParseNodeKind::ImportSpec));
  NameNode* importNameNode = &importSpec->left()->as<NameNode>();
  NameNode* bindingNameNode = &importSpec->right()->as<NameNode>();

  RootedValue importName(cx);
  RootedValue bindingName(cx);
  return identifierOrLiteral(importNameNode, &importName) &&
         identifier(bindingNameNode, &bindingName) &&
         builder.importSpecifier(importName, bindingName, &importSpec->pn_pos,
                                 dst);
}

bool ASTSerializer::importNamespaceSpecifier(UnaryNode* importSpec,
                                             MutableHandleValue dst) {
  MOZ_ASSERT(importSpec->isKind(ParseNodeKind::ImportNamespaceSpec));
  NameNode* bindingNameNode = &importSpec->kid()->as<NameNode>();

  RootedValue bindingName(cx);
  return identifier(bindingNameNode, &bindingName) &&
         builder.importNamespaceSpecifier(bindingName, &importSpec->pn_pos,
                                          dst);
}

bool ASTSerializer::exportDeclaration(ParseNode* exportNode,
                                      MutableHandleValue dst) {
  MOZ_ASSERT(exportNode->isKind(ParseNodeKind::ExportStmt) ||
             exportNode->isKind(ParseNodeKind::ExportFromStmt) ||
             exportNode->isKind(ParseNodeKind::ExportDefaultStmt));
  MOZ_ASSERT_IF(exportNode->isKind(ParseNodeKind::ExportStmt),
                exportNode->is<UnaryNode>());
  MOZ_ASSERT_IF(
      exportNode->isKind(ParseNodeKind::ExportFromStmt),
      exportNode->as<BinaryNode>().right()->isKind(ParseNodeKind::StringExpr));

  RootedValue decl(cx, NullValue());
  NodeVector elts(cx);

  ParseNode* kid = exportNode->isKind(ParseNodeKind::ExportStmt)
                       ? exportNode->as<UnaryNode>().kid()
                       : exportNode->as<BinaryNode>().left();
  switch (ParseNodeKind kind = kid->getKind()) {
    case ParseNodeKind::ExportSpecList: {
      ListNode* specList = &kid->as<ListNode>();
      if (!elts.reserve(specList->count())) {
        return false;
      }

      for (ParseNode* spec : specList->contents()) {
        RootedValue elt(cx);
        if (spec->isKind(ParseNodeKind::ExportSpec)) {
          if (!exportSpecifier(&spec->as<BinaryNode>(), &elt)) {
            return false;
          }
        } else if (spec->isKind(ParseNodeKind::ExportNamespaceSpec)) {
          if (!exportNamespaceSpecifier(&spec->as<UnaryNode>(), &elt)) {
            return false;
          }
        } else {
          MOZ_ASSERT(spec->isKind(ParseNodeKind::ExportBatchSpecStmt));
          if (!builder.exportBatchSpecifier(&exportNode->pn_pos, &elt)) {
            return false;
          }
        }
        elts.infallibleAppend(elt);
      }
      break;
    }

    case ParseNodeKind::Function:
      if (!function(&kid->as<FunctionNode>(), AST_FUNC_DECL, &decl)) {
        return false;
      }
      break;

    case ParseNodeKind::ClassDecl:
      if (!classDefinition(&kid->as<ClassNode>(), false, &decl)) {
        return false;
      }
      break;

    case ParseNodeKind::VarStmt:
    case ParseNodeKind::ConstDecl:
    case ParseNodeKind::LetDecl:
      if (!variableDeclaration(&kid->as<ListNode>(),
                               kind != ParseNodeKind::VarStmt, &decl)) {
        return false;
      }
      break;

    default:
      if (!expression(kid, &decl)) {
        return false;
      }
      break;
  }

  RootedValue moduleSpec(cx, NullValue());
  if (exportNode->isKind(ParseNodeKind::ExportFromStmt)) {
    if (!literal(exportNode->as<BinaryNode>().right(), &moduleSpec)) {
      return false;
    }
  }

  RootedValue isDefault(cx, BooleanValue(false));
  if (exportNode->isKind(ParseNodeKind::ExportDefaultStmt)) {
    isDefault.setBoolean(true);
  }

  return builder.exportDeclaration(decl, elts, moduleSpec, isDefault,
                                   &exportNode->pn_pos, dst);
}

bool ASTSerializer::exportSpecifier(BinaryNode* exportSpec,
                                    MutableHandleValue dst) {
  MOZ_ASSERT(exportSpec->isKind(ParseNodeKind::ExportSpec));
  NameNode* bindingNameNode = &exportSpec->left()->as<NameNode>();
  NameNode* exportNameNode = &exportSpec->right()->as<NameNode>();

  RootedValue bindingName(cx);
  RootedValue exportName(cx);
  return identifierOrLiteral(bindingNameNode, &bindingName) &&
         identifierOrLiteral(exportNameNode, &exportName) &&
         builder.exportSpecifier(bindingName, exportName, &exportSpec->pn_pos,
                                 dst);
}

bool ASTSerializer::exportNamespaceSpecifier(UnaryNode* exportSpec,
                                             MutableHandleValue dst) {
  MOZ_ASSERT(exportSpec->isKind(ParseNodeKind::ExportNamespaceSpec));
  NameNode* exportNameNode = &exportSpec->kid()->as<NameNode>();

  RootedValue exportName(cx);
  return identifierOrLiteral(exportNameNode, &exportName) &&
         builder.exportNamespaceSpecifier(exportName, &exportSpec->pn_pos, dst);
}

bool ASTSerializer::switchCase(CaseClause* caseClause, MutableHandleValue dst) {
  MOZ_ASSERT_IF(
      caseClause->caseExpression(),
      caseClause->pn_pos.encloses(caseClause->caseExpression()->pn_pos));
  MOZ_ASSERT(caseClause->pn_pos.encloses(caseClause->statementList()->pn_pos));

  NodeVector stmts(cx);
  RootedValue expr(cx);
  return optExpression(caseClause->caseExpression(), &expr) &&
         statements(caseClause->statementList(), stmts) &&
         builder.switchCase(expr, stmts, &caseClause->pn_pos, dst);
}

bool ASTSerializer::switchStatement(SwitchStatement* switchStmt,
                                    MutableHandleValue dst) {
  MOZ_ASSERT(switchStmt->pn_pos.encloses(switchStmt->discriminant().pn_pos));
  MOZ_ASSERT(
      switchStmt->pn_pos.encloses(switchStmt->lexicalForCaseList().pn_pos));

  RootedValue disc(cx);
  if (!expression(&switchStmt->discriminant(), &disc)) {
    return false;
  }

  ListNode* caseList =
      &switchStmt->lexicalForCaseList().scopeBody()->as<ListNode>();

  NodeVector cases(cx);
  if (!cases.reserve(caseList->count())) {
    return false;
  }

  for (ParseNode* item : caseList->contents()) {
    CaseClause* caseClause = &item->as<CaseClause>();
    RootedValue child(cx);
    if (!switchCase(caseClause, &child)) {
      return false;
    }
    cases.infallibleAppend(child);
  }

  // `lexical` field is always true.
  return builder.switchStatement(disc, cases, true, &switchStmt->pn_pos, dst);
}

bool ASTSerializer::catchClause(BinaryNode* catchClause,
                                MutableHandleValue dst) {
  MOZ_ASSERT(catchClause->isKind(ParseNodeKind::Catch));

  ParseNode* varNode = catchClause->left();
  MOZ_ASSERT_IF(varNode, catchClause->pn_pos.encloses(varNode->pn_pos));

  ParseNode* bodyNode = catchClause->right();
  MOZ_ASSERT(catchClause->pn_pos.encloses(bodyNode->pn_pos));

  RootedValue var(cx), body(cx);
  if (!optPattern(varNode, &var)) {
    return false;
  }

  return statement(bodyNode, &body) &&
         builder.catchClause(var, body, &catchClause->pn_pos, dst);
}

bool ASTSerializer::tryStatement(TryNode* tryNode, MutableHandleValue dst) {
  ParseNode* bodyNode = tryNode->body();
  MOZ_ASSERT(tryNode->pn_pos.encloses(bodyNode->pn_pos));

  LexicalScopeNode* catchNode = tryNode->catchScope();
  MOZ_ASSERT_IF(catchNode, tryNode->pn_pos.encloses(catchNode->pn_pos));

  ParseNode* finallyNode = tryNode->finallyBlock();
  MOZ_ASSERT_IF(finallyNode, tryNode->pn_pos.encloses(finallyNode->pn_pos));

  RootedValue body(cx);
  if (!statement(bodyNode, &body)) {
    return false;
  }

  RootedValue handler(cx, NullValue());
  if (catchNode) {
    LexicalScopeNode* catchScope = &catchNode->as<LexicalScopeNode>();
    if (!catchClause(&catchScope->scopeBody()->as<BinaryNode>(), &handler)) {
      return false;
    }
  }

  RootedValue finally(cx);
  return optStatement(finallyNode, &finally) &&
         builder.tryStatement(body, handler, finally, &tryNode->pn_pos, dst);
}

bool ASTSerializer::forInit(ParseNode* pn, MutableHandleValue dst) {
  if (!pn) {
    dst.setMagic(JS_SERIALIZE_NO_NODE);
    return true;
  }

  bool lexical = pn->isKind(ParseNodeKind::LetDecl) ||
                 pn->isKind(ParseNodeKind::ConstDecl);
  return (lexical || pn->isKind(ParseNodeKind::VarStmt))
             ? variableDeclaration(&pn->as<ListNode>(), lexical, dst)
             : expression(pn, dst);
}

bool ASTSerializer::forOf(ForNode* loop, ParseNode* iterExpr, HandleValue var,
                          HandleValue stmt, MutableHandleValue dst) {
  RootedValue expr(cx);

  return expression(iterExpr, &expr) &&
         builder.forOfStatement(var, expr, stmt, &loop->pn_pos, dst);
}

bool ASTSerializer::forIn(ForNode* loop, ParseNode* iterExpr, HandleValue var,
                          HandleValue stmt, MutableHandleValue dst) {
  RootedValue expr(cx);

  return expression(iterExpr, &expr) &&
         builder.forInStatement(var, expr, stmt, &loop->pn_pos, dst);
}

bool ASTSerializer::classDefinition(ClassNode* pn, bool expr,
                                    MutableHandleValue dst) {
  RootedValue className(cx, MagicValue(JS_SERIALIZE_NO_NODE));
  RootedValue heritage(cx);
  RootedValue classBody(cx);

  if (ClassNames* names = pn->names()) {
    if (!identifier(names->innerBinding(), &className)) {
      return false;
    }
  }

  return optExpression(pn->heritage(), &heritage) &&
         statement(pn->memberList(), &classBody) &&
         builder.classDefinition(expr, className, heritage, classBody,
                                 &pn->pn_pos, dst);
}

bool ASTSerializer::statement(ParseNode* pn, MutableHandleValue dst) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  switch (pn->getKind()) {
    case ParseNodeKind::Function:
    case ParseNodeKind::VarStmt:
      return declaration(pn, dst);

    case ParseNodeKind::LetDecl:
    case ParseNodeKind::ConstDecl:
      return declaration(pn, dst);

    case ParseNodeKind::ImportDecl:
      return importDeclaration(&pn->as<BinaryNode>(), dst);

    case ParseNodeKind::ExportStmt:
    case ParseNodeKind::ExportDefaultStmt:
    case ParseNodeKind::ExportFromStmt:
      return exportDeclaration(pn, dst);

    case ParseNodeKind::EmptyStmt:
      return builder.emptyStatement(&pn->pn_pos, dst);

    case ParseNodeKind::ExpressionStmt: {
      RootedValue expr(cx);
      return expression(pn->as<UnaryNode>().kid(), &expr) &&
             builder.expressionStatement(expr, &pn->pn_pos, dst);
    }

    case ParseNodeKind::LexicalScope:
      pn = pn->as<LexicalScopeNode>().scopeBody();
      if (!pn->isKind(ParseNodeKind::StatementList)) {
        return statement(pn, dst);
      }
      [[fallthrough]];

    case ParseNodeKind::StatementList:
      return blockStatement(&pn->as<ListNode>(), dst);

    case ParseNodeKind::IfStmt: {
      TernaryNode* ifNode = &pn->as<TernaryNode>();

      ParseNode* testNode = ifNode->kid1();
      MOZ_ASSERT(ifNode->pn_pos.encloses(testNode->pn_pos));

      ParseNode* consNode = ifNode->kid2();
      MOZ_ASSERT(ifNode->pn_pos.encloses(consNode->pn_pos));

      ParseNode* altNode = ifNode->kid3();
      MOZ_ASSERT_IF(altNode, ifNode->pn_pos.encloses(altNode->pn_pos));

      RootedValue test(cx), cons(cx), alt(cx);

      return expression(testNode, &test) && statement(consNode, &cons) &&
             optStatement(altNode, &alt) &&
             builder.ifStatement(test, cons, alt, &ifNode->pn_pos, dst);
    }

    case ParseNodeKind::SwitchStmt:
      return switchStatement(&pn->as<SwitchStatement>(), dst);

    case ParseNodeKind::TryStmt:
      return tryStatement(&pn->as<TryNode>(), dst);

    case ParseNodeKind::WithStmt:
    case ParseNodeKind::WhileStmt: {
      BinaryNode* node = &pn->as<BinaryNode>();

      ParseNode* exprNode = node->left();
      MOZ_ASSERT(node->pn_pos.encloses(exprNode->pn_pos));

      ParseNode* stmtNode = node->right();
      MOZ_ASSERT(node->pn_pos.encloses(stmtNode->pn_pos));

      RootedValue expr(cx), stmt(cx);

      return expression(exprNode, &expr) && statement(stmtNode, &stmt) &&
             (node->isKind(ParseNodeKind::WithStmt)
                  ? builder.withStatement(expr, stmt, &node->pn_pos, dst)
                  : builder.whileStatement(expr, stmt, &node->pn_pos, dst));
    }

    case ParseNodeKind::DoWhileStmt: {
      BinaryNode* node = &pn->as<BinaryNode>();

      ParseNode* stmtNode = node->left();
      MOZ_ASSERT(node->pn_pos.encloses(stmtNode->pn_pos));

      ParseNode* testNode = node->right();
      MOZ_ASSERT(node->pn_pos.encloses(testNode->pn_pos));

      RootedValue stmt(cx), test(cx);

      return statement(stmtNode, &stmt) && expression(testNode, &test) &&
             builder.doWhileStatement(stmt, test, &node->pn_pos, dst);
    }

    case ParseNodeKind::ForStmt: {
      ForNode* forNode = &pn->as<ForNode>();

      TernaryNode* head = forNode->head();
      MOZ_ASSERT(forNode->pn_pos.encloses(head->pn_pos));

      ParseNode* stmtNode = forNode->right();
      MOZ_ASSERT(forNode->pn_pos.encloses(stmtNode->pn_pos));

      ParseNode* initNode = head->kid1();
      MOZ_ASSERT_IF(initNode, head->pn_pos.encloses(initNode->pn_pos));

      ParseNode* maybeTest = head->kid2();
      MOZ_ASSERT_IF(maybeTest, head->pn_pos.encloses(maybeTest->pn_pos));

      ParseNode* updateOrIter = head->kid3();
      MOZ_ASSERT_IF(updateOrIter, head->pn_pos.encloses(updateOrIter->pn_pos));

      RootedValue stmt(cx);
      if (!statement(stmtNode, &stmt)) {
        return false;
      }

      if (head->isKind(ParseNodeKind::ForIn) ||
          head->isKind(ParseNodeKind::ForOf)) {
        RootedValue var(cx);
        if (initNode->is<LexicalScopeNode>()) {
          LexicalScopeNode* scopeNode = &initNode->as<LexicalScopeNode>();
          if (!variableDeclaration(&scopeNode->scopeBody()->as<ListNode>(),
                                   true, &var)) {
            return false;
          }
        } else if (!initNode->isKind(ParseNodeKind::VarStmt) &&
                   !initNode->isKind(ParseNodeKind::LetDecl) &&
                   !initNode->isKind(ParseNodeKind::ConstDecl)) {
          if (!pattern(initNode, &var)) {
            return false;
          }
        } else {
          if (!variableDeclaration(
                  &initNode->as<ListNode>(),
                  initNode->isKind(ParseNodeKind::LetDecl) ||
                      initNode->isKind(ParseNodeKind::ConstDecl),
                  &var)) {
            return false;
          }
        }
        if (head->isKind(ParseNodeKind::ForIn)) {
          return forIn(forNode, updateOrIter, var, stmt, dst);
        }
        return forOf(forNode, updateOrIter, var, stmt, dst);
      }

      RootedValue init(cx), test(cx), update(cx);

      return forInit(initNode, &init) && optExpression(maybeTest, &test) &&
             optExpression(updateOrIter, &update) &&
             builder.forStatement(init, test, update, stmt, &forNode->pn_pos,
                                  dst);
    }

    case ParseNodeKind::BreakStmt:
    case ParseNodeKind::ContinueStmt: {
      LoopControlStatement* node = &pn->as<LoopControlStatement>();
      RootedValue label(cx);
      RootedAtom pnAtom(cx);
      if (node->label()) {
        pnAtom.set(parser->liftParserAtomToJSAtom(node->label()));
        if (!pnAtom) {
          return false;
        }
      }
      return optIdentifier(pnAtom, nullptr, &label) &&
             (node->isKind(ParseNodeKind::BreakStmt)
                  ? builder.breakStatement(label, &node->pn_pos, dst)
                  : builder.continueStatement(label, &node->pn_pos, dst));
    }

    case ParseNodeKind::LabelStmt: {
      LabeledStatement* labelNode = &pn->as<LabeledStatement>();
      ParseNode* stmtNode = labelNode->statement();
      MOZ_ASSERT(labelNode->pn_pos.encloses(stmtNode->pn_pos));

      RootedValue label(cx), stmt(cx);
      RootedAtom pnAtom(cx, parser->liftParserAtomToJSAtom(labelNode->label()));
      if (!pnAtom.get()) {
        return false;
      }
      return identifier(pnAtom, nullptr, &label) &&
             statement(stmtNode, &stmt) &&
             builder.labeledStatement(label, stmt, &labelNode->pn_pos, dst);
    }

    case ParseNodeKind::ThrowStmt: {
      UnaryNode* throwNode = &pn->as<UnaryNode>();
      ParseNode* operand = throwNode->kid();
      MOZ_ASSERT(throwNode->pn_pos.encloses(operand->pn_pos));

      RootedValue arg(cx);

      return expression(operand, &arg) &&
             builder.throwStatement(arg, &throwNode->pn_pos, dst);
    }

    case ParseNodeKind::ReturnStmt: {
      UnaryNode* returnNode = &pn->as<UnaryNode>();
      ParseNode* operand = returnNode->kid();
      MOZ_ASSERT_IF(operand, returnNode->pn_pos.encloses(operand->pn_pos));

      RootedValue arg(cx);

      return optExpression(operand, &arg) &&
             builder.returnStatement(arg, &returnNode->pn_pos, dst);
    }

    case ParseNodeKind::DebuggerStmt:
      return builder.debuggerStatement(&pn->pn_pos, dst);

    case ParseNodeKind::ClassDecl:
      return classDefinition(&pn->as<ClassNode>(), false, dst);

    case ParseNodeKind::ClassMemberList: {
      ListNode* memberList = &pn->as<ListNode>();
      NodeVector members(cx);
      if (!members.reserve(memberList->count())) {
        return false;
      }

      for (ParseNode* item : memberList->contents()) {
        if (item->is<LexicalScopeNode>()) {
          item = item->as<LexicalScopeNode>().scopeBody();
        }
        if (item->is<ClassField>()) {
          ClassField* field = &item->as<ClassField>();
          MOZ_ASSERT(memberList->pn_pos.encloses(field->pn_pos));

          RootedValue prop(cx);
          if (!classField(field, &prop)) {
            return false;
          }
          members.infallibleAppend(prop);
        } else if (item->is<StaticClassBlock>()) {
          // StaticClassBlock* block = &item->as<StaticClassBlock>();
          StaticClassBlock* scb = &item->as<StaticClassBlock>();
          MOZ_ASSERT(memberList->pn_pos.encloses(scb->pn_pos));
          RootedValue prop(cx);
          if (!staticClassBlock(scb, &prop)) {
            return false;
          }
          members.infallibleAppend(prop);
        } else if (!item->isKind(ParseNodeKind::DefaultConstructor)) {
          ClassMethod* method = &item->as<ClassMethod>();
          MOZ_ASSERT(memberList->pn_pos.encloses(method->pn_pos));

          RootedValue prop(cx);
          if (!classMethod(method, &prop)) {
            return false;
          }
          members.infallibleAppend(prop);
        }
      }

      return builder.classMembers(members, dst);
    }

    default:
      LOCAL_NOT_REACHED("unexpected statement type");
  }
}

bool ASTSerializer::classMethod(ClassMethod* classMethod,
                                MutableHandleValue dst) {
  PropKind kind;
  switch (classMethod->accessorType()) {
    case AccessorType::None:
      kind = PROP_INIT;
      break;

    case AccessorType::Getter:
      kind = PROP_GETTER;
      break;

    case AccessorType::Setter:
      kind = PROP_SETTER;
      break;

    default:
      LOCAL_NOT_REACHED("unexpected object-literal property");
  }

  RootedValue key(cx), val(cx);
  bool isStatic = classMethod->isStatic();
  return propertyName(&classMethod->name(), &key) &&
         expression(&classMethod->method(), &val) &&
         builder.classMethod(key, val, kind, isStatic, &classMethod->pn_pos,
                             dst);
}

bool ASTSerializer::classField(ClassField* classField, MutableHandleValue dst) {
  RootedValue key(cx), val(cx);
  // Dig through the lambda and get to the actual expression
  ParseNode* value = classField->initializer()
                         ->body()
                         ->head()
                         ->as<LexicalScopeNode>()
                         .scopeBody()
                         ->as<ListNode>()
                         .head()
                         ->as<UnaryNode>()
                         .kid()
                         ->as<BinaryNode>()
                         .right();
  // RawUndefinedExpr is the node we use for "there is no initializer". If one
  // writes, literally, `x = undefined;`, it will not be a RawUndefinedExpr
  // node, but rather a variable reference.
  // Behavior for "there is no initializer" should be { ..., "init": null }
  if (value->getKind() != ParseNodeKind::RawUndefinedExpr) {
    if (!expression(value, &val)) {
      return false;
    }
  } else {
    val.setNull();
  }
  return propertyName(&classField->name(), &key) &&
         builder.classField(key, val, &classField->pn_pos, dst);
}

bool ASTSerializer::staticClassBlock(StaticClassBlock* staticClassBlock,
                                     MutableHandleValue dst) {
  FunctionNode* fun = staticClassBlock->function();

  NodeVector args(cx);
  NodeVector defaults(cx);

  RootedValue body(cx), rest(cx);
  rest.setNull();
  return functionArgsAndBody(fun->body(), args, defaults, false, false, &body,
                             &rest) &&
         builder.staticClassBlock(body, &staticClassBlock->pn_pos, dst);
}

bool ASTSerializer::leftAssociate(ListNode* node, MutableHandleValue dst) {
  MOZ_ASSERT(!node->empty());

  ParseNodeKind pnk = node->getKind();
  bool lor = pnk == ParseNodeKind::OrExpr;
  bool coalesce = pnk == ParseNodeKind::CoalesceExpr;
  bool logop = lor || coalesce || pnk == ParseNodeKind::AndExpr;

  ParseNode* head = node->head();
  RootedValue left(cx);
  if (!expression(head, &left)) {
    return false;
  }
  for (ParseNode* next : node->contentsFrom(head->pn_next)) {
    RootedValue right(cx);
    if (!expression(next, &right)) {
      return false;
    }

    TokenPos subpos(node->pn_pos.begin, next->pn_pos.end);

    if (logop) {
      if (!builder.logicalExpression(pnk, left, right, &subpos, &left)) {
        return false;
      }
    } else {
      BinaryOperator op = binop(node->getKind());
      LOCAL_ASSERT(op > BINOP_ERR && op < BINOP_LIMIT);

      if (!builder.binaryExpression(op, left, right, &subpos, &left)) {
        return false;
      }
    }
  }

  dst.set(left);
  return true;
}

bool ASTSerializer::rightAssociate(ListNode* node, MutableHandleValue dst) {
  MOZ_ASSERT(!node->empty());

  // First, we need to reverse the list, so that we can traverse it in the right
  // order. It's OK to destructively reverse the list, because there are no
  // other consumers.

  ParseNode* head = node->head();
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
  if (!expression(head, &right)) {
    return false;
  }
  for (ParseNode* next = head->pn_next; next; next = next->pn_next) {
    RootedValue left(cx);
    if (!expression(next, &left)) {
      return false;
    }

    TokenPos subpos(node->pn_pos.begin, next->pn_pos.end);

    BinaryOperator op = binop(node->getKind());
    LOCAL_ASSERT(op > BINOP_ERR && op < BINOP_LIMIT);

    if (!builder.binaryExpression(op, left, right, &subpos, &right)) {
      return false;
    }
  }

  dst.set(right);
  return true;
}

bool ASTSerializer::expression(ParseNode* pn, MutableHandleValue dst) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  switch (pn->getKind()) {
    case ParseNodeKind::Function: {
      FunctionNode* funNode = &pn->as<FunctionNode>();
      ASTType type =
          funNode->funbox()->isArrow() ? AST_ARROW_EXPR : AST_FUNC_EXPR;
      return function(funNode, type, dst);
    }

    case ParseNodeKind::CommaExpr: {
      NodeVector exprs(cx);
      return expressions(&pn->as<ListNode>(), exprs) &&
             builder.sequenceExpression(exprs, &pn->pn_pos, dst);
    }

    case ParseNodeKind::ConditionalExpr: {
      ConditionalExpression* condNode = &pn->as<ConditionalExpression>();
      ParseNode* testNode = condNode->kid1();
      ParseNode* consNode = condNode->kid2();
      ParseNode* altNode = condNode->kid3();
      MOZ_ASSERT(condNode->pn_pos.encloses(testNode->pn_pos));
      MOZ_ASSERT(condNode->pn_pos.encloses(consNode->pn_pos));
      MOZ_ASSERT(condNode->pn_pos.encloses(altNode->pn_pos));

      RootedValue test(cx), cons(cx), alt(cx);

      return expression(testNode, &test) && expression(consNode, &cons) &&
             expression(altNode, &alt) &&
             builder.conditionalExpression(test, cons, alt, &condNode->pn_pos,
                                           dst);
    }

    case ParseNodeKind::CoalesceExpr:
    case ParseNodeKind::OrExpr:
    case ParseNodeKind::AndExpr:
      return leftAssociate(&pn->as<ListNode>(), dst);

    case ParseNodeKind::PreIncrementExpr:
    case ParseNodeKind::PreDecrementExpr: {
      UnaryNode* incDec = &pn->as<UnaryNode>();
      ParseNode* operand = incDec->kid();
      MOZ_ASSERT(incDec->pn_pos.encloses(operand->pn_pos));

      bool inc = incDec->isKind(ParseNodeKind::PreIncrementExpr);
      RootedValue expr(cx);
      return expression(operand, &expr) &&
             builder.updateExpression(expr, inc, true, &incDec->pn_pos, dst);
    }

    case ParseNodeKind::PostIncrementExpr:
    case ParseNodeKind::PostDecrementExpr: {
      UnaryNode* incDec = &pn->as<UnaryNode>();
      ParseNode* operand = incDec->kid();
      MOZ_ASSERT(incDec->pn_pos.encloses(operand->pn_pos));

      bool inc = incDec->isKind(ParseNodeKind::PostIncrementExpr);
      RootedValue expr(cx);
      return expression(operand, &expr) &&
             builder.updateExpression(expr, inc, false, &incDec->pn_pos, dst);
    }

    case ParseNodeKind::AssignExpr:
    case ParseNodeKind::AddAssignExpr:
    case ParseNodeKind::SubAssignExpr:
    case ParseNodeKind::CoalesceAssignExpr:
    case ParseNodeKind::OrAssignExpr:
    case ParseNodeKind::AndAssignExpr:
    case ParseNodeKind::BitOrAssignExpr:
    case ParseNodeKind::BitXorAssignExpr:
    case ParseNodeKind::BitAndAssignExpr:
    case ParseNodeKind::LshAssignExpr:
    case ParseNodeKind::RshAssignExpr:
    case ParseNodeKind::UrshAssignExpr:
    case ParseNodeKind::MulAssignExpr:
    case ParseNodeKind::DivAssignExpr:
    case ParseNodeKind::ModAssignExpr:
    case ParseNodeKind::PowAssignExpr: {
      AssignmentNode* assignNode = &pn->as<AssignmentNode>();
      ParseNode* lhsNode = assignNode->left();
      ParseNode* rhsNode = assignNode->right();
      MOZ_ASSERT(assignNode->pn_pos.encloses(lhsNode->pn_pos));
      MOZ_ASSERT(assignNode->pn_pos.encloses(rhsNode->pn_pos));

      AssignmentOperator op = aop(assignNode->getKind());
      LOCAL_ASSERT(op > AOP_ERR && op < AOP_LIMIT);

      RootedValue lhs(cx), rhs(cx);
      return pattern(lhsNode, &lhs) && expression(rhsNode, &rhs) &&
             builder.assignmentExpression(op, lhs, rhs, &assignNode->pn_pos,
                                          dst);
    }

    case ParseNodeKind::AddExpr:
    case ParseNodeKind::SubExpr:
    case ParseNodeKind::StrictEqExpr:
    case ParseNodeKind::EqExpr:
    case ParseNodeKind::StrictNeExpr:
    case ParseNodeKind::NeExpr:
    case ParseNodeKind::LtExpr:
    case ParseNodeKind::LeExpr:
    case ParseNodeKind::GtExpr:
    case ParseNodeKind::GeExpr:
    case ParseNodeKind::LshExpr:
    case ParseNodeKind::RshExpr:
    case ParseNodeKind::UrshExpr:
    case ParseNodeKind::MulExpr:
    case ParseNodeKind::DivExpr:
    case ParseNodeKind::ModExpr:
    case ParseNodeKind::BitOrExpr:
    case ParseNodeKind::BitXorExpr:
    case ParseNodeKind::BitAndExpr:
    case ParseNodeKind::InExpr:
    case ParseNodeKind::PrivateInExpr:
    case ParseNodeKind::InstanceOfExpr:
      return leftAssociate(&pn->as<ListNode>(), dst);

    case ParseNodeKind::PowExpr:
      return rightAssociate(&pn->as<ListNode>(), dst);

    case ParseNodeKind::DeleteNameExpr:
    case ParseNodeKind::DeletePropExpr:
    case ParseNodeKind::DeleteElemExpr:
    case ParseNodeKind::DeleteExpr:
    case ParseNodeKind::TypeOfNameExpr:
    case ParseNodeKind::TypeOfExpr:
    case ParseNodeKind::VoidExpr:
    case ParseNodeKind::NotExpr:
    case ParseNodeKind::BitNotExpr:
    case ParseNodeKind::PosExpr:
    case ParseNodeKind::AwaitExpr:
    case ParseNodeKind::NegExpr: {
      UnaryNode* unaryNode = &pn->as<UnaryNode>();
      ParseNode* operand = unaryNode->kid();
      MOZ_ASSERT(unaryNode->pn_pos.encloses(operand->pn_pos));

      UnaryOperator op = unop(unaryNode->getKind());
      LOCAL_ASSERT(op > UNOP_ERR && op < UNOP_LIMIT);

      RootedValue expr(cx);
      return expression(operand, &expr) &&
             builder.unaryExpression(op, expr, &unaryNode->pn_pos, dst);
    }

    case ParseNodeKind::DeleteOptionalChainExpr: {
      RootedValue expr(cx);
      return expression(pn->as<UnaryNode>().kid(), &expr) &&
             builder.deleteOptionalExpression(expr, &pn->pn_pos, dst);
    }

    case ParseNodeKind::OptionalChain: {
      RootedValue expr(cx);
      return expression(pn->as<UnaryNode>().kid(), &expr) &&
             builder.optionalExpression(expr, &pn->pn_pos, dst);
    }

    case ParseNodeKind::NewExpr:
    case ParseNodeKind::TaggedTemplateExpr:
    case ParseNodeKind::CallExpr:
    case ParseNodeKind::OptionalCallExpr:
    case ParseNodeKind::SuperCallExpr: {
      BinaryNode* node = &pn->as<BinaryNode>();
      ParseNode* calleeNode = node->left();
      ListNode* argsList = &node->right()->as<ListNode>();
      MOZ_ASSERT(node->pn_pos.encloses(calleeNode->pn_pos));

      RootedValue callee(cx);
      if (node->isKind(ParseNodeKind::SuperCallExpr)) {
        MOZ_ASSERT(calleeNode->isKind(ParseNodeKind::SuperBase));
        if (!builder.super(&calleeNode->pn_pos, &callee)) {
          return false;
        }
      } else {
        if (!expression(calleeNode, &callee)) {
          return false;
        }
      }

      NodeVector args(cx);
      if (!args.reserve(argsList->count())) {
        return false;
      }

      for (ParseNode* argNode : argsList->contents()) {
        MOZ_ASSERT(node->pn_pos.encloses(argNode->pn_pos));

        RootedValue arg(cx);
        if (!expression(argNode, &arg)) {
          return false;
        }
        args.infallibleAppend(arg);
      }

      if (node->getKind() == ParseNodeKind::TaggedTemplateExpr) {
        return builder.taggedTemplate(callee, args, &node->pn_pos, dst);
      }

      bool isOptional = node->isKind(ParseNodeKind::OptionalCallExpr);

      // SUPERCALL is Call(super, args)
      return node->isKind(ParseNodeKind::NewExpr)
                 ? builder.newExpression(callee, args, &node->pn_pos, dst)
                 : builder.callExpression(callee, args, &node->pn_pos, dst,
                                          isOptional);
    }

    case ParseNodeKind::DotExpr:
    case ParseNodeKind::OptionalDotExpr: {
      PropertyAccessBase* prop = &pn->as<PropertyAccessBase>();
      MOZ_ASSERT(prop->pn_pos.encloses(prop->expression().pn_pos));

      bool isSuper =
          prop->is<PropertyAccess>() && prop->as<PropertyAccess>().isSuper();

      RootedValue expr(cx);
      RootedValue propname(cx);
      RootedAtom pnAtom(cx, parser->liftParserAtomToJSAtom(prop->key().atom()));
      if (!pnAtom.get()) {
        return false;
      }

      if (isSuper) {
        if (!builder.super(&prop->expression().pn_pos, &expr)) {
          return false;
        }
      } else {
        if (!expression(&prop->expression(), &expr)) {
          return false;
        }
      }

      bool isOptional = prop->isKind(ParseNodeKind::OptionalDotExpr);

      return identifier(pnAtom, nullptr, &propname) &&
             builder.memberExpression(false, expr, propname, &prop->pn_pos, dst,
                                      isOptional);
    }

    case ParseNodeKind::ElemExpr:
    case ParseNodeKind::OptionalElemExpr: {
      PropertyByValueBase* elem = &pn->as<PropertyByValueBase>();
      MOZ_ASSERT(elem->pn_pos.encloses(elem->expression().pn_pos));
      MOZ_ASSERT(elem->pn_pos.encloses(elem->key().pn_pos));

      bool isSuper =
          elem->is<PropertyByValue>() && elem->as<PropertyByValue>().isSuper();

      RootedValue expr(cx), key(cx);

      if (isSuper) {
        if (!builder.super(&elem->expression().pn_pos, &expr)) {
          return false;
        }
      } else {
        if (!expression(&elem->expression(), &expr)) {
          return false;
        }
      }

      bool isOptional = elem->isKind(ParseNodeKind::OptionalElemExpr);

      return expression(&elem->key(), &key) &&
             builder.memberExpression(true, expr, key, &elem->pn_pos, dst,
                                      isOptional);
    }

    case ParseNodeKind::PrivateMemberExpr:
    case ParseNodeKind::OptionalPrivateMemberExpr: {
      PrivateMemberAccessBase* privateExpr = &pn->as<PrivateMemberAccessBase>();
      MOZ_ASSERT(
          privateExpr->pn_pos.encloses(privateExpr->expression().pn_pos));
      MOZ_ASSERT(
          privateExpr->pn_pos.encloses(privateExpr->privateName().pn_pos));

      RootedValue expr(cx), key(cx);

      if (!expression(&privateExpr->expression(), &expr)) {
        return false;
      }

      bool isOptional =
          privateExpr->isKind(ParseNodeKind::OptionalPrivateMemberExpr);

      return expression(&privateExpr->privateName(), &key) &&
             builder.memberExpression(true, expr, key, &privateExpr->pn_pos,
                                      dst, isOptional);
    }

    case ParseNodeKind::CallSiteObj: {
      CallSiteNode* callSiteObj = &pn->as<CallSiteNode>();
      ListNode* rawNodes = callSiteObj->rawNodes();
      NodeVector raw(cx);
      if (!raw.reserve(rawNodes->count())) {
        return false;
      }
      for (ParseNode* item : rawNodes->contents()) {
        NameNode* rawItem = &item->as<NameNode>();
        MOZ_ASSERT(callSiteObj->pn_pos.encloses(rawItem->pn_pos));

        JSAtom* exprAtom = parser->liftParserAtomToJSAtom(rawItem->atom());
        if (!exprAtom) {
          return false;
        }
        RootedValue expr(cx, StringValue(exprAtom));
        raw.infallibleAppend(expr);
      }

      NodeVector cooked(cx);
      if (!cooked.reserve(callSiteObj->count() - 1)) {
        return false;
      }

      for (ParseNode* cookedItem :
           callSiteObj->contentsFrom(rawNodes->pn_next)) {
        MOZ_ASSERT(callSiteObj->pn_pos.encloses(cookedItem->pn_pos));

        RootedValue expr(cx);
        if (cookedItem->isKind(ParseNodeKind::RawUndefinedExpr)) {
          expr.setUndefined();
        } else {
          MOZ_ASSERT(cookedItem->isKind(ParseNodeKind::TemplateStringExpr));
          JSAtom* exprAtom =
              parser->liftParserAtomToJSAtom(cookedItem->as<NameNode>().atom());
          if (!exprAtom) {
            return false;
          }
          expr.setString(exprAtom);
        }
        cooked.infallibleAppend(expr);
      }

      return builder.callSiteObj(raw, cooked, &callSiteObj->pn_pos, dst);
    }

    case ParseNodeKind::ArrayExpr: {
      ListNode* array = &pn->as<ListNode>();
      NodeVector elts(cx);
      if (!elts.reserve(array->count())) {
        return false;
      }

      for (ParseNode* item : array->contents()) {
        MOZ_ASSERT(array->pn_pos.encloses(item->pn_pos));

        if (item->isKind(ParseNodeKind::Elision)) {
          elts.infallibleAppend(NullValue());
        } else {
          RootedValue expr(cx);
          if (!expression(item, &expr)) {
            return false;
          }
          elts.infallibleAppend(expr);
        }
      }

      return builder.arrayExpression(elts, &array->pn_pos, dst);
    }

    case ParseNodeKind::Spread: {
      RootedValue expr(cx);
      return expression(pn->as<UnaryNode>().kid(), &expr) &&
             builder.spreadExpression(expr, &pn->pn_pos, dst);
    }

    case ParseNodeKind::ComputedName: {
      if (pn->as<UnaryNode>().isSyntheticComputedName()) {
        return literal(pn->as<UnaryNode>().kid(), dst);
      }
      RootedValue name(cx);
      return expression(pn->as<UnaryNode>().kid(), &name) &&
             builder.computedName(name, &pn->pn_pos, dst);
    }

    case ParseNodeKind::ObjectExpr: {
      ListNode* obj = &pn->as<ListNode>();
      NodeVector elts(cx);
      if (!elts.reserve(obj->count())) {
        return false;
      }

      for (ParseNode* item : obj->contents()) {
        MOZ_ASSERT(obj->pn_pos.encloses(item->pn_pos));

        RootedValue prop(cx);
        if (!property(item, &prop)) {
          return false;
        }
        elts.infallibleAppend(prop);
      }

      return builder.objectExpression(elts, &obj->pn_pos, dst);
    }

    case ParseNodeKind::PrivateName:
    case ParseNodeKind::Name:
      return identifier(&pn->as<NameNode>(), dst);

    case ParseNodeKind::ThisExpr:
      return builder.thisExpression(&pn->pn_pos, dst);

    case ParseNodeKind::TemplateStringListExpr: {
      ListNode* list = &pn->as<ListNode>();
      NodeVector elts(cx);
      if (!elts.reserve(list->count())) {
        return false;
      }

      for (ParseNode* item : list->contents()) {
        MOZ_ASSERT(list->pn_pos.encloses(item->pn_pos));

        RootedValue expr(cx);
        if (!expression(item, &expr)) {
          return false;
        }
        elts.infallibleAppend(expr);
      }

      return builder.templateLiteral(elts, &list->pn_pos, dst);
    }

    case ParseNodeKind::TemplateStringExpr:
    case ParseNodeKind::StringExpr:
    case ParseNodeKind::RegExpExpr:
    case ParseNodeKind::NumberExpr:
    case ParseNodeKind::BigIntExpr:
    case ParseNodeKind::TrueExpr:
    case ParseNodeKind::FalseExpr:
    case ParseNodeKind::NullExpr:
    case ParseNodeKind::RawUndefinedExpr:
      return literal(pn, dst);

    case ParseNodeKind::YieldStarExpr: {
      UnaryNode* yieldNode = &pn->as<UnaryNode>();
      ParseNode* operand = yieldNode->kid();
      MOZ_ASSERT(yieldNode->pn_pos.encloses(operand->pn_pos));

      RootedValue arg(cx);
      return expression(operand, &arg) &&
             builder.yieldExpression(arg, Delegating, &yieldNode->pn_pos, dst);
    }

    case ParseNodeKind::YieldExpr: {
      UnaryNode* yieldNode = &pn->as<UnaryNode>();
      ParseNode* operand = yieldNode->kid();
      MOZ_ASSERT_IF(operand, yieldNode->pn_pos.encloses(operand->pn_pos));

      RootedValue arg(cx);
      return optExpression(operand, &arg) &&
             builder.yieldExpression(arg, NotDelegating, &yieldNode->pn_pos,
                                     dst);
    }

    case ParseNodeKind::ClassDecl:
      return classDefinition(&pn->as<ClassNode>(), true, dst);

    case ParseNodeKind::NewTargetExpr:
    case ParseNodeKind::ImportMetaExpr: {
      BinaryNode* node = &pn->as<BinaryNode>();
      ParseNode* firstNode = node->left();
      MOZ_ASSERT(firstNode->isKind(ParseNodeKind::PosHolder));
      MOZ_ASSERT(node->pn_pos.encloses(firstNode->pn_pos));

      ParseNode* secondNode = node->right();
      MOZ_ASSERT(secondNode->isKind(ParseNodeKind::PosHolder));
      MOZ_ASSERT(node->pn_pos.encloses(secondNode->pn_pos));

      RootedValue firstIdent(cx);
      RootedValue secondIdent(cx);

      RootedAtom firstStr(cx);
      RootedAtom secondStr(cx);

      if (node->getKind() == ParseNodeKind::NewTargetExpr) {
        firstStr = cx->names().new_;
        secondStr = cx->names().target;
      } else {
        firstStr = cx->names().import;
        secondStr = cx->names().meta;
      }

      return identifier(firstStr, &firstNode->pn_pos, &firstIdent) &&
             identifier(secondStr, &secondNode->pn_pos, &secondIdent) &&
             builder.metaProperty(firstIdent, secondIdent, &node->pn_pos, dst);
    }

    case ParseNodeKind::CallImportExpr: {
      BinaryNode* node = &pn->as<BinaryNode>();
      ParseNode* identNode = node->left();
      MOZ_ASSERT(identNode->isKind(ParseNodeKind::PosHolder));
      MOZ_ASSERT(identNode->pn_pos.encloses(identNode->pn_pos));

      ParseNode* argNode = node->right();
      MOZ_ASSERT(node->pn_pos.encloses(argNode->pn_pos));

      RootedValue ident(cx);
      RootedValue arg(cx);

      HandlePropertyName name = cx->names().import;
      return identifier(name, &identNode->pn_pos, &ident) &&
             expression(argNode, &arg) &&
             builder.callImportExpression(ident, arg, &pn->pn_pos, dst);
    }

    case ParseNodeKind::SetThis: {
      // SETTHIS is used to assign the result of a super() call to |this|.
      // It's not part of the original AST, so just forward to the call.
      BinaryNode* node = &pn->as<BinaryNode>();
      MOZ_ASSERT(node->left()->isKind(ParseNodeKind::Name));
      return expression(node->right(), dst);
    }

    default:
      LOCAL_NOT_REACHED("unexpected expression type");
  }
}

bool ASTSerializer::propertyName(ParseNode* key, MutableHandleValue dst) {
  if (key->isKind(ParseNodeKind::ComputedName)) {
    return expression(key, dst);
  }
  if (key->isKind(ParseNodeKind::ObjectPropertyName) ||
      key->isKind(ParseNodeKind::PrivateName)) {
    return identifier(&key->as<NameNode>(), dst);
  }

  LOCAL_ASSERT(key->isKind(ParseNodeKind::StringExpr) ||
               key->isKind(ParseNodeKind::NumberExpr) ||
               key->isKind(ParseNodeKind::BigIntExpr));

  return literal(key, dst);
}

bool ASTSerializer::property(ParseNode* pn, MutableHandleValue dst) {
  if (pn->isKind(ParseNodeKind::MutateProto)) {
    RootedValue val(cx);
    return expression(pn->as<UnaryNode>().kid(), &val) &&
           builder.prototypeMutation(val, &pn->pn_pos, dst);
  }
  if (pn->isKind(ParseNodeKind::Spread)) {
    return expression(pn, dst);
  }

  PropKind kind;
  if (pn->is<PropertyDefinition>()) {
    switch (pn->as<PropertyDefinition>().accessorType()) {
      case AccessorType::None:
        kind = PROP_INIT;
        break;

      case AccessorType::Getter:
        kind = PROP_GETTER;
        break;

      case AccessorType::Setter:
        kind = PROP_SETTER;
        break;

      default:
        LOCAL_NOT_REACHED("unexpected object-literal property");
    }
  } else {
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Shorthand));
    kind = PROP_INIT;
  }

  BinaryNode* node = &pn->as<BinaryNode>();
  ParseNode* keyNode = node->left();
  ParseNode* valNode = node->right();

  bool isShorthand = node->isKind(ParseNodeKind::Shorthand);
  bool isMethod =
      valNode->is<FunctionNode>() &&
      valNode->as<FunctionNode>().funbox()->kind() == FunctionFlags::Method;
  RootedValue key(cx), val(cx);
  return propertyName(keyNode, &key) && expression(valNode, &val) &&
         builder.propertyInitializer(key, val, kind, isShorthand, isMethod,
                                     &node->pn_pos, dst);
}

bool ASTSerializer::literal(ParseNode* pn, MutableHandleValue dst) {
  RootedValue val(cx);
  switch (pn->getKind()) {
    case ParseNodeKind::TemplateStringExpr:
    case ParseNodeKind::StringExpr: {
      JSAtom* exprAtom =
          parser->liftParserAtomToJSAtom(pn->as<NameNode>().atom());
      if (!exprAtom) {
        return false;
      }
      val.setString(exprAtom);
      break;
    }

    case ParseNodeKind::RegExpExpr: {
      RegExpObject* re = pn->as<RegExpLiteral>().create(
          cx, parser->parserAtoms(),
          parser->getCompilationState().input.atomCache,
          parser->getCompilationState());
      if (!re) {
        return false;
      }

      val.setObject(*re);
      break;
    }

    case ParseNodeKind::NumberExpr:
      val.setNumber(pn->as<NumericLiteral>().value());
      break;

    case ParseNodeKind::BigIntExpr: {
      auto index = pn->as<BigIntLiteral>().index();
      BigInt* x = parser->compilationState_.bigIntData[index].createBigInt(cx);
      if (!x) {
        return false;
      }
      cx->check(x);
      val.setBigInt(x);
      break;
    }

    case ParseNodeKind::NullExpr:
      val.setNull();
      break;

    case ParseNodeKind::RawUndefinedExpr:
      val.setUndefined();
      break;

    case ParseNodeKind::TrueExpr:
      val.setBoolean(true);
      break;

    case ParseNodeKind::FalseExpr:
      val.setBoolean(false);
      break;

    default:
      LOCAL_NOT_REACHED("unexpected literal type");
  }

  return builder.literal(val, &pn->pn_pos, dst);
}

bool ASTSerializer::arrayPattern(ListNode* array, MutableHandleValue dst) {
  MOZ_ASSERT(array->isKind(ParseNodeKind::ArrayExpr));

  NodeVector elts(cx);
  if (!elts.reserve(array->count())) {
    return false;
  }

  for (ParseNode* item : array->contents()) {
    if (item->isKind(ParseNodeKind::Elision)) {
      elts.infallibleAppend(NullValue());
    } else if (item->isKind(ParseNodeKind::Spread)) {
      RootedValue target(cx);
      RootedValue spread(cx);
      if (!pattern(item->as<UnaryNode>().kid(), &target)) {
        return false;
      }
      if (!builder.spreadExpression(target, &item->pn_pos, &spread))
        return false;
      elts.infallibleAppend(spread);
    } else {
      RootedValue patt(cx);
      if (!pattern(item, &patt)) {
        return false;
      }
      elts.infallibleAppend(patt);
    }
  }

  return builder.arrayPattern(elts, &array->pn_pos, dst);
}

bool ASTSerializer::objectPattern(ListNode* obj, MutableHandleValue dst) {
  MOZ_ASSERT(obj->isKind(ParseNodeKind::ObjectExpr));

  NodeVector elts(cx);
  if (!elts.reserve(obj->count())) {
    return false;
  }

  for (ParseNode* propdef : obj->contents()) {
    if (propdef->isKind(ParseNodeKind::Spread)) {
      RootedValue target(cx);
      RootedValue spread(cx);
      if (!pattern(propdef->as<UnaryNode>().kid(), &target)) {
        return false;
      }
      if (!builder.spreadExpression(target, &propdef->pn_pos, &spread))
        return false;
      elts.infallibleAppend(spread);
      continue;
    }
    // Patterns can't have getters/setters.
    LOCAL_ASSERT(!propdef->is<PropertyDefinition>() ||
                 propdef->as<PropertyDefinition>().accessorType() ==
                     AccessorType::None);

    RootedValue key(cx);
    ParseNode* target;
    if (propdef->isKind(ParseNodeKind::MutateProto)) {
      RootedValue pname(cx, StringValue(cx->names().proto));
      if (!builder.literal(pname, &propdef->pn_pos, &key)) {
        return false;
      }
      target = propdef->as<UnaryNode>().kid();
    } else {
      BinaryNode* prop = &propdef->as<BinaryNode>();
      if (!propertyName(prop->left(), &key)) {
        return false;
      }
      target = prop->right();
    }

    RootedValue patt(cx), prop(cx);
    if (!pattern(target, &patt) ||
        !builder.propertyPattern(key, patt,
                                 propdef->isKind(ParseNodeKind::Shorthand),
                                 &propdef->pn_pos, &prop)) {
      return false;
    }

    elts.infallibleAppend(prop);
  }

  return builder.objectPattern(elts, &obj->pn_pos, dst);
}

bool ASTSerializer::pattern(ParseNode* pn, MutableHandleValue dst) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  switch (pn->getKind()) {
    case ParseNodeKind::ObjectExpr:
      return objectPattern(&pn->as<ListNode>(), dst);

    case ParseNodeKind::ArrayExpr:
      return arrayPattern(&pn->as<ListNode>(), dst);

    default:
      return expression(pn, dst);
  }
}

bool ASTSerializer::identifier(HandleAtom atom, TokenPos* pos,
                               MutableHandleValue dst) {
  RootedValue atomContentsVal(cx, unrootedAtomContents(atom));
  return builder.identifier(atomContentsVal, pos, dst);
}

bool ASTSerializer::identifier(NameNode* id, MutableHandleValue dst) {
  LOCAL_ASSERT(id->atom());

  RootedAtom pnAtom(cx, parser->liftParserAtomToJSAtom(id->atom()));
  if (!pnAtom.get()) {
    return false;
  }
  return identifier(pnAtom, &id->pn_pos, dst);
}

bool ASTSerializer::identifierOrLiteral(ParseNode* id, MutableHandleValue dst) {
  if (id->getKind() == ParseNodeKind::Name) {
    return identifier(&id->as<NameNode>(), dst);
  }
  return literal(id, dst);
}

bool ASTSerializer::function(FunctionNode* funNode, ASTType type,
                             MutableHandleValue dst) {
  FunctionBox* funbox = funNode->funbox();

  GeneratorStyle generatorStyle =
      funbox->isGenerator() ? GeneratorStyle::ES6 : GeneratorStyle::None;

  bool isAsync = funbox->isAsync();
  bool isExpression = funbox->hasExprBody();

  RootedValue id(cx);
  RootedAtom funcAtom(cx);
  if (funbox->explicitName()) {
    funcAtom.set(parser->liftParserAtomToJSAtom(funbox->explicitName()));
    if (!funcAtom) {
      return false;
    }
  }
  if (!optIdentifier(funcAtom, nullptr, &id)) {
    return false;
  }

  NodeVector args(cx);
  NodeVector defaults(cx);

  RootedValue body(cx), rest(cx);
  if (funbox->hasRest()) {
    rest.setUndefined();
  } else {
    rest.setNull();
  }
  return functionArgsAndBody(funNode->body(), args, defaults, isAsync,
                             isExpression, &body, &rest) &&
         builder.function(type, &funNode->pn_pos, id, args, defaults, body,
                          rest, generatorStyle, isAsync, isExpression, dst);
}

bool ASTSerializer::functionArgsAndBody(ParseNode* pn, NodeVector& args,
                                        NodeVector& defaults, bool isAsync,
                                        bool isExpression,
                                        MutableHandleValue body,
                                        MutableHandleValue rest) {
  ListNode* argsList;
  ParseNode* bodyNode;

  /* Extract the args and body separately. */
  if (pn->isKind(ParseNodeKind::ParamsBody)) {
    argsList = &pn->as<ListNode>();
    bodyNode = argsList->last();
  } else {
    argsList = nullptr;
    bodyNode = pn;
  }

  if (bodyNode->is<LexicalScopeNode>()) {
    bodyNode = bodyNode->as<LexicalScopeNode>().scopeBody();
  }

  /* Serialize the arguments and body. */
  switch (bodyNode->getKind()) {
    case ParseNodeKind::ReturnStmt: /* expression closure, no destructured args
                                     */
      return functionArgs(pn, argsList, args, defaults, rest) &&
             expression(bodyNode->as<UnaryNode>().kid(), body);

    case ParseNodeKind::StatementList: /* statement closure */
    {
      ParseNode* firstNode = bodyNode->as<ListNode>().head();

      // Skip over initial yield in generator.
      if (firstNode && firstNode->isKind(ParseNodeKind::InitialYield)) {
        firstNode = firstNode->pn_next;
      }

      // Async arrow with expression body is converted into STATEMENTLIST
      // to insert initial yield.
      if (isAsync && isExpression) {
        MOZ_ASSERT(firstNode->getKind() == ParseNodeKind::ReturnStmt);
        return functionArgs(pn, argsList, args, defaults, rest) &&
               expression(firstNode->as<UnaryNode>().kid(), body);
      }

      return functionArgs(pn, argsList, args, defaults, rest) &&
             functionBody(firstNode, &bodyNode->pn_pos, body);
    }

    default:
      LOCAL_NOT_REACHED("unexpected function contents");
  }
}

bool ASTSerializer::functionArgs(ParseNode* pn, ListNode* argsList,
                                 NodeVector& args, NodeVector& defaults,
                                 MutableHandleValue rest) {
  if (!argsList) {
    return true;
  }

  RootedValue node(cx);
  bool defaultsNull = true;
  MOZ_ASSERT(defaults.empty(),
             "must be initially empty for it to be proper to clear this "
             "when there are no defaults");

  for (ParseNode* arg : argsList->contentsTo(argsList->last())) {
    ParseNode* pat;
    ParseNode* defNode;
    if (arg->isKind(ParseNodeKind::Name) ||
        arg->isKind(ParseNodeKind::ArrayExpr) ||
        arg->isKind(ParseNodeKind::ObjectExpr)) {
      pat = arg;
      defNode = nullptr;
    } else {
      MOZ_ASSERT(arg->isKind(ParseNodeKind::AssignExpr));
      AssignmentNode* assignNode = &arg->as<AssignmentNode>();
      pat = assignNode->left();
      defNode = assignNode->right();
    }

    // Process the name or pattern.
    MOZ_ASSERT(pat->isKind(ParseNodeKind::Name) ||
               pat->isKind(ParseNodeKind::ArrayExpr) ||
               pat->isKind(ParseNodeKind::ObjectExpr));
    if (!pattern(pat, &node)) {
      return false;
    }
    if (rest.isUndefined() && arg->pn_next == argsList->last()) {
      rest.setObject(node.toObject());
    } else {
      if (!args.append(node)) {
        return false;
      }
    }

    // Process its default (or lack thereof).
    if (defNode) {
      defaultsNull = false;
      RootedValue def(cx);
      if (!expression(defNode, &def) || !defaults.append(def)) {
        return false;
      }
    } else {
      if (!defaults.append(NullValue())) {
        return false;
      }
    }
  }
  MOZ_ASSERT(!rest.isUndefined(),
             "if a rest argument was present (signified by "
             "|rest.isUndefined()| initially), the rest node was properly "
             "recorded");

  if (defaultsNull) {
    defaults.clear();
  }

  return true;
}

bool ASTSerializer::functionBody(ParseNode* pn, TokenPos* pos,
                                 MutableHandleValue dst) {
  NodeVector elts(cx);

  // We aren't sure how many elements there are up front, so we'll check each
  // append.
  for (ParseNode* next = pn; next; next = next->pn_next) {
    RootedValue child(cx);
    if (!sourceElement(next, &child) || !elts.append(child)) {
      return false;
    }
  }

  return builder.blockStatement(elts, pos, dst);
}

static bool reflect_parse(JSContext* cx, uint32_t argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "Reflect.parse", 1)) {
    return false;
  }

  RootedString src(cx, ToString<CanGC>(cx, args[0]));
  if (!src) {
    return false;
  }

  UniqueChars filename;
  uint32_t lineno = 1;
  bool loc = true;
  RootedObject builder(cx);
  ParseGoal target = ParseGoal::Script;

  RootedValue arg(cx, args.get(1));

  if (!arg.isNullOrUndefined()) {
    if (!arg.isObject()) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, arg,
                       nullptr, "not an object");
      return false;
    }

    RootedObject config(cx, &arg.toObject());

    RootedValue prop(cx);

    /* config.loc */
    RootedId locId(cx, NameToId(cx->names().loc));
    RootedValue trueVal(cx, BooleanValue(true));
    if (!GetPropertyDefault(cx, config, locId, trueVal, &prop)) {
      return false;
    }

    loc = ToBoolean(prop);

    if (loc) {
      /* config.source */
      RootedId sourceId(cx, NameToId(cx->names().source));
      RootedValue nullVal(cx, NullValue());
      if (!GetPropertyDefault(cx, config, sourceId, nullVal, &prop)) {
        return false;
      }

      if (!prop.isNullOrUndefined()) {
        RootedString str(cx, ToString<CanGC>(cx, prop));
        if (!str) {
          return false;
        }

        filename = EncodeLatin1(cx, str);
        if (!filename) {
          return false;
        }
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
    if (!GetPropertyDefault(cx, config, builderId, nullVal, &prop)) {
      return false;
    }

    if (!prop.isNullOrUndefined()) {
      if (!prop.isObject()) {
        ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, prop,
                         nullptr, "not an object");
        return false;
      }
      builder = &prop.toObject();
    }

    /* config.target */
    RootedId targetId(cx, NameToId(cx->names().target));
    RootedValue scriptVal(cx, StringValue(cx->names().script));
    if (!GetPropertyDefault(cx, config, targetId, scriptVal, &prop)) {
      return false;
    }

    if (!prop.isString()) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, prop,
                       nullptr, "not 'script' or 'module'");
      return false;
    }

    RootedString stringProp(cx, prop.toString());
    bool isScript = false;
    bool isModule = false;
    if (!EqualStrings(cx, stringProp, cx->names().script, &isScript)) {
      return false;
    }

    if (!EqualStrings(cx, stringProp, cx->names().module, &isModule)) {
      return false;
    }

    if (isScript) {
      target = ParseGoal::Script;
    } else if (isModule) {
      target = ParseGoal::Module;
    } else {
      JS_ReportErrorASCII(cx,
                          "Bad target value, expected 'script' or 'module'");
      return false;
    }
  }

  /* Extract the builder methods first to report errors before parsing. */
  ASTSerializer serialize(cx, loc, filename.get(), lineno);
  if (!serialize.init(builder)) {
    return false;
  }

  JSLinearString* linear = src->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  AutoStableStringChars linearChars(cx);
  if (!linearChars.initTwoByte(cx, linear)) {
    return false;
  }

  CompileOptions options(cx);
  options.setFileAndLine(filename.get(), lineno);
  options.setForceFullParse();
  options.allowHTMLComments = target == ParseGoal::Script;
  mozilla::Range<const char16_t> chars = linearChars.twoByteRange();

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  if (target == ParseGoal::Script) {
    if (!input.get().initForGlobal(cx)) {
      return false;
    }
  } else {
    if (!input.get().initForModule(cx)) {
      return false;
    }
  }

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  frontend::CompilationState compilationState(cx, allocScope, input.get());
  if (!compilationState.init(cx)) {
    return false;
  }

  Parser<FullParseHandler, char16_t> parser(
      cx, options, chars.begin().get(), chars.length(),
      /* foldConstants = */ false, compilationState,
      /* syntaxParser = */ nullptr);
  if (!parser.checkOptions()) {
    return false;
  }

  serialize.setParser(&parser);

  ParseNode* pn;
  if (target == ParseGoal::Script) {
    pn = parser.parse();
    if (!pn) {
      return false;
    }
  } else {
    if (!GlobalObject::ensureModulePrototypesCreated(cx, cx->global())) {
      return false;
    }

    ModuleBuilder builder(cx, &parser);

    uint32_t len = chars.length();
    SourceExtent extent =
        SourceExtent::makeGlobalExtent(len, options.lineno, options.column);
    ModuleSharedContext modulesc(cx, options, builder, extent);
    pn = parser.moduleBody(&modulesc);
    if (!pn) {
      return false;
    }

    pn = pn->as<ModuleNode>().body();
  }

  RootedValue val(cx);
  if (!serialize.program(&pn->as<ListNode>(), &val)) {
    args.rval().setNull();
    return false;
  }

  args.rval().set(val);
  return true;
}

JS_PUBLIC_API bool JS_InitReflectParse(JSContext* cx, HandleObject global) {
  RootedValue reflectVal(cx);
  if (!GetProperty(cx, global, global, cx->names().Reflect, &reflectVal)) {
    return false;
  }
  if (!reflectVal.isObject()) {
    JS_ReportErrorASCII(
        cx, "JS_InitReflectParse must be called during global initialization");
    return false;
  }

  RootedObject reflectObj(cx, &reflectVal.toObject());
  return JS_DefineFunction(cx, reflectObj, "parse", reflect_parse, 1, 0);
}
