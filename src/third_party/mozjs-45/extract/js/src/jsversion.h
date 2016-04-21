/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsversion_h
#define jsversion_h

/*
 * JS Capability Macros.
 */
#define JS_HAS_STR_HTML_HELPERS 1       /* (no longer used) */
#define JS_HAS_OBJ_PROTO_PROP   1       /* has o.__proto__ etc. */
#define JS_HAS_OBJ_WATCHPOINT   1       /* has o.watch and o.unwatch */
#define JS_HAS_TOSOURCE         1       /* has Object/Array toSource method */
#define JS_HAS_CATCH_GUARD      1       /* has exception handling catch guard */
#define JS_HAS_UNEVAL           1       /* has uneval() top-level function */
#define JS_HAS_CONST            1       /* (no longer used) */
#define JS_HAS_FUN_EXPR_STMT    1       /* (no longer used) */
#define JS_HAS_FOR_EACH_IN      1       /* has for each (lhs in iterable) */
#define JS_HAS_GENERATORS       1       /* (no longer used) */
#define JS_HAS_BLOCK_SCOPE      1       /* (no longer used) */
#define JS_HAS_DESTRUCTURING    2       /* (no longer used) */
#define JS_HAS_GENERATOR_EXPRS  1       /* has (expr for (lhs in iterable)) */
#define JS_HAS_EXPR_CLOSURES    1       /* has function (formals) listexpr */

/* (no longer used) */
#define JS_HAS_NEW_GLOBAL_OBJECT        1

/* (no longer used) */
#define JS_HAS_DESTRUCTURING_SHORTHAND  (JS_HAS_DESTRUCTURING == 2)

/*
 * Feature for Object.prototype.__{define,lookup}{G,S}etter__ legacy support;
 * support likely to be made opt-in at some future time.
 */
#define JS_OLD_GETTER_SETTER_METHODS    1

#ifdef NIGHTLY_BUILD

/* Support for ES7 Exponentiation proposal. */
#define JS_HAS_EXPONENTIATION 1

#endif // NIGHTLY_BUILD

#endif /* jsversion_h */
