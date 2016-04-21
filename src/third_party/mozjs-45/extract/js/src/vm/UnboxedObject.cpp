/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/UnboxedObject-inl.h"

#include "jit/BaselineIC.h"
#include "jit/JitCommon.h"
#include "jit/Linker.h"

#include "jsobjinlines.h"

#include "gc/Nursery-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "vm/Shape-inl.h"

using mozilla::ArrayLength;
using mozilla::DebugOnly;
using mozilla::PodCopy;
using mozilla::UniquePtr;

using namespace js;

/////////////////////////////////////////////////////////////////////
// UnboxedLayout
/////////////////////////////////////////////////////////////////////

void
UnboxedLayout::trace(JSTracer* trc)
{
    for (size_t i = 0; i < properties_.length(); i++)
        TraceManuallyBarrieredEdge(trc, &properties_[i].name, "unboxed_layout_name");

    if (newScript())
        newScript()->trace(trc);

    if (nativeGroup_)
        TraceEdge(trc, &nativeGroup_, "unboxed_layout_nativeGroup");

    if (nativeShape_)
        TraceEdge(trc, &nativeShape_, "unboxed_layout_nativeShape");

    if (allocationScript_)
        TraceEdge(trc, &allocationScript_, "unboxed_layout_allocationScript");

    if (replacementGroup_)
        TraceEdge(trc, &replacementGroup_, "unboxed_layout_replacementGroup");

    if (constructorCode_)
        TraceEdge(trc, &constructorCode_, "unboxed_layout_constructorCode");
}

size_t
UnboxedLayout::sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf)
{
    return mallocSizeOf(this)
         + properties_.sizeOfExcludingThis(mallocSizeOf)
         + (newScript() ? newScript()->sizeOfIncludingThis(mallocSizeOf) : 0)
         + mallocSizeOf(traceList());
}

void
UnboxedLayout::setNewScript(TypeNewScript* newScript, bool writeBarrier /* = true */)
{
    if (newScript_ && writeBarrier)
        TypeNewScript::writeBarrierPre(newScript_);
    newScript_ = newScript;
}

// Constructor code returns a 0x1 value to indicate the constructor code should
// be cleared.
static const uintptr_t CLEAR_CONSTRUCTOR_CODE_TOKEN = 0x1;

/* static */ bool
UnboxedLayout::makeConstructorCode(JSContext* cx, HandleObjectGroup group)
{
    gc::AutoSuppressGC suppress(cx);

    using namespace jit;

    if (!cx->compartment()->ensureJitCompartmentExists(cx))
        return false;

    UnboxedLayout& layout = group->unboxedLayout();
    MOZ_ASSERT(!layout.constructorCode());

    UnboxedPlainObject* templateObject = UnboxedPlainObject::create(cx, group, TenuredObject);
    if (!templateObject)
        return false;

    JitContext jitContext(cx, nullptr);

    MacroAssembler masm;

    Register propertiesReg, newKindReg;
#ifdef JS_CODEGEN_X86
    propertiesReg = eax;
    newKindReg = ecx;
    masm.loadPtr(Address(masm.getStackPointer(), sizeof(void*)), propertiesReg);
    masm.loadPtr(Address(masm.getStackPointer(), 2 * sizeof(void*)), newKindReg);
#else
    propertiesReg = IntArgReg0;
    newKindReg = IntArgReg1;
#endif

    MOZ_ASSERT(propertiesReg.volatile_());
    MOZ_ASSERT(newKindReg.volatile_());

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(propertiesReg);
    regs.take(newKindReg);
    Register object = regs.takeAny(), scratch1 = regs.takeAny(), scratch2 = regs.takeAny();

    LiveGeneralRegisterSet savedNonVolatileRegisters = SavedNonVolatileRegisters(regs);
    masm.PushRegsInMask(savedNonVolatileRegisters);

    // The scratch double register might be used by MacroAssembler methods.
    if (ScratchDoubleReg.volatile_())
        masm.push(ScratchDoubleReg);

    Label failure, tenuredObject, allocated;
    masm.branch32(Assembler::NotEqual, newKindReg, Imm32(GenericObject), &tenuredObject);
    masm.branchTest32(Assembler::NonZero, AbsoluteAddress(group->addressOfFlags()),
                      Imm32(OBJECT_FLAG_PRE_TENURE), &tenuredObject);

    // Allocate an object in the nursery
    masm.createGCObject(object, scratch1, templateObject, gc::DefaultHeap, &failure,
                        /* initFixedSlots = */ false);

    masm.jump(&allocated);
    masm.bind(&tenuredObject);

    // Allocate an object in the tenured heap.
    masm.createGCObject(object, scratch1, templateObject, gc::TenuredHeap, &failure,
                        /* initFixedSlots = */ false);

    // If any of the properties being stored are in the nursery, add a store
    // buffer entry for the new object.
    Label postBarrier;
    for (size_t i = 0; i < layout.properties().length(); i++) {
        const UnboxedLayout::Property& property = layout.properties()[i];
        if (property.type == JSVAL_TYPE_OBJECT) {
            Address valueAddress(propertiesReg, i * sizeof(IdValuePair) + offsetof(IdValuePair, value));
            Label notObject;
            masm.branchTestObject(Assembler::NotEqual, valueAddress, &notObject);
            Register valueObject = masm.extractObject(valueAddress, scratch1);
            masm.branchPtrInNurseryRange(Assembler::Equal, valueObject, scratch2, &postBarrier);
            masm.bind(&notObject);
        }
    }

    masm.jump(&allocated);
    masm.bind(&postBarrier);

    LiveGeneralRegisterSet liveVolatileRegisters;
    liveVolatileRegisters.add(propertiesReg);
    if (object.volatile_())
        liveVolatileRegisters.add(object);
    masm.PushRegsInMask(liveVolatileRegisters);

    masm.mov(ImmPtr(cx->runtime()), scratch1);
    masm.setupUnalignedABICall(scratch2);
    masm.passABIArg(scratch1);
    masm.passABIArg(object);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, PostWriteBarrier));

    masm.PopRegsInMask(liveVolatileRegisters);

    masm.bind(&allocated);

    ValueOperand valueOperand;
#ifdef JS_NUNBOX32
    valueOperand = ValueOperand(scratch1, scratch2);
#else
    valueOperand = ValueOperand(scratch1);
#endif

    Label failureStoreOther, failureStoreObject;

    for (size_t i = 0; i < layout.properties().length(); i++) {
        const UnboxedLayout::Property& property = layout.properties()[i];
        Address valueAddress(propertiesReg, i * sizeof(IdValuePair) + offsetof(IdValuePair, value));
        Address targetAddress(object, UnboxedPlainObject::offsetOfData() + property.offset);

        masm.loadValue(valueAddress, valueOperand);

        if (property.type == JSVAL_TYPE_OBJECT) {
            HeapTypeSet* types = group->maybeGetProperty(IdToTypeId(NameToId(property.name)));

            Label notObject;
            masm.branchTestObject(Assembler::NotEqual, valueOperand,
                                  types->mightBeMIRType(MIRType_Null) ? &notObject : &failureStoreObject);

            Register payloadReg = masm.extractObject(valueOperand, scratch1);

            if (!types->hasType(TypeSet::AnyObjectType())) {
                Register scratch = (payloadReg == scratch1) ? scratch2 : scratch1;
                masm.guardObjectType(payloadReg, types, scratch, &failureStoreObject);
            }

            masm.storeUnboxedProperty(targetAddress, JSVAL_TYPE_OBJECT,
                                      TypedOrValueRegister(MIRType_Object,
                                                           AnyRegister(payloadReg)), nullptr);

            if (notObject.used()) {
                Label done;
                masm.jump(&done);
                masm.bind(&notObject);
                masm.branchTestNull(Assembler::NotEqual, valueOperand, &failureStoreOther);
                masm.storeUnboxedProperty(targetAddress, JSVAL_TYPE_OBJECT, NullValue(), nullptr);
                masm.bind(&done);
            }
        } else {
            masm.storeUnboxedProperty(targetAddress, property.type,
                                      ConstantOrRegister(valueOperand), &failureStoreOther);
        }
    }

    Label done;
    masm.bind(&done);

    if (object != ReturnReg)
        masm.movePtr(object, ReturnReg);

    // Restore non-volatile registers which were saved on entry.
    if (ScratchDoubleReg.volatile_())
        masm.pop(ScratchDoubleReg);
    masm.PopRegsInMask(savedNonVolatileRegisters);

    masm.abiret();

    masm.bind(&failureStoreOther);

    // There was a failure while storing a value which cannot be stored at all
    // in the unboxed object. Initialize the object so it is safe for GC and
    // return null.
    masm.initUnboxedObjectContents(object, templateObject);

    masm.bind(&failure);

    masm.movePtr(ImmWord(0), object);
    masm.jump(&done);

    masm.bind(&failureStoreObject);

    // There was a failure while storing a value to an object slot of the
    // unboxed object. If the value is storable, the failure occurred due to
    // incomplete type information in the object, so return a token to trigger
    // regeneration of the jitcode after a new object is created in the VM.
    {
        Label isObject;
        masm.branchTestObject(Assembler::Equal, valueOperand, &isObject);
        masm.branchTestNull(Assembler::NotEqual, valueOperand, &failureStoreOther);
        masm.bind(&isObject);
    }

    // Initialize the object so it is safe for GC.
    masm.initUnboxedObjectContents(object, templateObject);

    masm.movePtr(ImmWord(CLEAR_CONSTRUCTOR_CODE_TOKEN), object);
    masm.jump(&done);

    Linker linker(masm);
    AutoFlushICache afc("UnboxedObject");
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);
    if (!code)
        return false;

    layout.setConstructorCode(code);
    return true;
}

