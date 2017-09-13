/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineFrame_inl_h
#define jit_BaselineFrame_inl_h

#include "jit/BaselineFrame.h"

#include "jscntxt.h"
#include "jscompartment.h"

#include "vm/ScopeObject.h"

#include "jsscriptinlines.h"

#include "vm/ScopeObject-inl.h"

namespace js {
namespace jit {

inline void
BaselineFrame::pushOnScopeChain(ScopeObject& scope)
{
    MOZ_ASSERT(*scopeChain() == scope.enclosingScope() ||
               *scopeChain() == scope.as<CallObject>().enclosingScope().as<DeclEnvObject>().enclosingScope());
    scopeChain_ = &scope;
}

inline void
BaselineFrame::popOffScopeChain()
{
    scopeChain_ = &scopeChain_->as<ScopeObject>().enclosingScope();
}

inline void
BaselineFrame::popWith(JSContext* cx)
{
    if (MOZ_UNLIKELY(isDebuggee()))
        DebugScopes::onPopWith(this);

    MOZ_ASSERT(scopeChain()->is<DynamicWithObject>());
    popOffScopeChain();
}

inline void
BaselineFrame::replaceInnermostScope(ScopeObject& scope)
{
    MOZ_ASSERT(scope.enclosingScope() == scopeChain_->as<ScopeObject>().enclosingScope());
    scopeChain_ = &scope;
}

inline bool
BaselineFrame::pushBlock(JSContext* cx, Handle<StaticBlockObject*> block)
{
    MOZ_ASSERT(block->needsClone());

    ClonedBlockObject* clone = ClonedBlockObject::create(cx, block, this);
    if (!clone)
        return false;
    pushOnScopeChain(*clone);

    return true;
}

inline void
BaselineFrame::popBlock(JSContext* cx)
{
    MOZ_ASSERT(scopeChain_->is<ClonedBlockObject>());

    popOffScopeChain();
}

inline bool
BaselineFrame::freshenBlock(JSContext* cx)
{
    Rooted<ClonedBlockObject*> current(cx, &scopeChain_->as<ClonedBlockObject>());
    ClonedBlockObject* clone = ClonedBlockObject::clone(cx, current);
    if (!clone)
        return false;

    replaceInnermostScope(*clone);
    return true;
}

inline CallObject&
BaselineFrame::callObj() const
{
    MOZ_ASSERT(hasCallObj());
    MOZ_ASSERT(fun()->needsCallObject());

    JSObject* obj = scopeChain();
    while (!obj->is<CallObject>())
        obj = obj->enclosingScope();
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
