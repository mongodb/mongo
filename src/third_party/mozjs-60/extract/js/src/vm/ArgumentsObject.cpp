/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ArgumentsObject-inl.h"

#include "mozilla/PodOperations.h"

#include "gc/FreeOp.h"
#include "jit/JitFrames.h"
#include "vm/AsyncFunction.h"
#include "vm/GlobalObject.h"
#include "vm/Stack.h"

#include "gc/Nursery-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::gc;

/* static */ size_t
RareArgumentsData::bytesRequired(size_t numActuals)
{
    size_t extraBytes = NumWordsForBitArrayOfLength(numActuals) * sizeof(size_t);
    return offsetof(RareArgumentsData, deletedBits_) + extraBytes;
}

/* static */ RareArgumentsData*
RareArgumentsData::create(JSContext* cx, ArgumentsObject* obj)
{
    size_t bytes = RareArgumentsData::bytesRequired(obj->initialLength());

    uint8_t* data = AllocateObjectBuffer<uint8_t>(cx, obj, bytes);
    if (!data)
        return nullptr;

    mozilla::PodZero(data, bytes);

    return new(data) RareArgumentsData();
}

bool
ArgumentsObject::createRareData(JSContext* cx)
{
    MOZ_ASSERT(!data()->rareData);

    RareArgumentsData* rareData = RareArgumentsData::create(cx, this);
    if (!rareData)
        return false;

    data()->rareData = rareData;
    return true;
}

bool
ArgumentsObject::markElementDeleted(JSContext* cx, uint32_t i)
{
    RareArgumentsData* data = getOrCreateRareData(cx);
    if (!data)
        return false;

    data->markElementDeleted(initialLength(), i);
    return true;
}

static void
CopyStackFrameArguments(const AbstractFramePtr frame, GCPtrValue* dst, unsigned totalArgs)
{
    MOZ_ASSERT_IF(frame.isInterpreterFrame(), !frame.asInterpreterFrame()->runningInJit());

    MOZ_ASSERT(Max(frame.numActualArgs(), frame.numFormalArgs()) == totalArgs);

    /* Copy arguments. */
    Value* src = frame.argv();
    Value* end = src + totalArgs;
    while (src != end)
        (dst++)->init(*src++);
}

/* static */ void
ArgumentsObject::MaybeForwardToCallObject(AbstractFramePtr frame, ArgumentsObject* obj,
                                          ArgumentsData* data)
{
    JSScript* script = frame.script();
    if (frame.callee()->needsCallObject() && script->argumentsAliasesFormals()) {
        obj->initFixedSlot(MAYBE_CALL_SLOT, ObjectValue(frame.callObj()));
        for (PositionalFormalParameterIter fi(script); fi; fi++) {
            if (fi.closedOver())
                data->args[fi.argumentSlot()] = MagicEnvSlotValue(fi.location().slot());
        }
    }
}

/* static */ void
ArgumentsObject::MaybeForwardToCallObject(jit::JitFrameLayout* frame, HandleObject callObj,
                                          ArgumentsObject* obj, ArgumentsData* data)
{
    JSFunction* callee = jit::CalleeTokenToFunction(frame->calleeToken());
    JSScript* script = callee->nonLazyScript();
    if (callee->needsCallObject() && script->argumentsAliasesFormals()) {
        MOZ_ASSERT(callObj && callObj->is<CallObject>());
        obj->initFixedSlot(MAYBE_CALL_SLOT, ObjectValue(*callObj.get()));
        for (PositionalFormalParameterIter fi(script); fi; fi++) {
            if (fi.closedOver())
                data->args[fi.argumentSlot()] = MagicEnvSlotValue(fi.location().slot());
        }
    }
}

struct CopyFrameArgs
{
    AbstractFramePtr frame_;

    explicit CopyFrameArgs(AbstractFramePtr frame)
      : frame_(frame)
    { }

    void copyArgs(JSContext*, GCPtrValue* dst, unsigned totalArgs) const {
        CopyStackFrameArguments(frame_, dst, totalArgs);
    }

    /*
     * If a call object exists and the arguments object aliases formals, the
     * call object is the canonical location for formals.
     */
    void maybeForwardToCallObject(ArgumentsObject* obj, ArgumentsData* data) {
        ArgumentsObject::MaybeForwardToCallObject(frame_, obj, data);
    }
};

struct CopyJitFrameArgs
{
    jit::JitFrameLayout* frame_;
    HandleObject callObj_;

    CopyJitFrameArgs(jit::JitFrameLayout* frame, HandleObject callObj)
      : frame_(frame), callObj_(callObj)
    { }

