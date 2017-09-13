/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A higher-order macro for enumerating keyword tokens. */

#ifndef vm_Keywords_h
#define vm_Keywords_h

#define FOR_EACH_JAVASCRIPT_KEYWORD(macro) \
    macro(false, false_, TOK_FALSE) \
    macro(true, true_, TOK_TRUE) \
    macro(null, null, TOK_NULL) \
    /* Keywords. */ \
    macro(break, break_, TOK_BREAK) \
    macro(case, case_, TOK_CASE) \
    macro(catch, catch_, TOK_CATCH) \
    macro(const, const_, TOK_CONST) \
    macro(continue, continue_, TOK_CONTINUE) \
    macro(debugger, debugger, TOK_DEBUGGER) \
    macro(default, default_, TOK_DEFAULT) \
    macro(delete, delete_, TOK_DELETE) \
    macro(do, do_, TOK_DO) \
    macro(else, else_, TOK_ELSE) \
    macro(finally, finally_, TOK_FINALLY) \
    macro(for, for_, TOK_FOR) \
    macro(function, function, TOK_FUNCTION) \
    macro(if, if_, TOK_IF) \
    macro(in, in, TOK_IN) \
    macro(instanceof, instanceof, TOK_INSTANCEOF) \
    macro(new, new_, TOK_NEW) \
    macro(return, return_, TOK_RETURN) \
    macro(switch, switch_, TOK_SWITCH) \
    macro(this, this_, TOK_THIS) \
    macro(throw, throw_, TOK_THROW) \
    macro(try, try_, TOK_TRY) \
    macro(typeof, typeof, TOK_TYPEOF) \
    macro(var, var, TOK_VAR) \
    macro(void, void_, TOK_VOID) \
    macro(while, while_, TOK_WHILE) \
    macro(with, with, TOK_WITH) \
    macro(import, import, TOK_IMPORT) \
    macro(export, export, TOK_EXPORT) \
    macro(class, class_, TOK_CLASS) \
    macro(extends, extends, TOK_EXTENDS) \
    macro(super, super, TOK_SUPER) \
    /* Reserved keywords. */ \
    macro(enum, enum_, TOK_RESERVED) \
    /* Future reserved keywords, but only in strict mode. */ \
    macro(implements, implements, TOK_STRICT_RESERVED) \
    macro(interface, interface, TOK_STRICT_RESERVED) \
    macro(package, package, TOK_STRICT_RESERVED) \
    macro(private, private_, TOK_STRICT_RESERVED) \
    macro(protected, protected_, TOK_STRICT_RESERVED) \
    macro(public, public_, TOK_STRICT_RESERVED) \
    macro(static, static_, TOK_STRICT_RESERVED) \
    /* \
     * Yield is a token inside function*.  Outside of a function*, it is a \
     * future reserved keyword in strict mode, but a keyword in JS1.7 even \
     * when strict.  Punt logic to parser. \
     */ \
    macro(yield, yield, TOK_YIELD) \
    macro(let, let, TOK_LET)

#endif /* vm_Keywords_h */