void
UnboxedLayout::detachFromCompartment()
{
    if (isInList())
        remove();
}

/////////////////////////////////////////////////////////////////////
// UnboxedPlainObject
/////////////////////////////////////////////////////////////////////

bool
UnboxedPlainObject::setValue(ExclusiveContext* cx, const UnboxedLayout::Property& property,
                             const Value& v)
{
    uint8_t* p = &data_[property.offset];
    return SetUnboxedValue(cx, this, NameToId(property.name), p, property.type, v,
                           /* preBarrier = */ true);
}

Value
UnboxedPlainObject::getValue(const UnboxedLayout::Property& property,
                             bool maybeUninitialized /* = false */)
{
    uint8_t* p = &data_[property.offset];
    return GetUnboxedValue(p, property.type, maybeUninitialized);
}

void
UnboxedPlainObject::trace(JSTracer* trc, JSObject* obj)
{
    if (obj->as<UnboxedPlainObject>().expando_) {
        TraceManuallyBarrieredEdge(trc,
            reinterpret_cast<NativeObject**>(&obj->as<UnboxedPlainObject>().expando_),
            "unboxed_expando");
    }

    const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layoutDontCheckGeneration();
    const int32_t* list = layout.traceList();
    if (!list)
        return;

    uint8_t* data = obj->as<UnboxedPlainObject>().data();
    while (*list != -1) {
        HeapPtrString* heap = reinterpret_cast<HeapPtrString*>(data + *list);
        TraceEdge(trc, heap, "unboxed_string");
        list++;
    }
    list++;
    while (*list != -1) {
        HeapPtrObject* heap = reinterpret_cast<HeapPtrObject*>(data + *list);
        if (*heap)
            TraceEdge(trc, heap, "unboxed_object");
        list++;
    }

    // Unboxed objects don't have Values to trace.
    MOZ_ASSERT(*(list + 1) == -1);
}

/* static */ UnboxedExpandoObject*
UnboxedPlainObject::ensureExpando(JSContext* cx, Handle<UnboxedPlainObject*> obj)
{
    if (obj->expando_)
        return obj->expando_;

    UnboxedExpandoObject* expando =
        NewObjectWithGivenProto<UnboxedExpandoObject>(cx, nullptr, gc::AllocKind::OBJECT4);
    if (!expando)
        return nullptr;

    // If the expando is tenured then the original object must also be tenured.
    // Otherwise barriers triggered on the original object for writes to the
    // expando (as can happen in the JIT) won't see the tenured->nursery edge.
    // See WholeCellEdges::mark.
    MOZ_ASSERT_IF(!IsInsideNursery(expando), !IsInsideNursery(obj));

    // As with setValue(), we need to manually trigger post barriers on the
    // whole object. If we treat the field as a HeapPtrObject and later convert
    // the object to its native representation, we will end up with a corrupted
    // store buffer entry.
    if (IsInsideNursery(expando) && !IsInsideNursery(obj))
        cx->runtime()->gc.storeBuffer.putWholeCell(obj);

    obj->expando_ = expando;
    return expando;
}

bool
UnboxedPlainObject::containsUnboxedOrExpandoProperty(ExclusiveContext* cx, jsid id) const
{
    if (layout().lookup(id))
        return true;

    if (maybeExpando() && maybeExpando()->containsShapeOrElement(cx, id))
        return true;

    return false;
}

static bool
PropagatePropertyTypes(JSContext* cx, jsid id, ObjectGroup* oldGroup, ObjectGroup* newGroup)
{
    HeapTypeSet* typeProperty = oldGroup->maybeGetProperty(id);
    TypeSet::TypeList types;
    if (!typeProperty->enumerateTypes(&types)) {
        ReportOutOfMemory(cx);
        return false;
    }
    for (size_t j = 0; j < types.length(); j++)
        AddTypePropertyId(cx, newGroup, nullptr, id, types[j]);
    return true;
}

static PlainObject*
MakeReplacementTemplateObject(JSContext* cx, HandleObjectGroup group, const UnboxedLayout &layout)
{
    PlainObject* obj = NewObjectWithGroup<PlainObject>(cx, group, layout.getAllocKind(),
                                                       TenuredObject);
    if (!obj)
        return nullptr;

    for (size_t i = 0; i < layout.properties().length(); i++) {
        const UnboxedLayout::Property& property = layout.properties()[i];
        if (!obj->addDataProperty(cx, NameToId(property.name), i, JSPROP_ENUMERATE))
            return nullptr;
        MOZ_ASSERT(obj->slotSpan() == i + 1);
        MOZ_ASSERT(!obj->inDictionaryMode());
    }

    return obj;
}

/* static */ bool
UnboxedLayout::makeNativeGroup(JSContext* cx, ObjectGroup* group)
{
    AutoEnterAnalysis enter(cx);

    UnboxedLayout& layout = group->unboxedLayout();
    Rooted<TaggedProto> proto(cx, group->proto());

    MOZ_ASSERT(!layout.nativeGroup());

    RootedObjectGroup replacementGroup(cx);

    const Class* clasp = layout.isArray() ? &ArrayObject::class_ : &PlainObject::class_;

    // Immediately clear any new script on the group. This is done by replacing
    // the existing new script with one for a replacement default new group.
    // This is done so that the size of the replacment group's objects is the
    // same as that for the unboxed group, so that we do not see polymorphic
    // slot accesses later on for sites that see converted objects from this
    // group and objects that were allocated using the replacement new group.
    if (layout.newScript()) {
        MOZ_ASSERT(!layout.isArray());

        replacementGroup = ObjectGroupCompartment::makeGroup(cx, &PlainObject::class_, proto);
        if (!replacementGroup)
            return false;

        PlainObject* templateObject = MakeReplacementTemplateObject(cx, replacementGroup, layout);
        if (!templateObject)
            return false;

        TypeNewScript* replacementNewScript =
            TypeNewScript::makeNativeVersion(cx, layout.newScript(), templateObject);
        if (!replacementNewScript)
            return false;

        replacementGroup->setNewScript(replacementNewScript);
        gc::TraceTypeNewScript(replacementGroup);

        group->clearNewScript(cx, replacementGroup);
    }

    // Similarly, if this group is keyed to an allocation site, replace its
    // entry with a new group that has no unboxed layout.
    if (layout.allocationScript()) {
        RootedScript script(cx, layout.allocationScript());
        jsbytecode* pc = layout.allocationPc();

        replacementGroup = ObjectGroupCompartment::makeGroup(cx, clasp, proto);
        if (!replacementGroup)
            return false;

        PlainObject* templateObject = &script->getObject(pc)->as<PlainObject>();
        replacementGroup->addDefiniteProperties(cx, templateObject->lastProperty());

        JSProtoKey key = layout.isArray() ? JSProto_Array : JSProto_Object;
        cx->compartment()->objectGroups.replaceAllocationSiteGroup(script, pc, key,
                                                                   replacementGroup);

        // Clear any baseline information at this opcode which might use the old group.
        if (script->hasBaselineScript()) {
            jit::ICEntry& entry = script->baselineScript()->icEntryFromPCOffset(script->pcToOffset(pc));
            jit::ICFallbackStub* fallback = entry.fallbackStub();
            for (jit::ICStubIterator iter = fallback->beginChain(); !iter.atEnd(); iter++)
                iter.unlink(cx);
            if (fallback->isNewObject_Fallback())
                fallback->toNewObject_Fallback()->setTemplateObject(nullptr);
            else if (fallback->isNewArray_Fallback())
                fallback->toNewArray_Fallback()->setTemplateGroup(replacementGroup);
        }
    }

    size_t nfixed = layout.isArray() ? 0 : gc::GetGCKindSlots(layout.getAllocKind());

    if (layout.isArray()) {
        // The length shape to use for arrays is cached via a modified initial
        // shape for array objects. Create an array now to make sure this entry
        // is instantiated.
        if (!NewDenseEmptyArray(cx))
            return false;
    }

    RootedShape shape(cx, EmptyShape::getInitialShape(cx, clasp, proto, nfixed, 0));
    if (!shape)
        return false;

    MOZ_ASSERT_IF(layout.isArray(), !shape->isEmptyShape() && shape->slotSpan() == 0);

    // Add shapes for each property, if this is for a plain object.
    for (size_t i = 0; i < layout.properties().length(); i++) {
        const UnboxedLayout::Property& property = layout.properties()[i];

        Rooted<StackShape> child(cx, StackShape(shape->base()->unowned(), NameToId(property.name),
                                                i, JSPROP_ENUMERATE, 0));
        shape = cx->compartment()->propertyTree.getChild(cx, shape, child);
        if (!shape)
            return false;
    }

    ObjectGroup* nativeGroup =
        ObjectGroupCompartment::makeGroup(cx, clasp, proto,
                                          group->flags() & OBJECT_FLAG_DYNAMIC_MASK);
    if (!nativeGroup)
        return false;

    // Propagate all property types from the old group to the new group.
    if (layout.isArray()) {
        if (!PropagatePropertyTypes(cx, JSID_VOID, group, nativeGroup))
            return false;
    } else {
        for (size_t i = 0; i < layout.properties().length(); i++) {
            const UnboxedLayout::Property& property = layout.properties()[i];
            jsid id = NameToId(property.name);
            if (!PropagatePropertyTypes(cx, id, group, nativeGroup))
                return false;

            // If we are OOM we may not be able to propagate properties.
            if (nativeGroup->unknownProperties())
                break;

            HeapTypeSet* nativeProperty = nativeGroup->maybeGetProperty(id);
            if (nativeProperty && nativeProperty->canSetDefinite(i))
                nativeProperty->setDefinite(i);
        }
    }

    layout.nativeGroup_ = nativeGroup;
    layout.nativeShape_ = shape;
    layout.replacementGroup_ = replacementGroup;

    nativeGroup->setOriginalUnboxedGroup(group);

    group->markStateChange(cx);

    return true;
}