    void copyArgs(JSContext*, GCPtrValue* dstBase, unsigned totalArgs) const {
        unsigned numActuals = frame_->numActualArgs();
        unsigned numFormals = jit::CalleeTokenToFunction(frame_->calleeToken())->nargs();
        MOZ_ASSERT(numActuals <= totalArgs);
        MOZ_ASSERT(numFormals <= totalArgs);
        MOZ_ASSERT(Max(numActuals, numFormals) == totalArgs);

        /* Copy all arguments. */
        Value* src = frame_->argv() + 1;  /* +1 to skip this. */
        Value* end = src + numActuals;
        GCPtrValue* dst = dstBase;
        while (src != end)
            (dst++)->init(*src++);

        if (numActuals < numFormals) {
            GCPtrValue* dstEnd = dstBase + totalArgs;
            while (dst != dstEnd)
                (dst++)->init(UndefinedValue());
        }
    }

    /*
     * If a call object exists and the arguments object aliases formals, the
     * call object is the canonical location for formals.
     */
    void maybeForwardToCallObject(ArgumentsObject* obj, ArgumentsData* data) {
        ArgumentsObject::MaybeForwardToCallObject(frame_, callObj_, obj, data);
    }
};

struct CopyScriptFrameIterArgs
{
    ScriptFrameIter& iter_;

    explicit CopyScriptFrameIterArgs(ScriptFrameIter& iter)
      : iter_(iter)
    { }

    void copyArgs(JSContext* cx, GCPtrValue* dstBase, unsigned totalArgs) const {
        /* Copy actual arguments. */
        iter_.unaliasedForEachActual(cx, CopyToHeap(dstBase));

        /* Define formals which are not part of the actuals. */
        unsigned numActuals = iter_.numActualArgs();
        unsigned numFormals = iter_.calleeTemplate()->nargs();
        MOZ_ASSERT(numActuals <= totalArgs);
        MOZ_ASSERT(numFormals <= totalArgs);
        MOZ_ASSERT(Max(numActuals, numFormals) == totalArgs);

        if (numActuals < numFormals) {
            GCPtrValue* dst = dstBase + numActuals;
            GCPtrValue* dstEnd = dstBase + totalArgs;
            while (dst != dstEnd)
                (dst++)->init(UndefinedValue());
        }
    }

    /*
     * Ion frames are copying every argument onto the stack, other locations are
     * invalid.
     */
    void maybeForwardToCallObject(ArgumentsObject* obj, ArgumentsData* data) {
        if (!iter_.isIon())
            ArgumentsObject::MaybeForwardToCallObject(iter_.abstractFramePtr(), obj, data);
    }
};

ArgumentsObject*
ArgumentsObject::createTemplateObject(JSContext* cx, bool mapped)
{
    const Class* clasp = mapped
                         ? &MappedArgumentsObject::class_
                         : &UnmappedArgumentsObject::class_;

    RootedObject proto(cx, GlobalObject::getOrCreateObjectPrototype(cx, cx->global()));
    if (!proto)
        return nullptr;

    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, clasp, TaggedProto(proto.get())));
    if (!group)
        return nullptr;

    RootedShape shape(cx, EmptyShape::getInitialShape(cx, clasp, TaggedProto(proto),
                                                      FINALIZE_KIND, BaseShape::INDEXED));
    if (!shape)
        return nullptr;

    AutoSetNewObjectMetadata metadata(cx);
    JSObject* base;
    JS_TRY_VAR_OR_RETURN_NULL(cx, base, NativeObject::create(cx, FINALIZE_KIND, gc::TenuredHeap,
                                                             shape, group));

    ArgumentsObject* obj = &base->as<js::ArgumentsObject>();
    obj->initFixedSlot(ArgumentsObject::DATA_SLOT, PrivateValue(nullptr));
    return obj;
}

ArgumentsObject*
JSCompartment::maybeArgumentsTemplateObject(bool mapped) const
{
    return mapped ? mappedArgumentsTemplate_ : unmappedArgumentsTemplate_;
}

ArgumentsObject*
JSCompartment::getOrCreateArgumentsTemplateObject(JSContext* cx, bool mapped)
{
    ReadBarriered<ArgumentsObject*>& obj =
        mapped ? mappedArgumentsTemplate_ : unmappedArgumentsTemplate_;

    ArgumentsObject* templateObj = obj;
    if (templateObj)
        return templateObj;

    templateObj = ArgumentsObject::createTemplateObject(cx, mapped);
    if (!templateObj)
        return nullptr;

    obj.set(templateObj);
    return templateObj;
}

