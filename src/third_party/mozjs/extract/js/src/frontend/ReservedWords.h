/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A higher-order macro for enumerating reserved word tokens. */

#ifndef vm_ReservedWords_h
#define vm_ReservedWords_h

#define FOR_EACH_JAVASCRIPT_RESERVED_WORD(macro) \
    macro(false, false_, TokenKind::False) \
    macro(true, true_, TokenKind::True) \
    macro(null, null, TokenKind::Null) \
    \
    /* Keywords. */ \
    macro(break, break_, TokenKind::Break) \
    macro(case, case_, TokenKind::Case) \
    macro(catch, catch_, TokenKind::Catch) \
    macro(const, const_, TokenKind::Const) \
    macro(continue, continue_, TokenKind::Continue) \
    macro(debugger, debugger, TokenKind::Debugger) \
    macro(default, default_, TokenKind::Default) \
    macro(delete, delete_, TokenKind::Delete) \
    macro(do, do_, TokenKind::Do) \
    macro(else, else_, TokenKind::Else) \
    macro(finally, finally_, TokenKind::Finally) \
    macro(for, for_, TokenKind::For) \
    macro(function, function, TokenKind::Function) \
    macro(if, if_, TokenKind::If) \
    macro(in, in, TokenKind::In) \
    macro(instanceof, instanceof, TokenKind::InstanceOf) \
    macro(new, new_, TokenKind::New) \
    macro(return, return_, TokenKind::Return) \
    macro(switch, switch_, TokenKind::Switch) \
    macro(this, this_, TokenKind::This) \
    macro(throw, throw_, TokenKind::Throw) \
    macro(try, try_, TokenKind::Try) \
    macro(typeof, typeof_, TokenKind::TypeOf) \
    macro(var, var, TokenKind::Var) \
    macro(void, void_, TokenKind::Void) \
    macro(while, while_, TokenKind::While) \
    macro(with, with, TokenKind::With) \
    macro(import, import, TokenKind::Import) \
    macro(export, export_, TokenKind::Export) \
    macro(class, class_, TokenKind::Class) \
    macro(extends, extends, TokenKind::Extends) \
    macro(super, super, TokenKind::Super) \
    \
    /* Future reserved words. */ \
    macro(enum, enum_, TokenKind::Enum) \
    \
    /* Future reserved words, but only in strict mode. */ \
    macro(implements, implements, TokenKind::Implements) \
    macro(interface, interface, TokenKind::Interface) \
    macro(package, package, TokenKind::Package) \
    macro(private, private_, TokenKind::Private) \
    macro(protected, protected_, TokenKind::Protected) \
    macro(public, public_, TokenKind::Public) \
    \
    /* Contextual keywords. */ \
    macro(as, as, TokenKind::As) \
    macro(async, async, TokenKind::Async) \
    macro(await, await, TokenKind::Await) \
    macro(from, from, TokenKind::From) \
    macro(get, get, TokenKind::Get) \
    macro(let, let, TokenKind::Let) \
    macro(of, of, TokenKind::Of) \
    macro(set, set, TokenKind::Set) \
    macro(static, static_, TokenKind::Static) \
    macro(target, target, TokenKind::Target) \
    /* \
     * Yield is a token inside function*.  Outside of a function*, it is a \
     * future reserved word in strict mode, but a keyword in JS1.7 even \
     * when strict.  Punt logic to parser. \
     */ \
    macro(yield, yield, TokenKind::Yield)

#endif /* vm_ReservedWords_h */