/* static */ bool
UnboxedPlainObject::convertToNative(JSContext* cx, JSObject* obj)
{
    const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layout();
    UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando();

    if (!layout.nativeGroup()) {
        if (!UnboxedLayout::makeNativeGroup(cx, obj->group()))
            return false;

        // makeNativeGroup can reentrantly invoke this method.
        if (obj->is<PlainObject>())
            return true;
    }

    AutoValueVector values(cx);
    for (size_t i = 0; i < layout.properties().length(); i++) {
        // We might be reading properties off the object which have not been
        // initialized yet. Make sure any double values we read here are
        // canonicalized.
        if (!values.append(obj->as<UnboxedPlainObject>().getValue(layout.properties()[i], true)))
            return false;
    }

    // We are eliminating the expando edge with the conversion, so trigger a
    // pre barrier.
    JSObject::writeBarrierPre(expando);

    // Additionally trigger a post barrier on the expando itself. Whole cell
    // store buffer entries can be added on the original unboxed object for
    // writes to the expando (see WholeCellEdges::trace), so after conversion
    // we need to make sure the expando itself will still be traced.
    if (expando && !IsInsideNursery(expando))
        cx->runtime()->gc.storeBuffer.putWholeCell(expando);

    obj->setGroup(layout.nativeGroup());
    obj->as<PlainObject>().setLastPropertyMakeNative(cx, layout.nativeShape());

    for (size_t i = 0; i < values.length(); i++)
        obj->as<PlainObject>().initSlotUnchecked(i, values[i]);

    if (expando) {
        // Add properties from the expando object to the object, in order.
        // Suppress GC here, so that callers don't need to worry about this
        // method collecting. The stuff below can only fail due to OOM, in
        // which case the object will not have been completely filled back in.
        gc::AutoSuppressGC suppress(cx);

        Vector<jsid> ids(cx);
        for (Shape::Range<NoGC> r(expando->lastProperty()); !r.empty(); r.popFront()) {
            if (!ids.append(r.front().propid()))
                return false;
        }
        for (size_t i = 0; i < expando->getDenseInitializedLength(); i++) {
            if (!expando->getDenseElement(i).isMagic(JS_ELEMENTS_HOLE)) {
                if (!ids.append(INT_TO_JSID(i)))
                    return false;
            }
        }
        ::Reverse(ids.begin(), ids.end());

        RootedPlainObject nobj(cx, &obj->as<PlainObject>());
        Rooted<UnboxedExpandoObject*> nexpando(cx, expando);
        RootedId id(cx);
        Rooted<PropertyDescriptor> desc(cx);
        for (size_t i = 0; i < ids.length(); i++) {
            id = ids[i];
            if (!GetOwnPropertyDescriptor(cx, nexpando, id, &desc))
                return false;
            ObjectOpResult result;
            if (!DefineProperty(cx, nobj, id, desc, result))
                return false;
            MOZ_ASSERT(result.ok());
        }
    }

    return true;
}

/* static */
UnboxedPlainObject*
UnboxedPlainObject::create(ExclusiveContext* cx, HandleObjectGroup group, NewObjectKind newKind)
{
    MOZ_ASSERT(group->clasp() == &class_);
    gc::AllocKind allocKind = group->unboxedLayout().getAllocKind();

    UnboxedPlainObject* res =
        NewObjectWithGroup<UnboxedPlainObject>(cx, group, allocKind, newKind);
    if (!res)
        return nullptr;

    // Overwrite the dummy shape which was written to the object's expando field.
    res->initExpando();

    // Initialize reference fields of the object. All fields in the object will
    // be overwritten shortly, but references need to be safe for the GC.
    const int32_t* list = res->layout().traceList();
    if (list) {
        uint8_t* data = res->data();
        while (*list != -1) {
            HeapPtrString* heap = reinterpret_cast<HeapPtrString*>(data + *list);
            heap->init(cx->names().empty);
            list++;
        }
        list++;
        while (*list != -1) {
            HeapPtrObject* heap = reinterpret_cast<HeapPtrObject*>(data + *list);
            heap->init(nullptr);
            list++;
        }
        // Unboxed objects don't have Values to initialize.
        MOZ_ASSERT(*(list + 1) == -1);
    }

    return res;
}

/* static */ JSObject*
UnboxedPlainObject::createWithProperties(ExclusiveContext* cx, HandleObjectGroup group,
                                         NewObjectKind newKind, IdValuePair* properties)
{
    MOZ_ASSERT(newKind == GenericObject || newKind == TenuredObject);

    UnboxedLayout& layout = group->unboxedLayout();

    if (layout.constructorCode()) {
        MOZ_ASSERT(cx->isJSContext());

        typedef JSObject* (*ConstructorCodeSignature)(IdValuePair*, NewObjectKind);
        ConstructorCodeSignature function =
            reinterpret_cast<ConstructorCodeSignature>(layout.constructorCode()->raw());

        JSObject* obj;
        {
            JS::AutoSuppressGCAnalysis nogc;
            obj = reinterpret_cast<JSObject*>(CALL_GENERATED_2(function, properties, newKind));
        }
        if (obj > reinterpret_cast<JSObject*>(CLEAR_CONSTRUCTOR_CODE_TOKEN))
            return obj;

        if (obj == reinterpret_cast<JSObject*>(CLEAR_CONSTRUCTOR_CODE_TOKEN))
            layout.setConstructorCode(nullptr);
    }

    UnboxedPlainObject* obj = UnboxedPlainObject::create(cx, group, newKind);
    if (!obj)
        return nullptr;

    for (size_t i = 0; i < layout.properties().length(); i++) {
        if (!obj->setValue(cx, layout.properties()[i], properties[i].value))
            return NewPlainObjectWithProperties(cx, properties, layout.properties().length(), newKind);
    }

#ifndef JS_CODEGEN_NONE
    if (cx->isJSContext() &&
        !layout.constructorCode() &&
        cx->asJSContext()->runtime()->jitSupportsFloatingPoint)
    {
        if (!UnboxedLayout::makeConstructorCode(cx->asJSContext(), group))
            return nullptr;
    }
#endif

    return obj;
}

/* static */ bool
UnboxedPlainObject::obj_lookupProperty(JSContext* cx, HandleObject obj,
                                       HandleId id, MutableHandleObject objp,
                                       MutableHandleShape propp)
{
    if (obj->as<UnboxedPlainObject>().containsUnboxedOrExpandoProperty(cx, id)) {
        MarkNonNativePropertyFound<CanGC>(propp);
        objp.set(obj);
        return true;
    }

    RootedObject proto(cx, obj->getProto());
    if (!proto) {
        objp.set(nullptr);
        propp.set(nullptr);
        return true;
    }

    return LookupProperty(cx, proto, id, objp, propp);
}

/* static */ bool
UnboxedPlainObject::obj_defineProperty(JSContext* cx, HandleObject obj, HandleId id,
                                       Handle<JSPropertyDescriptor> desc,
                                       ObjectOpResult& result)
{
    const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layout();

    if (const UnboxedLayout::Property* property = layout.lookup(id)) {
        if (!desc.getter() && !desc.setter() && desc.attributes() == JSPROP_ENUMERATE) {
            // This define is equivalent to setting an existing property.
            if (obj->as<UnboxedPlainObject>().setValue(cx, *property, desc.value()))
                return result.succeed();
        }

        // Trying to incompatibly redefine an existing property requires the
        // object to be converted to a native object.
        if (!convertToNative(cx, obj))
            return false;

        return DefineProperty(cx, obj, id, desc, result);
    }

    // Define the property on the expando object.
    Rooted<UnboxedExpandoObject*> expando(cx, ensureExpando(cx, obj.as<UnboxedPlainObject>()));
    if (!expando)
        return false;

    // Update property types on the unboxed object as well.
    AddTypePropertyId(cx, obj, id, desc.value());

    return DefineProperty(cx, expando, id, desc, result);
}

/* static */ bool
UnboxedPlainObject::obj_hasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp)
{
    if (obj->as<UnboxedPlainObject>().containsUnboxedOrExpandoProperty(cx, id)) {
        *foundp = true;
        return true;
    }

    RootedObject proto(cx, obj->getProto());
    if (!proto) {
        *foundp = false;
        return true;
    }

    return HasProperty(cx, proto, id, foundp);
}

/* static */ bool
UnboxedPlainObject::obj_getProperty(JSContext* cx, HandleObject obj, HandleValue receiver,
                                    HandleId id, MutableHandleValue vp)
{
    const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layout();

    if (const UnboxedLayout::Property* property = layout.lookup(id)) {
        vp.set(obj->as<UnboxedPlainObject>().getValue(*property));
        return true;
    }

    if (UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando()) {
        if (expando->containsShapeOrElement(cx, id)) {
            RootedObject nexpando(cx, expando);
            return GetProperty(cx, nexpando, receiver, id, vp);
        }
    }

    RootedObject proto(cx, obj->getProto());
    if (!proto) {
        vp.setUndefined();
        return true;
    }

    return GetProperty(cx, proto, receiver, id, vp);
}