template <typename CopyArgs>
/* static */ ArgumentsObject*
ArgumentsObject::create(JSContext* cx, HandleFunction callee, unsigned numActuals, CopyArgs& copy)
{
    bool mapped = callee->nonLazyScript()->hasMappedArgsObj();
    ArgumentsObject* templateObj = cx->compartment()->getOrCreateArgumentsTemplateObject(cx, mapped);
    if (!templateObj)
        return nullptr;

    RootedShape shape(cx, templateObj->lastProperty());
    RootedObjectGroup group(cx, templateObj->group());

    unsigned numFormals = callee->nargs();
    unsigned numArgs = Max(numActuals, numFormals);
    unsigned numBytes = ArgumentsData::bytesRequired(numArgs);

    Rooted<ArgumentsObject*> obj(cx);
    ArgumentsData* data = nullptr;
    {
        // The copyArgs call below can allocate objects, so add this block scope
        // to make sure we set the metadata for this arguments object first.
        AutoSetNewObjectMetadata metadata(cx);

        JSObject* base;
        JS_TRY_VAR_OR_RETURN_NULL(cx, base, NativeObject::create(cx, FINALIZE_KIND,
                                                                 gc::DefaultHeap, shape, group));
        obj = &base->as<ArgumentsObject>();

        data =
            reinterpret_cast<ArgumentsData*>(AllocateObjectBuffer<uint8_t>(cx, obj, numBytes));
        if (!data) {
            // Make the object safe for GC.
            obj->initFixedSlot(DATA_SLOT, PrivateValue(nullptr));
            return nullptr;
        }

        data->numArgs = numArgs;
        data->rareData = nullptr;

        // Zero the argument Values. This sets each value to DoubleValue(0), which
        // is safe for GC tracing.
        memset(data->args, 0, numArgs * sizeof(Value));
        MOZ_ASSERT(DoubleValue(0).asRawBits() == 0x0);
        MOZ_ASSERT_IF(numArgs > 0, data->args[0].asRawBits() == 0x0);

        obj->initFixedSlot(DATA_SLOT, PrivateValue(data));
        obj->initFixedSlot(CALLEE_SLOT, ObjectValue(*callee));
    }
    MOZ_ASSERT(data != nullptr);

    /* Copy [0, numArgs) into data->slots. */
    copy.copyArgs(cx, data->args, numArgs);

    obj->initFixedSlot(INITIAL_LENGTH_SLOT, Int32Value(numActuals << PACKED_BITS_COUNT));

    copy.maybeForwardToCallObject(obj, data);

    MOZ_ASSERT(obj->initialLength() == numActuals);
    MOZ_ASSERT(!obj->hasOverriddenLength());
    return obj;
}

ArgumentsObject*
ArgumentsObject::createExpected(JSContext* cx, AbstractFramePtr frame)
{
    MOZ_ASSERT(frame.script()->needsArgsObj());
    RootedFunction callee(cx, frame.callee());
    CopyFrameArgs copy(frame);
    ArgumentsObject* argsobj = create(cx, callee, frame.numActualArgs(), copy);
    if (!argsobj)
        return nullptr;

    frame.initArgsObj(*argsobj);
    return argsobj;
}

ArgumentsObject*
ArgumentsObject::createUnexpected(JSContext* cx, ScriptFrameIter& iter)
{
    RootedFunction callee(cx, iter.callee(cx));
    CopyScriptFrameIterArgs copy(iter);
    return create(cx, callee, iter.numActualArgs(), copy);
}

ArgumentsObject*
ArgumentsObject::createUnexpected(JSContext* cx, AbstractFramePtr frame)
{
    RootedFunction callee(cx, frame.callee());
    CopyFrameArgs copy(frame);
    return create(cx, callee, frame.numActualArgs(), copy);
}

ArgumentsObject*
ArgumentsObject::createForIon(JSContext* cx, jit::JitFrameLayout* frame, HandleObject scopeChain)
{
    jit::CalleeToken token = frame->calleeToken();
    MOZ_ASSERT(jit::CalleeTokenIsFunction(token));
    RootedFunction callee(cx, jit::CalleeTokenToFunction(token));
    RootedObject callObj(cx, scopeChain->is<CallObject>() ? scopeChain.get() : nullptr);
    CopyJitFrameArgs copy(frame, callObj);
    return create(cx, callee, frame->numActualArgs(), copy);
}

