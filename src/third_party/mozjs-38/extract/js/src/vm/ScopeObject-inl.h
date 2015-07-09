/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ScopeObject_inl_h
#define vm_ScopeObject_inl_h

#include "vm/ScopeObject.h"

#include "vm/TypeInference-inl.h"

namespace js {

inline void
ScopeObject::setAliasedVar(JSContext* cx, ScopeCoordinate sc, PropertyName* name, const Value& v)
{
    MOZ_ASSERT(is<CallObject>() || is<ClonedBlockObject>());
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
CallObject::setAliasedVar(JSContext* cx, AliasedFormalIter fi, PropertyName* name, const Value& v)
{
    MOZ_ASSERT(name == fi->name());
    setSlot(fi.scopeSlot(), v);
    if (isSingleton())
        AddTypePropertyId(cx, this, NameToId(name), v);
}

inline void
CallObject::setAliasedVarFromArguments(JSContext* cx, const Value& argsValue, jsid id, const Value& v)
{
    setSlot(ArgumentsObject::SlotFromMagicScopeSlotValue(argsValue), v);
    if (isSingleton())
        AddTypePropertyId(cx, this, id, v);
}

inline void
CallObject::initRemainingSlotsToUninitializedLexicals(uint32_t begin)
{
    uint32_t end = slotSpan();
    for (uint32_t slot = begin; slot < end; slot++)
        initSlot(slot, MagicValue(JS_UNINITIALIZED_LEXICAL));
}

inline void
CallObject::initAliasedLexicalsToThrowOnTouch(JSScript* script)
{
    initRemainingSlotsToUninitializedLexicals(script->bindings.aliasedBodyLevelLexicalBegin());
}

template <AllowGC allowGC>
inline bool
StaticScopeIter<allowGC>::done() const
{
    return !obj;
}

template <AllowGC allowGC>
inline void
StaticScopeIter<allowGC>::operator++(int)
{
    if (obj->template is<NestedScopeObject>()) {
        obj = obj->template as<NestedScopeObject>().enclosingScopeForStaticScopeIter();
    } else if (obj->template is<StaticEvalObject>()) {
        obj = obj->template as<StaticEvalObject>().enclosingScopeForStaticScopeIter();
    } else if (onNamedLambda || !obj->template as<JSFunction>().isNamedLambda()) {
        onNamedLambda = false;
        obj = obj->template as<JSFunction>().nonLazyScript()->enclosingStaticScope();
    } else {
        onNamedLambda = true;
    }
    MOZ_ASSERT_IF(obj, obj->template is<NestedScopeObject>() ||
                       obj->template is<StaticEvalObject>() ||
                       obj->template is<JSFunction>());
    MOZ_ASSERT_IF(onNamedLambda, obj->template is<JSFunction>());
}

template <AllowGC allowGC>
inline bool
StaticScopeIter<allowGC>::hasDynamicScopeObject() const
{
    return obj->template is<StaticBlockObject>()
           ? obj->template as<StaticBlockObject>().needsClone()
           : (obj->template is<StaticEvalObject>()
              ? obj->template as<StaticEvalObject>().isStrict()
              : (obj->template is<StaticWithObject>() ||
                 obj->template as<JSFunction>().isHeavyweight()));
}

template <AllowGC allowGC>
inline Shape*
StaticScopeIter<allowGC>::scopeShape() const
{
    MOZ_ASSERT(hasDynamicScopeObject());
    MOZ_ASSERT(type() != NamedLambda && type() != Eval);
    if (type() == Block)
        return block().lastProperty();
    return funScript()->callObjShape();
}

template <AllowGC allowGC>
inline typename StaticScopeIter<allowGC>::Type
StaticScopeIter<allowGC>::type() const
{
    if (onNamedLambda)
        return NamedLambda;
    return obj->template is<StaticBlockObject>()
           ? Block
           : (obj->template is<StaticWithObject>()
              ? With
              : (obj->template is<StaticEvalObject>()
                 ? Eval
                 : Function));
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

}  /* namespace js */

#endif /* vm_ScopeObject_inl_h */