/* static */ bool
UnboxedPlainObject::obj_setProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                                    HandleValue receiver, ObjectOpResult& result)
{
    const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layout();

    if (const UnboxedLayout::Property* property = layout.lookup(id)) {
        if (receiver.isObject() && obj == &receiver.toObject()) {
            if (obj->as<UnboxedPlainObject>().setValue(cx, *property, v))
                return result.succeed();

            if (!convertToNative(cx, obj))
                return false;
            return SetProperty(cx, obj, id, v, receiver, result);
        }

        return SetPropertyByDefining(cx, id, v, receiver, result);
    }

    if (UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando()) {
        if (expando->containsShapeOrElement(cx, id)) {
            // Update property types on the unboxed object as well.
            AddTypePropertyId(cx, obj, id, v);

            RootedObject nexpando(cx, expando);
            return SetProperty(cx, nexpando, id, v, receiver, result);
        }
    }

    return SetPropertyOnProto(cx, obj, id, v, receiver, result);
}

/* static */ bool
UnboxedPlainObject::obj_getOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                                 MutableHandle<JSPropertyDescriptor> desc)
{
    const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layout();

    if (const UnboxedLayout::Property* property = layout.lookup(id)) {
        desc.value().set(obj->as<UnboxedPlainObject>().getValue(*property));
        desc.setAttributes(JSPROP_ENUMERATE);
        desc.object().set(obj);
        return true;
    }

    if (UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando()) {
        if (expando->containsShapeOrElement(cx, id)) {
            RootedObject nexpando(cx, expando);
            if (!GetOwnPropertyDescriptor(cx, nexpando, id, desc))
                return false;
            if (desc.object() == nexpando)
                desc.object().set(obj);
            return true;
        }
    }

    desc.object().set(nullptr);
    return true;
}

/* static */ bool
UnboxedPlainObject::obj_deleteProperty(JSContext* cx, HandleObject obj, HandleId id,
                                       ObjectOpResult& result)
{
    if (!convertToNative(cx, obj))
        return false;
    return DeleteProperty(cx, obj, id, result);
}

/* static */ bool
UnboxedPlainObject::obj_watch(JSContext* cx, HandleObject obj, HandleId id, HandleObject callable)
{
    if (!convertToNative(cx, obj))
        return false;
    return WatchProperty(cx, obj, id, callable);
}

/* static */ bool
UnboxedPlainObject::obj_enumerate(JSContext* cx, HandleObject obj, AutoIdVector& properties,
                                  bool enumerableOnly)
{
    // Ignore expando properties here, they are special-cased by the property
    // enumeration code.

    const UnboxedLayout::PropertyVector& unboxed = obj->as<UnboxedPlainObject>().layout().properties();
    for (size_t i = 0; i < unboxed.length(); i++) {
        if (!properties.append(NameToId(unboxed[i].name)))
            return false;
    }

    return true;
}

const Class UnboxedExpandoObject::class_ = {
    "UnboxedExpandoObject",
    0
};

const Class UnboxedPlainObject::class_ = {
    js_Object_str,
    Class::NON_NATIVE |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Object),
    nullptr,        /* addProperty */
    nullptr,        /* delProperty */
    nullptr,        /* getProperty */
    nullptr,        /* setProperty */
    nullptr,        /* enumerate   */
    nullptr,        /* resolve     */
    nullptr,        /* mayResolve  */
    nullptr,        /* finalize    */
    nullptr,        /* call        */
    nullptr,        /* hasInstance */
    nullptr,        /* construct   */
    UnboxedPlainObject::trace,
    JS_NULL_CLASS_SPEC,
    JS_NULL_CLASS_EXT,
    {
        UnboxedPlainObject::obj_lookupProperty,
        UnboxedPlainObject::obj_defineProperty,
        UnboxedPlainObject::obj_hasProperty,
        UnboxedPlainObject::obj_getProperty,
        UnboxedPlainObject::obj_setProperty,
        UnboxedPlainObject::obj_getOwnPropertyDescriptor,
        UnboxedPlainObject::obj_deleteProperty,
        UnboxedPlainObject::obj_watch,
        nullptr,   /* No unwatch needed, as watch() converts the object to native */
        nullptr,   /* getElements */
        UnboxedPlainObject::obj_enumerate,
    }
};

/////////////////////////////////////////////////////////////////////
// UnboxedArrayObject
/////////////////////////////////////////////////////////////////////

template <JSValueType Type>
DenseElementResult
AppendUnboxedDenseElements(UnboxedArrayObject* obj, uint32_t initlen, AutoValueVector* values)
{
    for (size_t i = 0; i < initlen; i++)
        values->infallibleAppend(obj->getElementSpecific<Type>(i));
    return DenseElementResult::Success;
}

DefineBoxedOrUnboxedFunctor3(AppendUnboxedDenseElements,
                             UnboxedArrayObject*, uint32_t, AutoValueVector*);

/* static */ bool
UnboxedArrayObject::convertToNativeWithGroup(ExclusiveContext* cx, JSObject* obj,
                                             ObjectGroup* group, Shape* shape)
{
    size_t length = obj->as<UnboxedArrayObject>().length();
    size_t initlen = obj->as<UnboxedArrayObject>().initializedLength();

    AutoValueVector values(cx);
    if (!values.reserve(initlen))
        return false;

    AppendUnboxedDenseElementsFunctor functor(&obj->as<UnboxedArrayObject>(), initlen, &values);
    DebugOnly<DenseElementResult> result = CallBoxedOrUnboxedSpecialization(functor, obj);
    MOZ_ASSERT(result.value == DenseElementResult::Success);

    obj->setGroup(group);

    ArrayObject* aobj = &obj->as<ArrayObject>();
    aobj->setLastPropertyMakeNative(cx, shape);

    // Make sure there is at least one element, so that this array does not
    // use emptyObjectElements / emptyObjectElementsShared.
    if (!aobj->ensureElements(cx, Max<size_t>(initlen, 1)))
        return false;

    MOZ_ASSERT(!aobj->getDenseInitializedLength());
    aobj->setDenseInitializedLength(initlen);
    aobj->initDenseElements(0, values.begin(), initlen);
    aobj->setLengthInt32(length);

    return true;
}

/* static */ bool
UnboxedArrayObject::convertToNative(JSContext* cx, JSObject* obj)
{
    const UnboxedLayout& layout = obj->as<UnboxedArrayObject>().layout();

    if (!layout.nativeGroup()) {
        if (!UnboxedLayout::makeNativeGroup(cx, obj->group()))
            return false;
    }

    return convertToNativeWithGroup(cx, obj, layout.nativeGroup(), layout.nativeShape());
}

bool
UnboxedArrayObject::convertInt32ToDouble(ExclusiveContext* cx, ObjectGroup* group)
{
    MOZ_ASSERT(elementType() == JSVAL_TYPE_INT32);
    MOZ_ASSERT(group->unboxedLayout().elementType() == JSVAL_TYPE_DOUBLE);

    Vector<int32_t> values(cx);
    if (!values.reserve(initializedLength()))
        return false;
    for (size_t i = 0; i < initializedLength(); i++)
        values.infallibleAppend(getElementSpecific<JSVAL_TYPE_INT32>(i).toInt32());

    uint8_t* newElements;
    if (hasInlineElements()) {
        newElements = AllocateObjectBuffer<uint8_t>(cx, this, capacity() * sizeof(double));
    } else {
        newElements = ReallocateObjectBuffer<uint8_t>(cx, this, elements(),
                                                      capacity() * sizeof(int32_t),
                                                      capacity() * sizeof(double));
    }
    if (!newElements)
        return false;

    setGroup(group);
    elements_ = newElements;

    for (size_t i = 0; i < initializedLength(); i++)
        setElementNoTypeChangeSpecific<JSVAL_TYPE_DOUBLE>(i, DoubleValue(values[i]));

    return true;
}

/* static */ UnboxedArrayObject*
UnboxedArrayObject::create(ExclusiveContext* cx, HandleObjectGroup group, uint32_t length,
                           NewObjectKind newKind, uint32_t maxLength)
{
    MOZ_ASSERT(length <= MaximumCapacity);

    MOZ_ASSERT(group->clasp() == &class_);
    uint32_t elementSize = UnboxedTypeSize(group->unboxedLayout().elementType());
    uint32_t capacity = Min(length, maxLength);
    uint32_t nbytes = offsetOfInlineElements() + elementSize * capacity;

    UnboxedArrayObject* res;
    if (nbytes <= JSObject::MAX_BYTE_SIZE) {
        gc::AllocKind allocKind = gc::GetGCObjectKindForBytes(nbytes);

        // If there was no provided length information, pick an allocation kind
        // to accommodate small arrays (as is done for normal native arrays).
        if (capacity == 0)
            allocKind = gc::AllocKind::OBJECT8;

        res = NewObjectWithGroup<UnboxedArrayObject>(cx, group, allocKind, newKind);
        if (!res)
            return nullptr;
        res->setInitializedLengthNoBarrier(0);
        res->setInlineElements();

        size_t actualCapacity = (GetGCKindBytes(allocKind) - offsetOfInlineElements()) / elementSize;
        MOZ_ASSERT(actualCapacity >= capacity);
        res->setCapacityIndex(exactCapacityIndex(actualCapacity));
    } else {
        res = NewObjectWithGroup<UnboxedArrayObject>(cx, group, gc::AllocKind::OBJECT0, newKind);
        if (!res)
            return nullptr;
        res->setInitializedLengthNoBarrier(0);

        uint32_t capacityIndex = (capacity == length)
                                 ? CapacityMatchesLengthIndex
                                 : chooseCapacityIndex(capacity, length);
        uint32_t actualCapacity = computeCapacity(capacityIndex, length);

        res->elements_ = AllocateObjectBuffer<uint8_t>(cx, res, actualCapacity * elementSize);
        if (!res->elements_) {
            // Make the object safe for GC.
            res->setInlineElements();
            return nullptr;
        }

        res->setCapacityIndex(capacityIndex);
    }

    res->setLength(cx, length);
    return res;
}

