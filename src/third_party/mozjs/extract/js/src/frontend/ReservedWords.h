/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A higher-order macro for enumerating reserved word tokens. */

#ifndef vm_ReservedWords_h
#define vm_ReservedWords_h

#define FOR_EACH_JAVASCRIPT_RESERVED_WORD(MACRO)                \
  MACRO(false, false_, TokenKind::False)                        \
  MACRO(true, true_, TokenKind::True)                           \
  MACRO(null, null, TokenKind::Null)                            \
                                                                \
  /* Keywords. */                                               \
  MACRO(break, break_, TokenKind::Break)                        \
  MACRO(case, case_, TokenKind::Case)                           \
  MACRO(catch, catch_, TokenKind::Catch)                        \
  MACRO(const, const_, TokenKind::Const)                        \
  MACRO(continue, continue_, TokenKind::Continue)               \
  MACRO(debugger, debugger, TokenKind::Debugger)                \
  MACRO(default, default_, TokenKind::Default)                  \
  MACRO(delete, delete_, TokenKind::Delete)                     \
  MACRO(do, do_, TokenKind::Do)                                 \
  MACRO(else, else_, TokenKind::Else)                           \
  MACRO(finally, finally_, TokenKind::Finally)                  \
  MACRO(for, for_, TokenKind::For)                              \
  MACRO(function, function, TokenKind::Function)                \
  MACRO(if, if_, TokenKind::If)                                 \
  MACRO(in, in, TokenKind::In)                                  \
  MACRO(instanceof, instanceof, TokenKind::InstanceOf)          \
  MACRO(new, new_, TokenKind::New)                              \
  MACRO(return, return_, TokenKind::Return)                     \
  MACRO(switch, switch_, TokenKind::Switch)                     \
  MACRO(this, this_, TokenKind::This)                           \
  MACRO(throw, throw_, TokenKind::Throw)                        \
  MACRO(try, try_, TokenKind::Try)                              \
  MACRO(typeof, typeof_, TokenKind::TypeOf)                     \
  MACRO(var, var, TokenKind::Var)                               \
  MACRO(void, void_, TokenKind::Void)                           \
  MACRO(while, while_, TokenKind::While)                        \
  MACRO(with, with, TokenKind::With)                            \
  MACRO(import, import, TokenKind::Import)                      \
  MACRO(export, export_, TokenKind::Export)                     \
  MACRO(class, class_, TokenKind::Class)                        \
  MACRO(extends, extends, TokenKind::Extends)                   \
  IF_DECORATORS(MACRO(accessor, accessor, TokenKind::Accessor)) \
  MACRO(super, super, TokenKind::Super)                         \
                                                                \
  /* Future reserved words. */                                  \
  MACRO(enum, enum_, TokenKind::Enum)                           \
                                                                \
  /* Future reserved words, but only in strict mode. */         \
  MACRO(implements, implements, TokenKind::Implements)          \
  MACRO(interface, interface, TokenKind::Interface)             \
  MACRO(package, package, TokenKind::Package)                   \
  MACRO(private, private_, TokenKind::Private)                  \
  MACRO(protected, protected_, TokenKind::Protected)            \
  MACRO(public, public_, TokenKind::Public)                     \
                                                                \
  /* Contextual keywords. */                                    \
  MACRO(as, as, TokenKind::As)                                  \
  MACRO(assert, assert_, TokenKind::Assert)                     \
  MACRO(async, async, TokenKind::Async)                         \
  MACRO(await, await, TokenKind::Await)                         \
  MACRO(from, from, TokenKind::From)                            \
  MACRO(get, get, TokenKind::Get)                               \
  MACRO(let, let, TokenKind::Let)                               \
  MACRO(meta, meta, TokenKind::Meta)                            \
  MACRO(of, of, TokenKind::Of)                                  \
  MACRO(set, set, TokenKind::Set)                               \
  MACRO(static, static_, TokenKind::Static)                     \
  MACRO(target, target, TokenKind::Target)                      \
  MACRO(yield, yield, TokenKind::Yield)

#endif /* vm_ReservedWords_h */