/* static */ ArgumentsObject*
ArgumentsObject::finishForIon(JSContext* cx, jit::JitFrameLayout* frame,
                              JSObject* scopeChain, ArgumentsObject* obj)
{
    // JIT code calls this directly (no callVM), because it's faster, so we're
    // not allowed to GC in here.
    AutoUnsafeCallWithABI unsafe;

    JSFunction* callee = jit::CalleeTokenToFunction(frame->calleeToken());
    RootedObject callObj(cx, scopeChain->is<CallObject>() ? scopeChain : nullptr);
    CopyJitFrameArgs copy(frame, callObj);

    unsigned numActuals = frame->numActualArgs();
    unsigned numFormals = callee->nargs();
    unsigned numArgs = Max(numActuals, numFormals);
    unsigned numBytes = ArgumentsData::bytesRequired(numArgs);

    ArgumentsData* data =
        reinterpret_cast<ArgumentsData*>(AllocateObjectBuffer<uint8_t>(cx, obj, numBytes));
    if (!data) {
        // Make the object safe for GC. Don't report OOM, the slow path will
        // retry the allocation.
        cx->recoverFromOutOfMemory();
        obj->initFixedSlot(DATA_SLOT, PrivateValue(nullptr));
        return nullptr;
    }

    data->numArgs = numArgs;
    data->rareData = nullptr;

    obj->initFixedSlot(INITIAL_LENGTH_SLOT, Int32Value(numActuals << PACKED_BITS_COUNT));
    obj->initFixedSlot(DATA_SLOT, PrivateValue(data));
    obj->initFixedSlot(MAYBE_CALL_SLOT, UndefinedValue());
    obj->initFixedSlot(CALLEE_SLOT, ObjectValue(*callee));

    copy.copyArgs(cx, data->args, numArgs);

    if (callObj && callee->needsCallObject())
        copy.maybeForwardToCallObject(obj, data);

    MOZ_ASSERT(obj->initialLength() == numActuals);
    MOZ_ASSERT(!obj->hasOverriddenLength());
    return obj;
}

/* static */ bool
ArgumentsObject::obj_delProperty(JSContext* cx, HandleObject obj, HandleId id,
                                 ObjectOpResult& result)
{
    ArgumentsObject& argsobj = obj->as<ArgumentsObject>();
    if (JSID_IS_INT(id)) {
        unsigned arg = unsigned(JSID_TO_INT(id));
        if (arg < argsobj.initialLength() && !argsobj.isElementDeleted(arg)) {
            if (!argsobj.markElementDeleted(cx, arg))
                return false;
        }
    } else if (JSID_IS_ATOM(id, cx->names().length)) {
        argsobj.markLengthOverridden();
    } else if (JSID_IS_ATOM(id, cx->names().callee)) {
        argsobj.as<MappedArgumentsObject>().markCalleeOverridden();
    } else if (JSID_IS_SYMBOL(id) && JSID_TO_SYMBOL(id) == cx->wellKnownSymbols().iterator) {
        argsobj.markIteratorOverridden();
    }
    return result.succeed();
}

/* static */ bool
ArgumentsObject::obj_mayResolve(const JSAtomState& names, jsid id, JSObject*)
{
    // Arguments might resolve indexes, Symbol.iterator, or length/callee.
    if (JSID_IS_ATOM(id)) {
        JSAtom* atom = JSID_TO_ATOM(id);
        uint32_t index;
        if (atom->isIndex(&index))
            return true;
        return atom == names.length || atom == names.callee;
    }
    if (JSID_IS_SYMBOL(id))
        return JSID_TO_SYMBOL(id)->code() == JS::SymbolCode::iterator;
    return true;
}

static bool
MappedArgGetter(JSContext* cx, HandleObject obj, HandleId id, MutableHandleValue vp)
{
    MappedArgumentsObject& argsobj = obj->as<MappedArgumentsObject>();
    if (JSID_IS_INT(id)) {
        /*
         * arg can exceed the number of arguments if a script changed the
         * prototype to point to another Arguments object with a bigger argc.
         */
        unsigned arg = unsigned(JSID_TO_INT(id));
        if (arg < argsobj.initialLength() && !argsobj.isElementDeleted(arg))
            vp.set(argsobj.element(arg));
    } else if (JSID_IS_ATOM(id, cx->names().length)) {
        if (!argsobj.hasOverriddenLength())
            vp.setInt32(argsobj.initialLength());
    } else {
        MOZ_ASSERT(JSID_IS_ATOM(id, cx->names().callee));
        if (!argsobj.hasOverriddenCallee()) {
            RootedFunction callee(cx, &argsobj.callee());
            if (callee->isAsync())
                vp.setObject(*GetWrappedAsyncFunction(callee));
            else
                vp.setObject(*callee);
        }
    }
    return true;
}