bool
UnboxedArrayObject::setElement(ExclusiveContext* cx, size_t index, const Value& v)
{
    MOZ_ASSERT(index < initializedLength());
    uint8_t* p = elements() + index * elementSize();
    return SetUnboxedValue(cx, this, JSID_VOID, p, elementType(), v, /* preBarrier = */ true);
}

bool
UnboxedArrayObject::initElement(ExclusiveContext* cx, size_t index, const Value& v)
{
    MOZ_ASSERT(index < initializedLength());
    uint8_t* p = elements() + index * elementSize();
    return SetUnboxedValue(cx, this, JSID_VOID, p, elementType(), v, /* preBarrier = */ false);
}

void
UnboxedArrayObject::initElementNoTypeChange(size_t index, const Value& v)
{
    MOZ_ASSERT(index < initializedLength());
    uint8_t* p = elements() + index * elementSize();
    if (UnboxedTypeNeedsPreBarrier(elementType()))
        *reinterpret_cast<void**>(p) = nullptr;
    SetUnboxedValueNoTypeChange(this, p, elementType(), v, /* preBarrier = */ false);
}

Value
UnboxedArrayObject::getElement(size_t index)
{
    MOZ_ASSERT(index < initializedLength());
    uint8_t* p = elements() + index * elementSize();
    return GetUnboxedValue(p, elementType(), /* maybeUninitialized = */ false);
}

/* static */ void
UnboxedArrayObject::trace(JSTracer* trc, JSObject* obj)
{
    JSValueType type = obj->as<UnboxedArrayObject>().elementType();
    if (!UnboxedTypeNeedsPreBarrier(type))
        return;

    MOZ_ASSERT(obj->as<UnboxedArrayObject>().elementSize() == sizeof(uintptr_t));
    size_t initlen = obj->as<UnboxedArrayObject>().initializedLength();
    void** elements = reinterpret_cast<void**>(obj->as<UnboxedArrayObject>().elements());

    switch (type) {
      case JSVAL_TYPE_OBJECT:
        for (size_t i = 0; i < initlen; i++) {
            HeapPtrObject* heap = reinterpret_cast<HeapPtrObject*>(elements + i);
            if (*heap)
                TraceEdge(trc, heap, "unboxed_object");
        }
        break;

      case JSVAL_TYPE_STRING:
        for (size_t i = 0; i < initlen; i++) {
            HeapPtrString* heap = reinterpret_cast<HeapPtrString*>(elements + i);
            TraceEdge(trc, heap, "unboxed_string");
        }
        break;

      default:
        MOZ_CRASH();
    }
}

/* static */ void
UnboxedArrayObject::objectMoved(JSObject* obj, const JSObject* old)
{
    UnboxedArrayObject& dst = obj->as<UnboxedArrayObject>();
    const UnboxedArrayObject& src = old->as<UnboxedArrayObject>();

    // Fix up possible inline data pointer.
    if (src.hasInlineElements())
        dst.setInlineElements();
}

/* static */ void
UnboxedArrayObject::finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(!IsInsideNursery(obj));
    if (!obj->as<UnboxedArrayObject>().hasInlineElements())
        js_free(obj->as<UnboxedArrayObject>().elements());
}

/* static */ size_t
UnboxedArrayObject::objectMovedDuringMinorGC(JSTracer* trc, JSObject* dst, JSObject* src,
                                             gc::AllocKind allocKind)
{
    UnboxedArrayObject* ndst = &dst->as<UnboxedArrayObject>();
    UnboxedArrayObject* nsrc = &src->as<UnboxedArrayObject>();
    MOZ_ASSERT(ndst->elements() == nsrc->elements());

    Nursery& nursery = trc->runtime()->gc.nursery;

    if (!nursery.isInside(nsrc->elements())) {
        nursery.removeMallocedBuffer(nsrc->elements());
        return 0;
    }

    // Determine if we can use inline data for the target array. If this is
    // possible, the nursery will have picked an allocation size that is large
    // enough.
    size_t nbytes = nsrc->capacity() * nsrc->elementSize();
    if (offsetOfInlineElements() + nbytes <= GetGCKindBytes(allocKind)) {
        ndst->setInlineElements();
    } else {
        MOZ_ASSERT(allocKind == gc::AllocKind::OBJECT0);

        AutoEnterOOMUnsafeRegion oomUnsafe;
        uint8_t* data = nsrc->zone()->pod_malloc<uint8_t>(nbytes);
        if (!data)
            oomUnsafe.crash("Failed to allocate unboxed array elements while tenuring.");
        ndst->elements_ = data;
    }

    PodCopy(ndst->elements(), nsrc->elements(), nsrc->initializedLength() * nsrc->elementSize());

    // Set a forwarding pointer for the element buffers in case they were
    // preserved on the stack by Ion.
    bool direct = nsrc->capacity() * nsrc->elementSize() >= sizeof(uintptr_t);
    nursery.maybeSetForwardingPointer(trc, nsrc->elements(), ndst->elements(), direct);

    return ndst->hasInlineElements() ? 0 : nbytes;
}

// Possible capacities for unboxed arrays. Some of these capacities might seem
// a little weird, but were chosen to allow the inline data of objects of each
// size to be fully utilized for arrays of the various types on both 32 bit and
// 64 bit platforms.
//
// To find the possible inline capacities, the following script was used:
//
// var fixedSlotCapacities = [0, 2, 4, 8, 12, 16];
// var dataSizes = [1, 4, 8];
// var header32 = 4 * 2 + 4 * 2;
// var header64 = 8 * 2 + 4 * 2;
//
// for (var i = 0; i < fixedSlotCapacities.length; i++) {
//    var nfixed = fixedSlotCapacities[i];
//    var size32 = 4 * 4 + 8 * nfixed - header32;
//    var size64 = 8 * 4 + 8 * nfixed - header64;
//    for (var j = 0; j < dataSizes.length; j++) {
//        print(size32 / dataSizes[j]);
//        print(size64 / dataSizes[j]);
//    }
// }
//
/* static */ const uint32_t
UnboxedArrayObject::CapacityArray[] = {
    UINT32_MAX, // For CapacityMatchesLengthIndex.
    0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 13, 16, 17, 18, 24, 26, 32, 34, 40, 64, 72, 96, 104, 128, 136,
    256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288,
    1048576, 2097152, 3145728, 4194304, 5242880, 6291456, 7340032, 8388608, 9437184, 11534336,
    13631488, 15728640, 17825792, 20971520, 24117248, 27262976, 31457280, 35651584, 40894464,
    46137344, 52428800, 59768832, MaximumCapacity
};

static const uint32_t
Pow2CapacityIndexes[] = {
    2,  // 1
    3,  // 2
    5,  // 4
    8,  // 8
    13, // 16
    18, // 32
    21, // 64
    25, // 128
    27, // 256
    28, // 512
    29, // 1024
    30, // 2048
    31, // 4096
    32, // 8192
    33, // 16384
    34, // 32768
    35, // 65536
    36, // 131072
    37, // 262144
    38, // 524288
    39  // 1048576
};

static const uint32_t MebiCapacityIndex = 39;

/* static */ uint32_t
UnboxedArrayObject::chooseCapacityIndex(uint32_t capacity, uint32_t length)
{
    // Note: the structure and behavior of this method follow along with
    // NativeObject::goodAllocated. Changes to the allocation strategy in one
    // should generally be matched by the other.

    // Make sure we have enough space to store all possible values for the capacity index.
    // This ought to be a static_assert, but MSVC doesn't like that.
    MOZ_ASSERT(mozilla::ArrayLength(CapacityArray) - 1 <= (CapacityMask >> CapacityShift));

    // The caller should have ensured the capacity is possible for an unboxed array.
    MOZ_ASSERT(capacity <= MaximumCapacity);

    static const uint32_t Mebi = 1024 * 1024;

    if (capacity <= Mebi) {
        capacity = mozilla::RoundUpPow2(capacity);

        // When the required capacity is close to the array length, then round
        // up to the array length itself, as for NativeObject.
        if (length >= capacity && capacity > (length / 3) * 2)
            return CapacityMatchesLengthIndex;

        if (capacity < MinimumDynamicCapacity)
            capacity = MinimumDynamicCapacity;

        uint32_t bit = mozilla::FloorLog2Size(capacity);
        MOZ_ASSERT(capacity == uint32_t(1 << bit));
        MOZ_ASSERT(bit <= 20);
        MOZ_ASSERT(mozilla::ArrayLength(Pow2CapacityIndexes) == 21);

        uint32_t index = Pow2CapacityIndexes[bit];
        MOZ_ASSERT(CapacityArray[index] == capacity);

        return index;
    }

    MOZ_ASSERT(CapacityArray[MebiCapacityIndex] == Mebi);

    for (uint32_t i = MebiCapacityIndex + 1;; i++) {
        if (CapacityArray[i] >= capacity)
            return i;
    }

    MOZ_CRASH("Invalid capacity");
}

