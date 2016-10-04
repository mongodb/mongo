/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsfuninlines_h
#define jsfuninlines_h

#include "jsfun.h"

#include "vm/ScopeObject.h"

namespace js {

inline const char*
GetFunctionNameBytes(JSContext* cx, JSFunction* fun, JSAutoByteString* bytes)
{
    JSAtom* atom = fun->atom();
    if (atom)
        return bytes->encodeLatin1(cx, atom);
    return js_anonymous_str;
}

static inline JSObject*
SkipScopeParent(JSObject* parent)
{
    if (!parent)
        return nullptr;
    while (parent->is<ScopeObject>())
        parent = &parent->as<ScopeObject>().enclosingScope();
    return parent;
}

inline bool
CanReuseFunctionForClone(JSContext* cx, HandleFunction fun)
{
    if (!fun->isSingleton())
        return false;
    if (fun->isInterpretedLazy()) {
        LazyScript* lazy = fun->lazyScript();
        if (lazy->hasBeenCloned())
            return false;
        lazy->setHasBeenCloned();
    } else {
        JSScript* script = fun->nonLazyScript();
        if (script->hasBeenCloned())
            return false;
        script->setHasBeenCloned();
    }
    return true;
}

inline JSFunction*
CloneFunctionObjectIfNotSingleton(JSContext* cx, HandleFunction fun, HandleObject parent,
                                  HandleObject proto = nullptr,
                                  NewObjectKind newKind = GenericObject)
{
    /*
     * For attempts to clone functions at a function definition opcode,
     * try to avoid the the clone if the function has singleton type. This
     * was called pessimistically, and we need to preserve the type's
     * property that if it is singleton there is only a single object
     * with its type in existence.
     *
     * For functions inner to run once lambda, it may be possible that
     * the lambda runs multiple times and we repeatedly clone it. In these
     * cases, fall through to CloneFunctionObject, which will deep clone
     * the function's script.
     */
    if (CanReuseFunctionForClone(cx, fun)) {
        RootedObject obj(cx, SkipScopeParent(parent));
        ObjectOpResult succeeded;
        if (proto && !SetPrototype(cx, fun, proto, succeeded))
            return nullptr;
        MOZ_ASSERT(!proto || succeeded);
        fun->setEnvironment(parent);
        return fun;
    }

    // These intermediate variables are needed to avoid link errors on some
    // platforms.  Sigh.
    gc::AllocKind finalizeKind = gc::AllocKind::FUNCTION;
    gc::AllocKind extendedFinalizeKind = gc::AllocKind::FUNCTION_EXTENDED;
    gc::AllocKind kind = fun->isExtended()
                         ? extendedFinalizeKind
                         : finalizeKind;

    if (CanReuseScriptForClone(cx->compartment(), fun, parent))
        return CloneFunctionReuseScript(cx, fun, parent, kind, newKind, proto);

    RootedScript script(cx, fun->getOrCreateScript(cx));
    if (!script)
        return nullptr;
    RootedObject staticScope(cx, script->enclosingStaticScope());
    return CloneFunctionAndScript(cx, fun, parent, staticScope, kind, proto);
}

} /* namespace js */

#endif /* jsfuninlines_h */