static bool
MappedArgSetter(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                ObjectOpResult& result)
{
    if (!obj->is<MappedArgumentsObject>())
        return result.succeed();
    Handle<MappedArgumentsObject*> argsobj = obj.as<MappedArgumentsObject>();

    Rooted<PropertyDescriptor> desc(cx);
    if (!GetOwnPropertyDescriptor(cx, argsobj, id, &desc))
        return false;
    MOZ_ASSERT(desc.object());
    unsigned attrs = desc.attributes();
    MOZ_ASSERT(!(attrs & JSPROP_READONLY));
    attrs &= (JSPROP_ENUMERATE | JSPROP_PERMANENT); /* only valid attributes */

    RootedFunction callee(cx, &argsobj->callee());
    RootedScript script(cx, JSFunction::getOrCreateScript(cx, callee));
    if (!script)
        return false;

    if (JSID_IS_INT(id)) {
        unsigned arg = unsigned(JSID_TO_INT(id));
        if (arg < argsobj->initialLength() && !argsobj->isElementDeleted(arg)) {
            argsobj->setElement(cx, arg, v);
            if (arg < script->functionNonDelazifying()->nargs())
                TypeScript::SetArgument(cx, script, arg, v);
            return result.succeed();
        }
    } else {
        MOZ_ASSERT(JSID_IS_ATOM(id, cx->names().length) || JSID_IS_ATOM(id, cx->names().callee));
    }

    /*
     * For simplicity we use delete/define to replace the property with a
     * simple data property. Note that we rely on ArgumentsObject::obj_delProperty
     * to set the corresponding override-bit.
     * Note also that we must define the property instead of setting it in case
     * the user has changed the prototype to an object that has a setter for
     * this id.
     */
    ObjectOpResult ignored;
    return NativeDeleteProperty(cx, argsobj, id, ignored) &&
           NativeDefineDataProperty(cx, argsobj, id, v, attrs, result);
}

static bool
DefineArgumentsIterator(JSContext* cx, Handle<ArgumentsObject*> argsobj)
{
    RootedId iteratorId(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator));
    HandlePropertyName shName = cx->names().ArrayValues;
    RootedAtom name(cx, cx->names().values);
    RootedValue val(cx);
    if (!GlobalObject::getSelfHostedFunction(cx, cx->global(), shName, name, 0, &val))
        return false;
    return NativeDefineDataProperty(cx, argsobj, iteratorId, val, JSPROP_RESOLVING);
}

/* static */ bool
ArgumentsObject::reifyLength(JSContext* cx, Handle<ArgumentsObject*> obj)
{
    if (obj->hasOverriddenLength())
        return true;

    RootedId id(cx, NameToId(cx->names().length));
    RootedValue val(cx, Int32Value(obj->initialLength()));
    if (!NativeDefineDataProperty(cx, obj, id, val, JSPROP_RESOLVING))
        return false;

    obj->markLengthOverridden();
    return true;
}

/* static */ bool
ArgumentsObject::reifyIterator(JSContext* cx, Handle<ArgumentsObject*> obj)
{
    if (obj->hasOverriddenIterator())
        return true;

    if (!DefineArgumentsIterator(cx, obj))
        return false;

    obj->markIteratorOverridden();
    return true;
}

/* static */ bool
MappedArgumentsObject::obj_resolve(JSContext* cx, HandleObject obj, HandleId id, bool* resolvedp)
{
    Rooted<MappedArgumentsObject*> argsobj(cx, &obj->as<MappedArgumentsObject>());

    if (JSID_IS_SYMBOL(id) && JSID_TO_SYMBOL(id) == cx->wellKnownSymbols().iterator) {
        if (argsobj->hasOverriddenIterator())
            return true;

        if (!DefineArgumentsIterator(cx, argsobj))
            return false;
        *resolvedp = true;
        return true;
    }

    unsigned attrs = JSPROP_SHADOWABLE | JSPROP_RESOLVING;
    if (JSID_IS_INT(id)) {
        uint32_t arg = uint32_t(JSID_TO_INT(id));
        if (arg >= argsobj->initialLength() || argsobj->isElementDeleted(arg))
            return true;

        attrs |= JSPROP_ENUMERATE;
    } else if (JSID_IS_ATOM(id, cx->names().length)) {
        if (argsobj->hasOverriddenLength())
            return true;
    } else {
        if (!JSID_IS_ATOM(id, cx->names().callee))
            return true;

        if (argsobj->hasOverriddenCallee())
            return true;
    }

    if (!NativeDefineAccessorProperty(cx, argsobj, id, MappedArgGetter, MappedArgSetter, attrs))
        return false;

    *resolvedp = true;
    return true;
}