/* static */ uint32_t
UnboxedArrayObject::exactCapacityIndex(uint32_t capacity)
{
    for (size_t i = CapacityMatchesLengthIndex + 1; i < ArrayLength(CapacityArray); i++) {
        if (CapacityArray[i] == capacity)
            return i;
    }
    MOZ_CRASH();
}

bool
UnboxedArrayObject::growElements(ExclusiveContext* cx, size_t cap)
{
    // The caller should have checked if this capacity is possible for an
    // unboxed array, so the only way this call can fail is from OOM.
    MOZ_ASSERT(cap <= MaximumCapacity);

    uint32_t oldCapacity = capacity();
    uint32_t newCapacityIndex = chooseCapacityIndex(cap, length());
    uint32_t newCapacity = computeCapacity(newCapacityIndex, length());

    MOZ_ASSERT(oldCapacity < cap);
    MOZ_ASSERT(cap <= newCapacity);

    // The allocation size computation below cannot have integer overflows.
    JS_STATIC_ASSERT(MaximumCapacity < UINT32_MAX / sizeof(double));

    uint8_t* newElements;
    if (hasInlineElements()) {
        newElements = AllocateObjectBuffer<uint8_t>(cx, this, newCapacity * elementSize());
        if (!newElements)
            return false;
        js_memcpy(newElements, elements(), initializedLength() * elementSize());
    } else {
        newElements = ReallocateObjectBuffer<uint8_t>(cx, this, elements(),
                                                      oldCapacity * elementSize(),
                                                      newCapacity * elementSize());
        if (!newElements)
            return false;
    }

    elements_ = newElements;
    setCapacityIndex(newCapacityIndex);

    return true;
}

void
UnboxedArrayObject::shrinkElements(ExclusiveContext* cx, size_t cap)
{
    if (hasInlineElements())
        return;

    uint32_t oldCapacity = capacity();
    uint32_t newCapacityIndex = chooseCapacityIndex(cap, 0);
    uint32_t newCapacity = computeCapacity(newCapacityIndex, 0);

    MOZ_ASSERT(cap < oldCapacity);
    MOZ_ASSERT(cap <= newCapacity);

    if (newCapacity >= oldCapacity)
        return;

    uint8_t* newElements = ReallocateObjectBuffer<uint8_t>(cx, this, elements(),
                                                           oldCapacity * elementSize(),
                                                           newCapacity * elementSize());
    if (!newElements)
        return;

    elements_ = newElements;
    setCapacityIndex(newCapacityIndex);
}

bool
UnboxedArrayObject::containsProperty(ExclusiveContext* cx, jsid id)
{
    if (JSID_IS_INT(id) && uint32_t(JSID_TO_INT(id)) < initializedLength())
        return true;
    if (JSID_IS_ATOM(id) && JSID_TO_ATOM(id) == cx->names().length)
        return true;
    return false;
}

/* static */ bool
UnboxedArrayObject::obj_lookupProperty(JSContext* cx, HandleObject obj,
                                       HandleId id, MutableHandleObject objp,
                                       MutableHandleShape propp)
{
    if (obj->as<UnboxedArrayObject>().containsProperty(cx, id)) {
        MarkNonNativePropertyFound<CanGC>(propp);
        objp.set(obj);
        return true;
    }

    RootedObject proto(cx, obj->getProto());
    if (!proto) {
        objp.set(nullptr);
        propp.set(nullptr);
        return true;
    }

    return LookupProperty(cx, proto, id, objp, propp);
}

/* static */ bool
UnboxedArrayObject::obj_defineProperty(JSContext* cx, HandleObject obj, HandleId id,
                                       Handle<JSPropertyDescriptor> desc,
                                       ObjectOpResult& result)
{
    if (JSID_IS_INT(id) && !desc.getter() && !desc.setter() && desc.attributes() == JSPROP_ENUMERATE) {
        UnboxedArrayObject* nobj = &obj->as<UnboxedArrayObject>();

        uint32_t index = JSID_TO_INT(id);
        if (index < nobj->initializedLength()) {
            if (nobj->setElement(cx, index, desc.value()))
                return result.succeed();
        } else if (index == nobj->initializedLength() && index < MaximumCapacity) {
            if (nobj->initializedLength() == nobj->capacity()) {
                if (!nobj->growElements(cx, index + 1))
                    return false;
            }
            nobj->setInitializedLength(index + 1);
            if (nobj->initElement(cx, index, desc.value())) {
                if (nobj->length() <= index)
                    nobj->setLengthInt32(index + 1);
                return result.succeed();
            }
            nobj->setInitializedLengthNoBarrier(index);
        }
    }

    if (!convertToNative(cx, obj))
        return false;

    return DefineProperty(cx, obj, id, desc, result);
}

/* static */ bool
UnboxedArrayObject::obj_hasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp)
{
    if (obj->as<UnboxedArrayObject>().containsProperty(cx, id)) {
        *foundp = true;
        return true;
    }

    RootedObject proto(cx, obj->getProto());
    if (!proto) {
        *foundp = false;
        return true;
    }

    return HasProperty(cx, proto, id, foundp);
}

/* static */ bool
UnboxedArrayObject::obj_getProperty(JSContext* cx, HandleObject obj, HandleValue receiver,
                                    HandleId id, MutableHandleValue vp)
{
    if (obj->as<UnboxedArrayObject>().containsProperty(cx, id)) {
        if (JSID_IS_INT(id))
            vp.set(obj->as<UnboxedArrayObject>().getElement(JSID_TO_INT(id)));
        else
            vp.set(Int32Value(obj->as<UnboxedArrayObject>().length()));
        return true;
    }

    RootedObject proto(cx, obj->getProto());
    if (!proto) {
        vp.setUndefined();
        return true;
    }

    return GetProperty(cx, proto, receiver, id, vp);
}

/* static */ bool
UnboxedArrayObject::obj_setProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                                    HandleValue receiver, ObjectOpResult& result)
{
    if (obj->as<UnboxedArrayObject>().containsProperty(cx, id)) {
        if (receiver.isObject() && obj == &receiver.toObject()) {
            if (JSID_IS_INT(id)) {
                if (obj->as<UnboxedArrayObject>().setElement(cx, JSID_TO_INT(id), v))
                    return result.succeed();
            } else {
                uint32_t len;
                if (!CanonicalizeArrayLengthValue(cx, v, &len))
                    return false;
                UnboxedArrayObject* nobj = &obj->as<UnboxedArrayObject>();
                if (len < nobj->initializedLength()) {
                    nobj->setInitializedLength(len);
                    nobj->shrinkElements(cx, len);
                }
                nobj->setLength(cx, len);
                return result.succeed();
            }

            if (!convertToNative(cx, obj))
                return false;
            return SetProperty(cx, obj, id, v, receiver, result);
        }

        return SetPropertyByDefining(cx, id, v, receiver, result);
    }

    return SetPropertyOnProto(cx, obj, id, v, receiver, result);
}

/* static */ bool
UnboxedArrayObject::obj_getOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                                 MutableHandle<JSPropertyDescriptor> desc)
{
    if (obj->as<UnboxedArrayObject>().containsProperty(cx, id)) {
        if (JSID_IS_INT(id)) {
            desc.value().set(obj->as<UnboxedArrayObject>().getElement(JSID_TO_INT(id)));
            desc.setAttributes(JSPROP_ENUMERATE);
        } else {
            desc.value().set(Int32Value(obj->as<UnboxedArrayObject>().length()));
            desc.setAttributes(JSPROP_PERMANENT);
        }
        desc.object().set(obj);
        return true;
    }

    desc.object().set(nullptr);
    return true;
}

/* static */ bool
UnboxedArrayObject::obj_deleteProperty(JSContext* cx, HandleObject obj, HandleId id,
                                       ObjectOpResult& result)
{
    if (obj->as<UnboxedArrayObject>().containsProperty(cx, id)) {
        size_t initlen = obj->as<UnboxedArrayObject>().initializedLength();
        if (JSID_IS_INT(id) && JSID_TO_INT(id) == int32_t(initlen - 1)) {
            obj->as<UnboxedArrayObject>().setInitializedLength(initlen - 1);
            obj->as<UnboxedArrayObject>().shrinkElements(cx, initlen - 1);
            return result.succeed();
        }
    }

    if (!convertToNative(cx, obj))
        return false;
    return DeleteProperty(cx, obj, id, result);
}

/* static */ bool
UnboxedArrayObject::obj_watch(JSContext* cx, HandleObject obj, HandleId id, HandleObject callable)
{
    if (!convertToNative(cx, obj))
        return false;
    return WatchProperty(cx, obj, id, callable);
}

/* static */ bool
UnboxedArrayObject::obj_enumerate(JSContext* cx, HandleObject obj, AutoIdVector& properties,
                                  bool enumerableOnly)
{
    for (size_t i = 0; i < obj->as<UnboxedArrayObject>().initializedLength(); i++) {
        if (!properties.append(INT_TO_JSID(i)))
            return false;
    }

    if (!enumerableOnly && !properties.append(NameToId(cx->names().length)))
        return false;

    return true;
}

