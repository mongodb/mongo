/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ScopeObject_inl_h
#define vm_ScopeObject_inl_h

#include "vm/ScopeObject.h"
#include "frontend/SharedContext.h"

#include "jsobjinlines.h"

#include "vm/TypeInference-inl.h"

namespace js {

inline ClonedBlockObject&
NearestEnclosingExtensibleLexicalScope(JSObject* scope)
{
    while (!IsExtensibleLexicalScope(scope))
        scope = scope->enclosingScope();
    return scope->as<ClonedBlockObject>();
}

inline void
ScopeObject::setAliasedVar(JSContext* cx, ScopeCoordinate sc, PropertyName* name, const Value& v)
{
    MOZ_ASSERT(is<LexicalScopeBase>() || is<ClonedBlockObject>());
    JS_STATIC_ASSERT(CallObject::RESERVED_SLOTS == BlockObject::RESERVED_SLOTS);

    // name may be null if we don't need to track side effects on the object.
    MOZ_ASSERT_IF(isSingleton(), name);

    if (isSingleton()) {
        MOZ_ASSERT(name);
        AddTypePropertyId(cx, this, NameToId(name), v);

        // Keep track of properties which have ever been overwritten.
        if (!getSlot(sc.slot()).isUndefined()) {
            Shape* shape = lookup(cx, name);
            shape->setOverwritten();
        }
    }

    setSlot(sc.slot(), v);
}

inline void
LexicalScopeBase::setAliasedVar(JSContext* cx, AliasedFormalIter fi, PropertyName* name,
                                const Value& v)
{
    MOZ_ASSERT(name == fi->name());
    setSlot(fi.scopeSlot(), v);
    if (isSingleton())
        AddTypePropertyId(cx, this, NameToId(name), v);
}

inline void
LexicalScopeBase::setAliasedVarFromArguments(JSContext* cx, const Value& argsValue, jsid id,
                                             const Value& v)
{
    setSlot(ArgumentsObject::SlotFromMagicScopeSlotValue(argsValue), v);
    if (isSingleton())
        AddTypePropertyId(cx, this, id, v);
}

inline void
LexicalScopeBase::initRemainingSlotsToUninitializedLexicals(uint32_t begin)
{
    uint32_t end = slotSpan();
    for (uint32_t slot = begin; slot < end; slot++)
        initSlot(slot, MagicValue(JS_UNINITIALIZED_LEXICAL));
}

inline void
LexicalScopeBase::initAliasedLexicalsToThrowOnTouch(JSScript* script)
{
    initRemainingSlotsToUninitializedLexicals(script->bindings.aliasedBodyLevelLexicalBegin());
}

template <AllowGC allowGC>
inline void
StaticScopeIter<allowGC>::operator++(int)
{
    if (obj->template is<NestedScopeObject>()) {
        obj = obj->template as<NestedScopeObject>().enclosingScopeForStaticScopeIter();
    } else if (obj->template is<StaticEvalObject>()) {
        obj = obj->template as<StaticEvalObject>().enclosingScopeForStaticScopeIter();
    } else if (obj->template is<StaticNonSyntacticScopeObjects>()) {
        obj = obj->template as<StaticNonSyntacticScopeObjects>().enclosingScopeForStaticScopeIter();
    } else if (obj->template is<ModuleObject>()) {
        obj = obj->template as<ModuleObject>().enclosingStaticScope();
    } else if (onNamedLambda || !obj->template as<JSFunction>().isNamedLambda()) {
        onNamedLambda = false;
        JSFunction& fun = obj->template as<JSFunction>();
        if (fun.isBeingParsed())
            obj = fun.functionBox()->enclosingStaticScope();
        else
            obj = fun.nonLazyScript()->enclosingStaticScope();
    } else {
        onNamedLambda = true;
    }
    MOZ_ASSERT_IF(obj, IsStaticScope(obj));
    MOZ_ASSERT_IF(onNamedLambda, obj->template is<JSFunction>());
}

template <AllowGC allowGC>
inline bool
StaticScopeIter<allowGC>::hasSyntacticDynamicScopeObject() const
{
    if (obj->template is<JSFunction>()) {
        JSFunction& fun = obj->template as<JSFunction>();
        if (fun.isBeingParsed())
            return fun.functionBox()->needsCallObject();
        return fun.needsCallObject();
    }
    if (obj->template is<ModuleObject>())
        return true;
    if (obj->template is<StaticBlockObject>()) {
        return obj->template as<StaticBlockObject>().needsClone() ||
               obj->template as<StaticBlockObject>().isGlobal();
    }
    if (obj->template is<StaticWithObject>())
        return true;
    if (obj->template is<StaticEvalObject>())
        return obj->template as<StaticEvalObject>().isStrict();
    MOZ_ASSERT(obj->template is<StaticNonSyntacticScopeObjects>());
    return false;
}

template <AllowGC allowGC>
inline Shape*
StaticScopeIter<allowGC>::scopeShape() const
{
    MOZ_ASSERT(hasSyntacticDynamicScopeObject());
    MOZ_ASSERT(type() != NamedLambda && type() != Eval);
    if (type() == Block)
        return block().lastProperty();
    if (type() == Module)
        return moduleScript()->callObjShape();
    return funScript()->callObjShape();
}

template <AllowGC allowGC>
inline typename StaticScopeIter<allowGC>::Type
StaticScopeIter<allowGC>::type() const
{
    if (onNamedLambda)
        return NamedLambda;
    if (obj->template is<StaticBlockObject>())
        return Block;
    if (obj->template is<StaticWithObject>())
        return With;
    if (obj->template is<StaticEvalObject>())
        return Eval;
    if (obj->template is<StaticNonSyntacticScopeObjects>())
        return NonSyntactic;
    if (obj->template is<ModuleObject>())
        return Module;
    MOZ_ASSERT(obj->template is<JSFunction>());
    return Function;
}

template <AllowGC allowGC>
inline StaticBlockObject&
StaticScopeIter<allowGC>::block() const
{
    MOZ_ASSERT(type() == Block);
    return obj->template as<StaticBlockObject>();
}

template <AllowGC allowGC>
inline StaticWithObject&
StaticScopeIter<allowGC>::staticWith() const
{
    MOZ_ASSERT(type() == With);
    return obj->template as<StaticWithObject>();
}

template <AllowGC allowGC>
inline StaticEvalObject&
StaticScopeIter<allowGC>::eval() const
{
    MOZ_ASSERT(type() == Eval);
    return obj->template as<StaticEvalObject>();
}

template <AllowGC allowGC>
inline StaticNonSyntacticScopeObjects&
StaticScopeIter<allowGC>::nonSyntactic() const
{
    MOZ_ASSERT(type() == NonSyntactic);
    return obj->template as<StaticNonSyntacticScopeObjects>();
}

template <AllowGC allowGC>
inline JSScript*
StaticScopeIter<allowGC>::funScript() const
{
    MOZ_ASSERT(type() == Function);
    return obj->template as<JSFunction>().nonLazyScript();
}

template <AllowGC allowGC>
inline JSFunction&
StaticScopeIter<allowGC>::fun() const
{
    MOZ_ASSERT(type() == Function);
    return obj->template as<JSFunction>();
}

template <AllowGC allowGC>
inline frontend::FunctionBox*
StaticScopeIter<allowGC>::maybeFunctionBox() const
{
    MOZ_ASSERT(type() == Function);
    if (fun().isBeingParsed())
        return fun().functionBox();
    return nullptr;
}

template <AllowGC allowGC>
inline JSScript*
StaticScopeIter<allowGC>::moduleScript() const
{
    MOZ_ASSERT(type() == Module);
    return obj->template as<ModuleObject>().script();
}

template <AllowGC allowGC>
inline ModuleObject&
StaticScopeIter<allowGC>::module() const
{
    MOZ_ASSERT(type() == Module);
    return obj->template as<ModuleObject>();
}

}  /* namespace js */

inline JSObject*
JSObject::enclosingScope()
{
    if (is<js::ScopeObject>())
        return &as<js::ScopeObject>().enclosingScope();

    if (is<js::DebugScopeObject>())
        return &as<js::DebugScopeObject>().enclosingScope();

    if (is<js::GlobalObject>())
        return nullptr;

    MOZ_ASSERT_IF(is<JSFunction>(), as<JSFunction>().isInterpreted());
    return &global();
}

#endif /* vm_ScopeObject_inl_h */