/* static */ bool
MappedArgumentsObject::obj_enumerate(JSContext* cx, HandleObject obj)
{
    Rooted<MappedArgumentsObject*> argsobj(cx, &obj->as<MappedArgumentsObject>());

    RootedId id(cx);
    bool found;

    // Trigger reflection.
    id = NameToId(cx->names().length);
    if (!HasOwnProperty(cx, argsobj, id, &found))
        return false;

    id = NameToId(cx->names().callee);
    if (!HasOwnProperty(cx, argsobj, id, &found))
        return false;

    id = SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator);
    if (!HasOwnProperty(cx, argsobj, id, &found))
        return false;

    for (unsigned i = 0; i < argsobj->initialLength(); i++) {
        id = INT_TO_JSID(i);
        if (!HasOwnProperty(cx, argsobj, id, &found))
            return false;
    }

    return true;
}

// ES 2017 draft 9.4.4.2
/* static */ bool
MappedArgumentsObject::obj_defineProperty(JSContext* cx, HandleObject obj, HandleId id,
                                          Handle<PropertyDescriptor> desc, ObjectOpResult& result)
{
    // Step 1.
    Rooted<MappedArgumentsObject*> argsobj(cx, &obj->as<MappedArgumentsObject>());

    // Steps 2-3.
    bool isMapped = false;
    if (JSID_IS_INT(id)) {
        unsigned arg = unsigned(JSID_TO_INT(id));
        isMapped = arg < argsobj->initialLength() && !argsobj->isElementDeleted(arg);
    }

    // Step 4.
    Rooted<PropertyDescriptor> newArgDesc(cx, desc);

    // Step 5.
    if (!desc.isAccessorDescriptor() && isMapped) {
        // Step 5.a.
        if (desc.hasWritable() && !desc.writable()) {
            if (!desc.hasValue()) {
                RootedValue v(cx, argsobj->element(JSID_TO_INT(id)));
                newArgDesc.setValue(v);
            }
            newArgDesc.setGetter(nullptr);
            newArgDesc.setSetter(nullptr);
        } else {
            // In this case the live mapping is supposed to keep working,
            // we have to pass along the Getter/Setter otherwise they are
            // overwritten.
            newArgDesc.setGetter(MappedArgGetter);
            newArgDesc.setSetter(MappedArgSetter);
            newArgDesc.value().setUndefined();
            newArgDesc.attributesRef() |= JSPROP_IGNORE_VALUE;
        }
    }

    // Step 6. NativeDefineProperty will lookup [[Value]] for us.
    if (!NativeDefineProperty(cx, obj.as<NativeObject>(), id, newArgDesc, result))
        return false;
    // Step 7.
    if (!result.ok())
        return true;

    // Step 8.
    if (isMapped) {
        unsigned arg = unsigned(JSID_TO_INT(id));
        if (desc.isAccessorDescriptor()) {
            if (!argsobj->markElementDeleted(cx, arg))
                return false;
        } else {
            if (desc.hasValue()) {
                RootedFunction callee(cx, &argsobj->callee());
                RootedScript script(cx, JSFunction::getOrCreateScript(cx, callee));
                if (!script)
                    return false;
                argsobj->setElement(cx, arg, desc.value());
                if (arg < script->functionNonDelazifying()->nargs())
                    TypeScript::SetArgument(cx, script, arg, desc.value());
            }
            if (desc.hasWritable() && !desc.writable()) {
                if (!argsobj->markElementDeleted(cx, arg))
                    return false;
            }
        }
    }

    // Step 9.
    return result.succeed();
}

static bool
UnmappedArgGetter(JSContext* cx, HandleObject obj, HandleId id, MutableHandleValue vp)
{
    UnmappedArgumentsObject& argsobj = obj->as<UnmappedArgumentsObject>();

    if (JSID_IS_INT(id)) {
        /*
         * arg can exceed the number of arguments if a script changed the
         * prototype to point to another Arguments object with a bigger argc.
         */
        unsigned arg = unsigned(JSID_TO_INT(id));
        if (arg < argsobj.initialLength() && !argsobj.isElementDeleted(arg))
            vp.set(argsobj.element(arg));
    } else {
        MOZ_ASSERT(JSID_IS_ATOM(id, cx->names().length));
        if (!argsobj.hasOverriddenLength())
            vp.setInt32(argsobj.initialLength());
    }
    return true;
}