const Class UnboxedArrayObject::class_ = {
    "Array",
    Class::NON_NATIVE |
    JSCLASS_SKIP_NURSERY_FINALIZE |
    JSCLASS_BACKGROUND_FINALIZE,
    nullptr,        /* addProperty */
    nullptr,        /* delProperty */
    nullptr,        /* getProperty */
    nullptr,        /* setProperty */
    nullptr,        /* enumerate   */
    nullptr,        /* resolve     */
    nullptr,        /* mayResolve  */
    UnboxedArrayObject::finalize,
    nullptr,        /* call        */
    nullptr,        /* hasInstance */
    nullptr,        /* construct   */
    UnboxedArrayObject::trace,
    JS_NULL_CLASS_SPEC,
    {
        false,      /* isWrappedNative */
        nullptr,    /* weakmapKeyDelegateOp */
        UnboxedArrayObject::objectMoved
    },
    {
        UnboxedArrayObject::obj_lookupProperty,
        UnboxedArrayObject::obj_defineProperty,
        UnboxedArrayObject::obj_hasProperty,
        UnboxedArrayObject::obj_getProperty,
        UnboxedArrayObject::obj_setProperty,
        UnboxedArrayObject::obj_getOwnPropertyDescriptor,
        UnboxedArrayObject::obj_deleteProperty,
        UnboxedArrayObject::obj_watch,
        nullptr,   /* No unwatch needed, as watch() converts the object to native */
        nullptr,   /* getElements */
        UnboxedArrayObject::obj_enumerate,
    }
};

/////////////////////////////////////////////////////////////////////
// API
/////////////////////////////////////////////////////////////////////

static bool
UnboxedTypeIncludes(JSValueType supertype, JSValueType subtype)
{
    if (supertype == JSVAL_TYPE_DOUBLE && subtype == JSVAL_TYPE_INT32)
        return true;
    if (supertype == JSVAL_TYPE_OBJECT && subtype == JSVAL_TYPE_NULL)
        return true;
    return false;
}

static bool
CombineUnboxedTypes(const Value& value, JSValueType* existing)
{
    JSValueType type = value.isDouble() ? JSVAL_TYPE_DOUBLE : value.extractNonDoubleType();

    if (*existing == JSVAL_TYPE_MAGIC || *existing == type || UnboxedTypeIncludes(type, *existing)) {
        *existing = type;
        return true;
    }
    if (UnboxedTypeIncludes(*existing, type))
        return true;
    return false;
}

