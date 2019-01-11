/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineFrame_inl_h
#define jit_BaselineFrame_inl_h

#include "jit/BaselineFrame.h"

#include "vm/EnvironmentObject.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"

#include "vm/EnvironmentObject-inl.h"
#include "vm/JSScript-inl.h"

namespace js {
namespace jit {

template <typename SpecificEnvironment>
inline void
BaselineFrame::pushOnEnvironmentChain(SpecificEnvironment& env)
{
    MOZ_ASSERT(*environmentChain() == env.enclosingEnvironment());
    envChain_ = &env;
    if (IsFrameInitialEnvironment(this, env))
        flags_ |= HAS_INITIAL_ENV;
}

template <typename SpecificEnvironment>
inline void
BaselineFrame::popOffEnvironmentChain()
{
    MOZ_ASSERT(envChain_->is<SpecificEnvironment>());
    envChain_ = &envChain_->as<SpecificEnvironment>().enclosingEnvironment();
}

inline void
BaselineFrame::replaceInnermostEnvironment(EnvironmentObject& env)
{
    MOZ_ASSERT(env.enclosingEnvironment() ==
               envChain_->as<EnvironmentObject>().enclosingEnvironment());
    envChain_ = &env;
}

inline bool
BaselineFrame::pushLexicalEnvironment(JSContext* cx, Handle<LexicalScope*> scope)
{
    LexicalEnvironmentObject* env = LexicalEnvironmentObject::create(cx, scope, this);
    if (!env)
        return false;
    pushOnEnvironmentChain(*env);

    return true;
}

inline bool
BaselineFrame::freshenLexicalEnvironment(JSContext* cx)
{
    Rooted<LexicalEnvironmentObject*> current(cx, &envChain_->as<LexicalEnvironmentObject>());
    LexicalEnvironmentObject* clone = LexicalEnvironmentObject::clone(cx, current);
    if (!clone)
        return false;

    replaceInnermostEnvironment(*clone);
    return true;
}

inline bool
BaselineFrame::recreateLexicalEnvironment(JSContext* cx)
{
    Rooted<LexicalEnvironmentObject*> current(cx, &envChain_->as<LexicalEnvironmentObject>());
    LexicalEnvironmentObject* clone = LexicalEnvironmentObject::recreate(cx, current);
    if (!clone)
        return false;

    replaceInnermostEnvironment(*clone);
    return true;
}

inline CallObject&
BaselineFrame::callObj() const
{
    MOZ_ASSERT(hasInitialEnvironment());
    MOZ_ASSERT(callee()->needsCallObject());

    JSObject* obj = environmentChain();
    while (!obj->is<CallObject>())
        obj = obj->enclosingEnvironment();
    return obj->as<CallObject>();
}

inline void
BaselineFrame::unsetIsDebuggee()
{
    MOZ_ASSERT(!script()->isDebuggee());
    flags_ &= ~DEBUGGEE;
}

} // namespace jit
} // namespace js

#endif /* jit_BaselineFrame_inl_h */