static bool
UnmappedArgSetter(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                  ObjectOpResult& result)
{
    if (!obj->is<UnmappedArgumentsObject>())
        return result.succeed();
    Handle<UnmappedArgumentsObject*> argsobj = obj.as<UnmappedArgumentsObject>();

    Rooted<PropertyDescriptor> desc(cx);
    if (!GetOwnPropertyDescriptor(cx, argsobj, id, &desc))
        return false;
    MOZ_ASSERT(desc.object());
    unsigned attrs = desc.attributes();
    MOZ_ASSERT(!(attrs & JSPROP_READONLY));
    attrs &= (JSPROP_ENUMERATE | JSPROP_PERMANENT); /* only valid attributes */

    if (JSID_IS_INT(id)) {
        unsigned arg = unsigned(JSID_TO_INT(id));
        if (arg < argsobj->initialLength()) {
            argsobj->setElement(cx, arg, v);
            return result.succeed();
        }
    } else {
        MOZ_ASSERT(JSID_IS_ATOM(id, cx->names().length));
    }

    /*
     * For simplicity we use delete/define to replace the property with a
     * simple data property. Note that we rely on ArgumentsObject::obj_delProperty
     * to set the corresponding override-bit.
     */
    ObjectOpResult ignored;
    return NativeDeleteProperty(cx, argsobj, id, ignored) &&
           NativeDefineDataProperty(cx, argsobj, id, v, attrs, result);
}

/* static */ bool
UnmappedArgumentsObject::obj_resolve(JSContext* cx, HandleObject obj, HandleId id, bool* resolvedp)
{
    Rooted<UnmappedArgumentsObject*> argsobj(cx, &obj->as<UnmappedArgumentsObject>());

    if (JSID_IS_SYMBOL(id) && JSID_TO_SYMBOL(id) == cx->wellKnownSymbols().iterator) {
        if (argsobj->hasOverriddenIterator())
            return true;

        if (!DefineArgumentsIterator(cx, argsobj))
            return false;
        *resolvedp = true;
        return true;
    }

    unsigned attrs = JSPROP_SHADOWABLE;
    GetterOp getter = UnmappedArgGetter;
    SetterOp setter = UnmappedArgSetter;

    if (JSID_IS_INT(id)) {
        uint32_t arg = uint32_t(JSID_TO_INT(id));
        if (arg >= argsobj->initialLength() || argsobj->isElementDeleted(arg))
            return true;

        attrs |= JSPROP_ENUMERATE;
    } else if (JSID_IS_ATOM(id, cx->names().length)) {
        if (argsobj->hasOverriddenLength())
            return true;
    } else {
        if (!JSID_IS_ATOM(id, cx->names().callee))
            return true;

        JSObject* throwTypeError = GlobalObject::getOrCreateThrowTypeError(cx, cx->global());
        if (!throwTypeError)
            return false;

        attrs = JSPROP_PERMANENT | JSPROP_GETTER | JSPROP_SETTER;
        getter = CastAsGetterOp(throwTypeError);
        setter = CastAsSetterOp(throwTypeError);
    }

    attrs |= JSPROP_RESOLVING;
    if (!NativeDefineAccessorProperty(cx, argsobj, id, getter, setter, attrs))
        return false;

    *resolvedp = true;
    return true;
}

/* static */ bool
UnmappedArgumentsObject::obj_enumerate(JSContext* cx, HandleObject obj)
{
    Rooted<UnmappedArgumentsObject*> argsobj(cx, &obj->as<UnmappedArgumentsObject>());

    RootedId id(cx);
    bool found;

    // Trigger reflection.
    id = NameToId(cx->names().length);
    if (!HasOwnProperty(cx, argsobj, id, &found))
        return false;

    id = NameToId(cx->names().callee);
    if (!HasOwnProperty(cx, argsobj, id, &found))
        return false;

    id = SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator);
    if (!HasOwnProperty(cx, argsobj, id, &found))
        return false;

    for (unsigned i = 0; i < argsobj->initialLength(); i++) {
        id = INT_TO_JSID(i);
        if (!HasOwnProperty(cx, argsobj, id, &found))
            return false;
    }

    return true;
}

void
ArgumentsObject::finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(!IsInsideNursery(obj));
    if (obj->as<ArgumentsObject>().data()) {
        fop->free_(obj->as<ArgumentsObject>().maybeRareData());
        fop->free_(obj->as<ArgumentsObject>().data());
    }
}

void
ArgumentsObject::trace(JSTracer* trc, JSObject* obj)
{
    ArgumentsObject& argsobj = obj->as<ArgumentsObject>();
    if (ArgumentsData* data = argsobj.data()) // Template objects have no ArgumentsData.
        TraceRange(trc, data->numArgs, data->begin(), js_arguments_str);
}