// Return whether the property names and types in layout are a subset of the
// specified vector.
static bool
PropertiesAreSuperset(const UnboxedLayout::PropertyVector& properties, UnboxedLayout* layout)
{
    for (size_t i = 0; i < layout->properties().length(); i++) {
        const UnboxedLayout::Property& layoutProperty = layout->properties()[i];
        bool found = false;
        for (size_t j = 0; j < properties.length(); j++) {
            if (layoutProperty.name == properties[j].name) {
                found = (layoutProperty.type == properties[j].type);
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static bool
CombinePlainObjectProperties(PlainObject* obj, Shape* templateShape,
                             UnboxedLayout::PropertyVector& properties)
{
    // All preliminary objects must have been created with enough space to
    // fill in their unboxed data inline. This is ensured either by using
    // the largest allocation kind (which limits the maximum size of an
    // unboxed object), or by using an allocation kind that covers all
    // properties in the template, as the space used by unboxed properties
    // is less than or equal to that used by boxed properties.
    MOZ_ASSERT(gc::GetGCKindSlots(obj->asTenured().getAllocKind()) >=
               Min(NativeObject::MAX_FIXED_SLOTS, templateShape->slotSpan()));

    if (obj->lastProperty() != templateShape || obj->hasDynamicElements()) {
        // Only use an unboxed representation if all created objects match
        // the template shape exactly.
        return false;
    }

    for (size_t i = 0; i < templateShape->slotSpan(); i++) {
        Value val = obj->getSlot(i);

        JSValueType& existing = properties[i].type;
        if (!CombineUnboxedTypes(val, &existing))
            return false;
    }

    return true;
}

static bool
CombineArrayObjectElements(ExclusiveContext* cx, ArrayObject* obj, JSValueType* elementType)
{
    if (obj->inDictionaryMode() ||
        obj->lastProperty()->propid() != AtomToId(cx->names().length) ||
        !obj->lastProperty()->previous()->isEmptyShape())
    {
        // Only use an unboxed representation if the object has no properties.
        return false;
    }

    for (size_t i = 0; i < obj->getDenseInitializedLength(); i++) {
        Value val = obj->getDenseElement(i);

        // For now, unboxed arrays cannot have holes.
        if (val.isMagic(JS_ELEMENTS_HOLE))
            return false;

        if (!CombineUnboxedTypes(val, elementType))
            return false;
    }

    return true;
}

static size_t
ComputePlainObjectLayout(ExclusiveContext* cx, Shape* templateShape,
                         UnboxedLayout::PropertyVector& properties)
{
    // Fill in the names for all the object's properties.
    for (Shape::Range<NoGC> r(templateShape); !r.empty(); r.popFront()) {
        size_t slot = r.front().slot();
        MOZ_ASSERT(!properties[slot].name);
        properties[slot].name = JSID_TO_ATOM(r.front().propid())->asPropertyName();
    }

    // Fill in all the unboxed object's property offsets.
    uint32_t offset = 0;

    // Search for an existing unboxed layout which is a subset of this one.
    // If there are multiple such layouts, use the largest one. If we're able
    // to find such a layout, use the same property offsets for the shared
    // properties, which will allow us to generate better code if the objects
    // have a subtype/supertype relation and are accessed at common sites.
    UnboxedLayout* bestExisting = nullptr;
    for (UnboxedLayout* existing : cx->compartment()->unboxedLayouts) {
        if (PropertiesAreSuperset(properties, existing)) {
            if (!bestExisting ||
                existing->properties().length() > bestExisting->properties().length())
            {
                bestExisting = existing;
            }
        }
    }
    if (bestExisting) {
        for (size_t i = 0; i < bestExisting->properties().length(); i++) {
            const UnboxedLayout::Property& existingProperty = bestExisting->properties()[i];
            for (size_t j = 0; j < templateShape->slotSpan(); j++) {
                if (existingProperty.name == properties[j].name) {
                    MOZ_ASSERT(existingProperty.type == properties[j].type);
                    properties[j].offset = existingProperty.offset;
                }
            }
        }
        offset = bestExisting->size();
    }

    // Order remaining properties from the largest down for the best space
    // utilization.
    static const size_t typeSizes[] = { 8, 4, 1 };

    for (size_t i = 0; i < ArrayLength(typeSizes); i++) {
        size_t size = typeSizes[i];
        for (size_t j = 0; j < templateShape->slotSpan(); j++) {
            if (properties[j].offset != UINT32_MAX)
                continue;
            JSValueType type = properties[j].type;
            if (UnboxedTypeSize(type) == size) {
                offset = JS_ROUNDUP(offset, size);
                properties[j].offset = offset;
                offset += size;
            }
        }
    }

    // The final offset is the amount of data needed by the object.
    return offset;
}

static bool
SetLayoutTraceList(ExclusiveContext* cx, UnboxedLayout* layout)
{
    // Figure out the offsets of any objects or string properties.
    Vector<int32_t, 8, SystemAllocPolicy> objectOffsets, stringOffsets;
    for (size_t i = 0; i < layout->properties().length(); i++) {
        const UnboxedLayout::Property& property = layout->properties()[i];
        MOZ_ASSERT(property.offset != UINT32_MAX);
        if (property.type == JSVAL_TYPE_OBJECT) {
            if (!objectOffsets.append(property.offset))
                return false;
        } else if (property.type == JSVAL_TYPE_STRING) {
            if (!stringOffsets.append(property.offset))
                return false;
        }
    }

    // Construct the layout's trace list.
    if (!objectOffsets.empty() || !stringOffsets.empty()) {
        Vector<int32_t, 8, SystemAllocPolicy> entries;
        if (!entries.appendAll(stringOffsets) ||
            !entries.append(-1) ||
            !entries.appendAll(objectOffsets) ||
            !entries.append(-1) ||
            !entries.append(-1))
        {
            return false;
        }
        int32_t* traceList = cx->zone()->pod_malloc<int32_t>(entries.length());
        if (!traceList)
            return false;
        PodCopy(traceList, entries.begin(), entries.length());
        layout->setTraceList(traceList);
    }

    return true;
}

static inline Value
NextValue(const AutoValueVector& values, size_t* valueCursor)
{
    return values[(*valueCursor)++];
}

static bool
GetValuesFromPreliminaryArrayObject(ArrayObject* obj, AutoValueVector& values)
{
    if (!values.append(Int32Value(obj->length())))
        return false;
    if (!values.append(Int32Value(obj->getDenseInitializedLength())))
        return false;
    for (size_t i = 0; i < obj->getDenseInitializedLength(); i++) {
        if (!values.append(obj->getDenseElement(i)))
            return false;
    }
    return true;
}

void
UnboxedArrayObject::fillAfterConvert(ExclusiveContext* cx,
                                     const AutoValueVector& values, size_t* valueCursor)
{
    MOZ_ASSERT(CapacityArray[1] == 0);
    setCapacityIndex(1);
    setInitializedLengthNoBarrier(0);
    setInlineElements();

    setLength(cx, NextValue(values, valueCursor).toInt32());

    int32_t initlen = NextValue(values, valueCursor).toInt32();
    if (!initlen)
        return;

    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!growElements(cx, initlen))
        oomUnsafe.crash("UnboxedArrayObject::fillAfterConvert");

    setInitializedLength(initlen);

    for (size_t i = 0; i < size_t(initlen); i++)
        JS_ALWAYS_TRUE(initElement(cx, i, NextValue(values, valueCursor)));
}

static bool
GetValuesFromPreliminaryPlainObject(PlainObject* obj, AutoValueVector& values)
{
    for (size_t i = 0; i < obj->slotSpan(); i++) {
        if (!values.append(obj->getSlot(i)))
            return false;
    }
    return true;
}

void
UnboxedPlainObject::fillAfterConvert(ExclusiveContext* cx,
                                     const AutoValueVector& values, size_t* valueCursor)
{
    initExpando();
    memset(data(), 0, layout().size());
    for (size_t i = 0; i < layout().properties().length(); i++)
        JS_ALWAYS_TRUE(setValue(cx, layout().properties()[i], NextValue(values, valueCursor)));
}

bool
js::TryConvertToUnboxedLayout(ExclusiveContext* cx, Shape* templateShape,
                              ObjectGroup* group, PreliminaryObjectArray* objects)
{
    bool isArray = !templateShape;

    // Unboxed arrays are nightly only for now. The getenv() call will be
    // removed when they are on by default. See bug 1153266.
    if (isArray) {
#ifdef NIGHTLY_BUILD
        if (!getenv("JS_OPTION_USE_UNBOXED_ARRAYS")) {
            if (!group->runtimeFromAnyThread()->options().unboxedArrays())
                return true;
        }
#else
        return true;
#endif
    } else {
        if (jit::JitOptions.disableUnboxedObjects)
            return true;
    }

    MOZ_ASSERT_IF(templateShape, !templateShape->getObjectFlags());

    if (group->runtimeFromAnyThread()->isSelfHostingGlobal(cx->global()))
        return true;

    if (!isArray && templateShape->slotSpan() == 0)
        return true;

    UnboxedLayout::PropertyVector properties;
    if (!isArray) {
        if (!properties.appendN(UnboxedLayout::Property(), templateShape->slotSpan()))
            return false;
    }
    JSValueType elementType = JSVAL_TYPE_MAGIC;

    size_t objectCount = 0;
    for (size_t i = 0; i < PreliminaryObjectArray::COUNT; i++) {
        JSObject* obj = objects->get(i);
        if (!obj)
            continue;

        if (obj->isSingleton() || obj->group() != group)
            return true;

        objectCount++;

        if (isArray) {
            if (!CombineArrayObjectElements(cx, &obj->as<ArrayObject>(), &elementType))
                return true;
        } else {
            if (!CombinePlainObjectProperties(&obj->as<PlainObject>(), templateShape, properties))
                return true;
        }
    }

    size_t layoutSize = 0;
    if (isArray) {
        // Don't use an unboxed representation if we couldn't determine an
        // element type for the objects.
        if (UnboxedTypeSize(elementType) == 0)
            return true;
    } else {
        if (objectCount <= 1) {
            // If only one of the objects has been created, it is more likely
            // to have new properties added later. This heuristic is not used
            // for array objects, where we might want an unboxed representation
            // even if there is only one large array.
            return true;
        }

        for (size_t i = 0; i < templateShape->slotSpan(); i++) {
            // We can't use an unboxed representation if e.g. all the objects have
            // a null value for one of the properties, as we can't decide what type
            // it is supposed to have.
            if (UnboxedTypeSize(properties[i].type) == 0)
                return true;
        }

        // Make sure that all properties on the template shape are property
        // names, and not indexes.
        for (Shape::Range<NoGC> r(templateShape); !r.empty(); r.popFront()) {
            jsid id = r.front().propid();
            uint32_t dummy;
            if (!JSID_IS_ATOM(id) || JSID_TO_ATOM(id)->isIndex(&dummy))
                return true;
        }

        layoutSize = ComputePlainObjectLayout(cx, templateShape, properties);

        // The entire object must be allocatable inline.
        if (UnboxedPlainObject::offsetOfData() + layoutSize > JSObject::MAX_BYTE_SIZE)
            return true;
    }

    AutoInitGCManagedObject<UnboxedLayout> layout(group->zone()->make_unique<UnboxedLayout>());
    if (!layout)
        return false;

    if (isArray) {
        layout->initArray(elementType);
    } else {
        if (!layout->initProperties(properties, layoutSize))
            return false;

        // The unboxedLayouts list only tracks layouts for plain objects.
        cx->compartment()->unboxedLayouts.insertFront(layout.get());

        if (!SetLayoutTraceList(cx, layout.get()))
            return false;
    }

    // We've determined that all the preliminary objects can use the new layout
    // just constructed, so convert the existing group to use the unboxed class,
    // and update the preliminary objects to use the new layout. Do the
    // fallible stuff first before modifying any objects.

    // Get an empty shape which we can use for the preliminary objects.
    const Class* clasp = isArray ? &UnboxedArrayObject::class_ : &UnboxedPlainObject::class_;
    Shape* newShape = EmptyShape::getInitialShape(cx, clasp, group->proto(), 0);
    if (!newShape) {
        cx->recoverFromOutOfMemory();
        return false;
    }

    // Accumulate a list of all the values in each preliminary object, and
    // update their shapes.
    AutoValueVector values(cx);
    for (size_t i = 0; i < PreliminaryObjectArray::COUNT; i++) {
        JSObject* obj = objects->get(i);
        if (!obj)
            continue;

        bool ok;
        if (isArray)
            ok = GetValuesFromPreliminaryArrayObject(&obj->as<ArrayObject>(), values);
        else
            ok = GetValuesFromPreliminaryPlainObject(&obj->as<PlainObject>(), values);

        if (!ok) {
            cx->recoverFromOutOfMemory();
            return false;
        }
    }

    if (TypeNewScript* newScript = group->newScript())
        layout->setNewScript(newScript);

    for (size_t i = 0; i < PreliminaryObjectArray::COUNT; i++) {
        if (JSObject* obj = objects->get(i))
            obj->as<NativeObject>().setLastPropertyMakeNonNative(newShape);
    }

    group->setClasp(clasp);
    group->setUnboxedLayout(layout.release());

    size_t valueCursor = 0;
    for (size_t i = 0; i < PreliminaryObjectArray::COUNT; i++) {
        JSObject* obj = objects->get(i);
        if (!obj)
            continue;

        if (isArray)
            obj->as<UnboxedArrayObject>().fillAfterConvert(cx, values, &valueCursor);
        else
            obj->as<UnboxedPlainObject>().fillAfterConvert(cx, values, &valueCursor);
    }

    MOZ_ASSERT(valueCursor == values.length());
    layout.release();
    return true;
}

DefineBoxedOrUnboxedFunctor6(SetOrExtendBoxedOrUnboxedDenseElements,
                             ExclusiveContext*, JSObject*, uint32_t, const Value*, uint32_t,
                             ShouldUpdateTypes);

DenseElementResult
js::SetOrExtendAnyBoxedOrUnboxedDenseElements(ExclusiveContext* cx, JSObject* obj,
                                              uint32_t start, const Value* vp, uint32_t count,
                                              ShouldUpdateTypes updateTypes)
{
    SetOrExtendBoxedOrUnboxedDenseElementsFunctor functor(cx, obj, start, vp, count, updateTypes);
    return CallBoxedOrUnboxedSpecialization(functor, obj);
};

DefineBoxedOrUnboxedFunctor5(MoveBoxedOrUnboxedDenseElements,
                             JSContext*, JSObject*, uint32_t, uint32_t, uint32_t);

DenseElementResult
js::MoveAnyBoxedOrUnboxedDenseElements(JSContext* cx, JSObject* obj,
                                       uint32_t dstStart, uint32_t srcStart, uint32_t length)
{
    MoveBoxedOrUnboxedDenseElementsFunctor functor(cx, obj, dstStart, srcStart, length);
    return CallBoxedOrUnboxedSpecialization(functor, obj);
}

DefineBoxedOrUnboxedFunctorPair6(CopyBoxedOrUnboxedDenseElements,
                                 JSContext*, JSObject*, JSObject*, uint32_t, uint32_t, uint32_t);

DenseElementResult
js::CopyAnyBoxedOrUnboxedDenseElements(JSContext* cx, JSObject* dst, JSObject* src,
                                       uint32_t dstStart, uint32_t srcStart, uint32_t length)
{
    CopyBoxedOrUnboxedDenseElementsFunctor functor(cx, dst, src, dstStart, srcStart, length);
    return CallBoxedOrUnboxedSpecialization(functor, dst, src);
}

DefineBoxedOrUnboxedFunctor3(SetBoxedOrUnboxedInitializedLength,
                             JSContext*, JSObject*, size_t);

void
js::SetAnyBoxedOrUnboxedInitializedLength(JSContext* cx, JSObject* obj, size_t initlen)
{
    SetBoxedOrUnboxedInitializedLengthFunctor functor(cx, obj, initlen);
    JS_ALWAYS_TRUE(CallBoxedOrUnboxedSpecialization(functor, obj) == DenseElementResult::Success);
}

DefineBoxedOrUnboxedFunctor3(EnsureBoxedOrUnboxedDenseElements,
                             JSContext*, JSObject*, size_t);

DenseElementResult
js::EnsureAnyBoxedOrUnboxedDenseElements(JSContext* cx, JSObject* obj, size_t initlen)
{
    EnsureBoxedOrUnboxedDenseElementsFunctor functor(cx, obj, initlen);
    return CallBoxedOrUnboxedSpecialization(functor, obj);
}