/* static */ size_t
ArgumentsObject::objectMoved(JSObject* dst, JSObject* src)
{
    ArgumentsObject* ndst = &dst->as<ArgumentsObject>();
    const ArgumentsObject* nsrc = &src->as<ArgumentsObject>();
    MOZ_ASSERT(ndst->data() == nsrc->data());

    if (!IsInsideNursery(src))
        return 0;

    Nursery& nursery = dst->zone()->group()->nursery();

    size_t nbytesTotal = 0;
    if (!nursery.isInside(nsrc->data())) {
        nursery.removeMallocedBuffer(nsrc->data());
    } else {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        uint32_t nbytes = ArgumentsData::bytesRequired(nsrc->data()->numArgs);
        uint8_t* data = nsrc->zone()->pod_malloc<uint8_t>(nbytes);
        if (!data)
            oomUnsafe.crash("Failed to allocate ArgumentsObject data while tenuring.");
        ndst->initFixedSlot(DATA_SLOT, PrivateValue(data));

        mozilla::PodCopy(data, reinterpret_cast<uint8_t*>(nsrc->data()), nbytes);
        nbytesTotal += nbytes;
    }

    if (RareArgumentsData* srcRareData = nsrc->maybeRareData()) {
        if (!nursery.isInside(srcRareData)) {
            nursery.removeMallocedBuffer(srcRareData);
        } else {
            AutoEnterOOMUnsafeRegion oomUnsafe;
            uint32_t nbytes = RareArgumentsData::bytesRequired(nsrc->initialLength());
            uint8_t* dstRareData = nsrc->zone()->pod_malloc<uint8_t>(nbytes);
            if (!dstRareData)
                oomUnsafe.crash("Failed to allocate RareArgumentsData data while tenuring.");
            ndst->data()->rareData = (RareArgumentsData*)dstRareData;

            mozilla::PodCopy(dstRareData, reinterpret_cast<uint8_t*>(srcRareData), nbytes);
            nbytesTotal += nbytes;
        }
    }

    return nbytesTotal;
}

/*
 * The classes below collaborate to lazily reflect and synchronize actual
 * argument values, argument count, and callee function object stored in a
 * stack frame with their corresponding property values in the frame's
 * arguments object.
 */
const ClassOps MappedArgumentsObject::classOps_ = {
    nullptr,                 /* addProperty */
    ArgumentsObject::obj_delProperty,
    MappedArgumentsObject::obj_enumerate,
    nullptr,                 /* newEnumerate */
    MappedArgumentsObject::obj_resolve,
    ArgumentsObject::obj_mayResolve,
    ArgumentsObject::finalize,
    nullptr,                 /* call        */
    nullptr,                 /* hasInstance */
    nullptr,                 /* construct   */
    ArgumentsObject::trace
};

const js::ClassExtension MappedArgumentsObject::classExt_ = {
    nullptr,                      /* weakmapKeyDelegateOp */
    ArgumentsObject::objectMoved  /* objectMovedOp */
};

const ObjectOps MappedArgumentsObject::objectOps_ = {
    nullptr,                 /* lookupProperty */
    MappedArgumentsObject::obj_defineProperty
};

const Class MappedArgumentsObject::class_ = {
    "Arguments",
    JSCLASS_DELAY_METADATA_BUILDER |
    JSCLASS_HAS_RESERVED_SLOTS(MappedArgumentsObject::RESERVED_SLOTS) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Object) |
    JSCLASS_SKIP_NURSERY_FINALIZE |
    JSCLASS_BACKGROUND_FINALIZE,
    &MappedArgumentsObject::classOps_,
    nullptr,
    &MappedArgumentsObject::classExt_,
    &MappedArgumentsObject::objectOps_
};

/*
 * Unmapped arguments is significantly less magical than mapped arguments, so
 * it is represented by a different class while sharing some functionality.
 */
const ClassOps UnmappedArgumentsObject::classOps_ = {
    nullptr,                 /* addProperty */
    ArgumentsObject::obj_delProperty,
    UnmappedArgumentsObject::obj_enumerate,
    nullptr,                 /* newEnumerate */
    UnmappedArgumentsObject::obj_resolve,
    ArgumentsObject::obj_mayResolve,
    ArgumentsObject::finalize,
    nullptr,                 /* call        */
    nullptr,                 /* hasInstance */
    nullptr,                 /* construct   */
    ArgumentsObject::trace
};

const js::ClassExtension UnmappedArgumentsObject::classExt_ = {
    nullptr,                      /* weakmapKeyDelegateOp */
    ArgumentsObject::objectMoved  /* objectMovedOp */
};

const Class UnmappedArgumentsObject::class_ = {
    "Arguments",
    JSCLASS_DELAY_METADATA_BUILDER |
    JSCLASS_HAS_RESERVED_SLOTS(UnmappedArgumentsObject::RESERVED_SLOTS) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Object) |
    JSCLASS_SKIP_NURSERY_FINALIZE |
    JSCLASS_BACKGROUND_FINALIZE,
    &UnmappedArgumentsObject::classOps_,
    nullptr,
    &UnmappedArgumentsObject::classExt_
};
