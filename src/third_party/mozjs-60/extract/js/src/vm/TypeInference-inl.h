/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Inline members for javascript type inference. */

#ifndef vm_TypeInference_inl_h
#define vm_TypeInference_inl_h

#include "vm/TypeInference.h"

#include "mozilla/BinarySearch.h"
#include "mozilla/Casting.h"
#include "mozilla/PodOperations.h"

#include "builtin/Symbol.h"
#include "gc/GC.h"
#include "jit/BaselineJIT.h"
#include "vm/ArrayObject.h"
#include "vm/BooleanObject.h"
#include "vm/NumberObject.h"
#include "vm/SharedArrayObject.h"
#include "vm/StringObject.h"
#include "vm/TypedArrayObject.h"
#include "vm/UnboxedObject.h"

#include "vm/JSContext-inl.h"
#include "vm/ObjectGroup-inl.h"

namespace js {

/////////////////////////////////////////////////////////////////////
// CompilerOutput & RecompileInfo
/////////////////////////////////////////////////////////////////////

inline jit::IonScript*
CompilerOutput::ion() const
{
    // Note: If type constraints are generated before compilation has finished
    // (i.e. after IonBuilder but before CodeGenerator::link) then a valid
    // CompilerOutput may not yet have an associated IonScript.
    MOZ_ASSERT(isValid());
    jit::IonScript* ion = script()->maybeIonScript();
    MOZ_ASSERT(ion != ION_COMPILING_SCRIPT);
    return ion;
}

inline CompilerOutput*
RecompileInfo::compilerOutput(TypeZone& types) const
{
    if (generation != types.generation) {
        if (!types.sweepCompilerOutputs || outputIndex >= types.sweepCompilerOutputs->length())
            return nullptr;
        CompilerOutput* output = &(*types.sweepCompilerOutputs)[outputIndex];
        if (!output->isValid())
            return nullptr;
        output = &(*types.compilerOutputs)[output->sweepIndex()];
        return output->isValid() ? output : nullptr;
    }

    if (!types.compilerOutputs || outputIndex >= types.compilerOutputs->length())
        return nullptr;
    CompilerOutput* output = &(*types.compilerOutputs)[outputIndex];
    return output->isValid() ? output : nullptr;
}

inline CompilerOutput*
RecompileInfo::compilerOutput(JSContext* cx) const
{
    return compilerOutput(cx->zone()->types);
}

inline bool
RecompileInfo::shouldSweep(TypeZone& types)
{
    CompilerOutput* output = compilerOutput(types);
    if (!output || !output->isValid())
        return true;

    // If this info is for a compilation that occurred after sweeping started,
    // the index is already correct.
    MOZ_ASSERT_IF(generation == types.generation,
                  outputIndex == output - types.compilerOutputs->begin());

    // Update this info for the output's index in the zone's compiler outputs.
    outputIndex = output - types.compilerOutputs->begin();
    generation = types.generation;
    return false;
}

/////////////////////////////////////////////////////////////////////
// Types
/////////////////////////////////////////////////////////////////////

/* static */ inline TypeSet::ObjectKey*
TypeSet::ObjectKey::get(JSObject* obj)
{
    MOZ_ASSERT(obj);
    if (obj->isSingleton())
        return (ObjectKey*) (uintptr_t(obj) | 1);
    return (ObjectKey*) obj->group();
}

/* static */ inline TypeSet::ObjectKey*
TypeSet::ObjectKey::get(ObjectGroup* group)
{
    MOZ_ASSERT(group);
    if (group->singleton())
        return (ObjectKey*) (uintptr_t(group->singleton()) | 1);
    return (ObjectKey*) group;
}

inline ObjectGroup*
TypeSet::ObjectKey::groupNoBarrier()
{
    MOZ_ASSERT(isGroup());
    return (ObjectGroup*) this;
}

inline JSObject*
TypeSet::ObjectKey::singletonNoBarrier()
{
    MOZ_ASSERT(isSingleton());
    return (JSObject*) (uintptr_t(this) & ~1);
}

inline ObjectGroup*
TypeSet::ObjectKey::group()
{
    ObjectGroup* res = groupNoBarrier();
    ObjectGroup::readBarrier(res);
    return res;
}

inline JSObject*
TypeSet::ObjectKey::singleton()
{
    JSObject* res = singletonNoBarrier();
    JSObject::readBarrier(res);
    return res;
}

inline JSCompartment*
TypeSet::ObjectKey::maybeCompartment()
{
    if (isSingleton())
        return singleton()->compartment();

    return group()->compartment();
}

/* static */ inline TypeSet::Type
TypeSet::ObjectType(JSObject* obj)
{
    if (obj->isSingleton())
        return Type(uintptr_t(obj) | 1);
    return Type(uintptr_t(obj->group()));
}

/* static */ inline TypeSet::Type
TypeSet::ObjectType(ObjectGroup* group)
{
    if (group->singleton())
        return Type(uintptr_t(group->singleton()) | 1);
    return Type(uintptr_t(group));
}

/* static */ inline TypeSet::Type
TypeSet::ObjectType(ObjectKey* obj)
{
    return Type(uintptr_t(obj));
}

inline TypeSet::Type
TypeSet::GetValueType(const Value& val)
{
    if (val.isDouble())
        return TypeSet::DoubleType();
    if (val.isObject())
        return TypeSet::ObjectType(&val.toObject());
    return TypeSet::PrimitiveType(val.extractNonDoubleType());
}

inline bool
TypeSet::IsUntrackedValue(const Value& val)
{
    return val.isMagic() && (val.whyMagic() == JS_OPTIMIZED_OUT ||
                             val.whyMagic() == JS_UNINITIALIZED_LEXICAL);
}

inline TypeSet::Type
TypeSet::GetMaybeUntrackedValueType(const Value& val)
{
    return IsUntrackedValue(val) ? UnknownType() : GetValueType(val);
}

inline TypeFlags
PrimitiveTypeFlag(JSValueType type)
{
    switch (type) {
      case JSVAL_TYPE_UNDEFINED:
        return TYPE_FLAG_UNDEFINED;
      case JSVAL_TYPE_NULL:
        return TYPE_FLAG_NULL;
      case JSVAL_TYPE_BOOLEAN:
        return TYPE_FLAG_BOOLEAN;
      case JSVAL_TYPE_INT32:
        return TYPE_FLAG_INT32;
      case JSVAL_TYPE_DOUBLE:
        return TYPE_FLAG_DOUBLE;
      case JSVAL_TYPE_STRING:
        return TYPE_FLAG_STRING;
      case JSVAL_TYPE_SYMBOL:
        return TYPE_FLAG_SYMBOL;
      case JSVAL_TYPE_MAGIC:
        return TYPE_FLAG_LAZYARGS;
      default:
        MOZ_CRASH("Bad JSValueType");
    }
}

inline JSValueType
TypeFlagPrimitive(TypeFlags flags)
{
    switch (flags) {
      case TYPE_FLAG_UNDEFINED:
        return JSVAL_TYPE_UNDEFINED;
      case TYPE_FLAG_NULL:
        return JSVAL_TYPE_NULL;
      case TYPE_FLAG_BOOLEAN:
        return JSVAL_TYPE_BOOLEAN;
      case TYPE_FLAG_INT32:
        return JSVAL_TYPE_INT32;
      case TYPE_FLAG_DOUBLE:
        return JSVAL_TYPE_DOUBLE;
      case TYPE_FLAG_STRING:
        return JSVAL_TYPE_STRING;
      case TYPE_FLAG_SYMBOL:
        return JSVAL_TYPE_SYMBOL;
      case TYPE_FLAG_LAZYARGS:
        return JSVAL_TYPE_MAGIC;
      default:
        MOZ_CRASH("Bad TypeFlags");
    }
}

/*
 * Get the canonical representation of an id to use when doing inference.  This
 * maintains the constraint that if two different jsids map to the same property
 * in JS (e.g. 3 and "3"), they have the same type representation.
 */
inline jsid
IdToTypeId(jsid id)
{
    MOZ_ASSERT(!JSID_IS_EMPTY(id));

    // All properties which can be stored in an object's dense elements must
    // map to the aggregate property for index types.
    return JSID_IS_INT(id) ? JSID_VOID : id;
}

const char * TypeIdStringImpl(jsid id);

/* Convert an id for printing during debug. */
static inline const char*
TypeIdString(jsid id)
{
#ifdef DEBUG
    return TypeIdStringImpl(id);
#else
    return "(missing)";
#endif
}

/*
 * Structure for type inference entry point functions. All functions which can
 * change type information must use this, and functions which depend on
 * intermediate types (i.e. JITs) can use this to ensure that intermediate
 * information is not collected and does not change.
 *
 * Ensures that GC cannot occur. Does additional sanity checking that inference
 * is not reentrant and that recompilations occur properly.
 */
struct AutoEnterAnalysis
{
    // For use when initializing an UnboxedLayout.  The UniquePtr's destructor
    // must run when GC is not suppressed.
    UniquePtr<UnboxedLayout> unboxedLayoutToCleanUp;

    // Prevent GC activity in the middle of analysis.
    gc::AutoSuppressGC suppressGC;

    // Allow clearing inference info on OOM during incremental sweeping.
    mozilla::Maybe<AutoClearTypeInferenceStateOnOOM> oom;

    // Pending recompilations to perform before execution of JIT code can resume.
    RecompileInfoVector pendingRecompiles;

    // Prevent us from calling the objectMetadataCallback.
    js::AutoSuppressAllocationMetadataBuilder suppressMetadata;

    FreeOp* freeOp;
    Zone* zone;

    explicit AutoEnterAnalysis(JSContext* cx)
      : suppressGC(cx), suppressMetadata(cx)
    {
        init(cx->defaultFreeOp(), cx->zone());
    }

    AutoEnterAnalysis(FreeOp* fop, Zone* zone)
      : suppressGC(TlsContext.get()),
        suppressMetadata(zone)
    {
        init(fop, zone);
    }

    ~AutoEnterAnalysis()
    {
        if (this != zone->types.activeAnalysis)
            return;

        zone->types.activeAnalysis = nullptr;

        if (!pendingRecompiles.empty())
            zone->types.processPendingRecompiles(freeOp, pendingRecompiles);
    }

  private:
    void init(FreeOp* fop, Zone* zone) {
#ifdef JS_CRASH_DIAGNOSTICS
        MOZ_RELEASE_ASSERT(CurrentThreadCanAccessZone(zone));
#endif
        this->freeOp = fop;
        this->zone = zone;

        if (!zone->types.activeAnalysis) {
            MOZ_RELEASE_ASSERT(!zone->types.sweepingTypes);
            zone->types.activeAnalysis = this;
        }
    }
};

/////////////////////////////////////////////////////////////////////
// Interface functions
/////////////////////////////////////////////////////////////////////

void MarkIteratorUnknownSlow(JSContext* cx);

void TypeMonitorCallSlow(JSContext* cx, JSObject* callee, const CallArgs& args,
                         bool constructing);

/*
 * Monitor a javascript call, either on entry to the interpreter or made
 * from within the interpreter.
 */
inline void
TypeMonitorCall(JSContext* cx, const js::CallArgs& args, bool constructing)
{
    if (args.callee().is<JSFunction>()) {
        JSFunction* fun = &args.callee().as<JSFunction>();
        if (fun->isInterpreted() && fun->nonLazyScript()->types())
            TypeMonitorCallSlow(cx, &args.callee(), args, constructing);
    }
}

MOZ_ALWAYS_INLINE bool
TrackPropertyTypes(JSObject* obj, jsid id)
{
    if (obj->hasLazyGroup() || obj->group()->unknownPropertiesDontCheckGeneration())
        return false;

    if (obj->isSingleton() && !obj->group()->maybeGetPropertyDontCheckGeneration(id))
        return false;

    return true;
}

void
EnsureTrackPropertyTypes(JSContext* cx, JSObject* obj, jsid id);

inline bool
CanHaveEmptyPropertyTypesForOwnProperty(JSObject* obj)
{
    // Per the comment on TypeSet::propertySet, property type sets for global
    // objects may be empty for 'own' properties if the global property still
    // has its initial undefined value.
    return obj->is<GlobalObject>();
}

inline bool
PropertyHasBeenMarkedNonConstant(JSObject* obj, jsid id)
{
    // Non-constant properties are only relevant for singleton objects.
    if (!obj->isSingleton())
        return true;

    // EnsureTrackPropertyTypes must have been called on this object.
    if (obj->group()->unknownProperties())
        return true;
    HeapTypeSet* types = obj->group()->maybeGetProperty(IdToTypeId(id));
    return types->nonConstantProperty();
}

MOZ_ALWAYS_INLINE bool
HasTrackedPropertyType(JSObject* obj, jsid id, TypeSet::Type type)
{
    MOZ_ASSERT(id == IdToTypeId(id));
    MOZ_ASSERT(TrackPropertyTypes(obj, id));

    if (HeapTypeSet* types = obj->group()->maybeGetPropertyDontCheckGeneration(id)) {
        if (!types->hasType(type))
            return false;
        // Non-constant properties are only relevant for singleton objects.
        if (obj->isSingleton() && !types->nonConstantProperty())
            return false;
        return true;
    }

    return false;
}

MOZ_ALWAYS_INLINE bool
HasTypePropertyId(JSObject* obj, jsid id, TypeSet::Type type)
{
    id = IdToTypeId(id);
    if (!TrackPropertyTypes(obj, id))
        return true;

    return HasTrackedPropertyType(obj, id, type);
}

MOZ_ALWAYS_INLINE bool
HasTypePropertyId(JSObject* obj, jsid id, const Value& value)
{
    return HasTypePropertyId(obj, id, TypeSet::GetValueType(value));
}

void AddTypePropertyId(JSContext* cx, ObjectGroup* group, JSObject* obj, jsid id, TypeSet::Type type);
void AddTypePropertyId(JSContext* cx, ObjectGroup* group, JSObject* obj, jsid id, const Value& value);

/* Add a possible type for a property of obj. */
MOZ_ALWAYS_INLINE void
AddTypePropertyId(JSContext* cx, JSObject* obj, jsid id, TypeSet::Type type)
{
    id = IdToTypeId(id);
    if (TrackPropertyTypes(obj, id) && !HasTrackedPropertyType(obj, id, type))
        AddTypePropertyId(cx, obj->group(), obj, id, type);
}

MOZ_ALWAYS_INLINE void
AddTypePropertyId(JSContext* cx, JSObject* obj, jsid id, const Value& value)
{
    return AddTypePropertyId(cx, obj, id, TypeSet::GetValueType(value));
}

inline void
MarkObjectGroupFlags(JSContext* cx, JSObject* obj, ObjectGroupFlags flags)
{
    if (!obj->hasLazyGroup() && !obj->group()->hasAllFlags(flags))
        obj->group()->setFlags(cx, flags);
}

inline void
MarkObjectGroupUnknownProperties(JSContext* cx, ObjectGroup* obj)
{
    if (!obj->unknownProperties())
        obj->markUnknown(cx);
}

inline void
MarkTypePropertyNonData(JSContext* cx, JSObject* obj, jsid id)
{
    id = IdToTypeId(id);
    if (TrackPropertyTypes(obj, id))
        obj->group()->markPropertyNonData(cx, obj, id);
}

inline void
MarkTypePropertyNonWritable(JSContext* cx, JSObject* obj, jsid id)
{
    id = IdToTypeId(id);
    if (TrackPropertyTypes(obj, id))
        obj->group()->markPropertyNonWritable(cx, obj, id);
}

/* Mark a state change on a particular object. */
inline void
MarkObjectStateChange(JSContext* cx, JSObject* obj)
{
    if (!obj->hasLazyGroup() && !obj->group()->unknownProperties())
        obj->group()->markStateChange(cx);
}

/* Interface helpers for JSScript*. */
extern void TypeMonitorResult(JSContext* cx, JSScript* script, jsbytecode* pc, TypeSet::Type type);
extern void TypeMonitorResult(JSContext* cx, JSScript* script, jsbytecode* pc, StackTypeSet* types,
                              TypeSet::Type type);
extern void TypeMonitorResult(JSContext* cx, JSScript* script, jsbytecode* pc, const Value& rval);

/////////////////////////////////////////////////////////////////////
// Script interface functions
/////////////////////////////////////////////////////////////////////

/* static */ inline unsigned
TypeScript::NumTypeSets(JSScript* script)
{
    size_t num = script->nTypeSets() + 1 /* this */;
    if (JSFunction* fun = script->functionNonDelazifying())
        num += fun->nargs();
    return num;
}

/* static */ inline StackTypeSet*
TypeScript::ThisTypes(JSScript* script)
{
    TypeScript* types = script->types();
    return types ? types->typeArray() + script->nTypeSets() : nullptr;
}

/*
 * Note: for non-escaping arguments, argTypes reflect only the initial type of
 * the variable (e.g. passed values for argTypes, or undefined for localTypes)
 * and not types from subsequent assignments.
 */

/* static */ inline StackTypeSet*
TypeScript::ArgTypes(JSScript* script, unsigned i)
{
    MOZ_ASSERT(i < script->functionNonDelazifying()->nargs());
    TypeScript* types = script->types();
    return types ? types->typeArray() + script->nTypeSets() + 1 + i : nullptr;
}

template <typename TYPESET>
/* static */ inline TYPESET*
TypeScript::BytecodeTypes(JSScript* script, jsbytecode* pc, uint32_t* bytecodeMap,
                          uint32_t* hint, TYPESET* typeArray)
{
    MOZ_ASSERT(CodeSpec[*pc].format & JOF_TYPESET);
    uint32_t offset = script->pcToOffset(pc);

    // See if this pc is the next typeset opcode after the last one looked up.
    if ((*hint + 1) < script->nTypeSets() && bytecodeMap[*hint + 1] == offset) {
        (*hint)++;
        return typeArray + *hint;
    }

    // See if this pc is the same as the last one looked up.
    if (bytecodeMap[*hint] == offset)
        return typeArray + *hint;

    // Fall back to a binary search.  We'll either find the exact offset, or
    // there are more JOF_TYPESET opcodes than nTypeSets in the script (as can
    // happen if the script is very long) and we'll use the last location.
    size_t loc;
#ifdef DEBUG
    bool found =
#endif
        mozilla::BinarySearch(bytecodeMap, 0, script->nTypeSets() - 1, offset, &loc);

    MOZ_ASSERT_IF(found, bytecodeMap[loc] == offset);
    *hint = mozilla::AssertedCast<uint32_t>(loc);
    return typeArray + *hint;
}

/* static */ inline StackTypeSet*
TypeScript::BytecodeTypes(JSScript* script, jsbytecode* pc)
{
    MOZ_ASSERT(CurrentThreadCanAccessZone(script->zone()));
    TypeScript* types = script->types();
    if (!types)
        return nullptr;
    uint32_t* hint = script->baselineScript()->bytecodeTypeMap() + script->nTypeSets();
    return BytecodeTypes(script, pc, script->baselineScript()->bytecodeTypeMap(),
                         hint, types->typeArray());
}

/* static */ inline void
TypeScript::Monitor(JSContext* cx, JSScript* script, jsbytecode* pc, const js::Value& rval)
{
    TypeMonitorResult(cx, script, pc, rval);
}

/* static */ inline void
TypeScript::Monitor(JSContext* cx, JSScript* script, jsbytecode* pc, TypeSet::Type type)
{
    TypeMonitorResult(cx, script, pc, type);
}

/* static */ inline void
TypeScript::Monitor(JSContext* cx, const js::Value& rval)
{
    jsbytecode* pc;
    RootedScript script(cx, cx->currentScript(&pc));
    Monitor(cx, script, pc, rval);
}

/* static */ inline void
TypeScript::Monitor(JSContext* cx, JSScript* script, jsbytecode* pc, StackTypeSet* types,
                    const js::Value& rval)
{
    TypeSet::Type type = TypeSet::GetValueType(rval);
    if (!types->hasType(type))
        TypeMonitorResult(cx, script, pc, types, type);
}

/* static */ inline void
TypeScript::MonitorAssign(JSContext* cx, HandleObject obj, jsid id)
{
    if (!obj->isSingleton()) {
        /*
         * Mark as unknown any object which has had dynamic assignments to
         * non-integer properties at SETELEM opcodes. This avoids making large
         * numbers of type properties for hashmap-style objects. We don't need
         * to do this for objects with singleton type, because type properties
         * are only constructed for them when analyzed scripts depend on those
         * specific properties.
         */
        uint32_t i;
        if (IdIsIndex(id, &i))
            return;

        // But if we don't have too many properties yet, don't do anything.  The
        // idea here is that normal object initialization should not trigger
        // deoptimization in most cases, while actual usage as a hashmap should.
        ObjectGroup* group = obj->group();
        if (group->basePropertyCount() < 128)
            return;
        MarkObjectGroupUnknownProperties(cx, group);
    }
}

/* static */ inline void
TypeScript::SetThis(JSContext* cx, JSScript* script, TypeSet::Type type)
{
    assertSameCompartment(cx, script, type);

    StackTypeSet* types = ThisTypes(script);
    if (!types)
        return;

    if (!types->hasType(type)) {
        AutoEnterAnalysis enter(cx);

        InferSpew(ISpewOps, "externalType: setThis %p: %s",
                  script, TypeSet::TypeString(type).get());
        types->addType(cx, type);
    }
}

/* static */ inline void
TypeScript::SetThis(JSContext* cx, JSScript* script, const js::Value& value)
{
    SetThis(cx, script, TypeSet::GetValueType(value));
}

/* static */ inline void
TypeScript::SetArgument(JSContext* cx, JSScript* script, unsigned arg, TypeSet::Type type)
{
    assertSameCompartment(cx, script, type);

    StackTypeSet* types = ArgTypes(script, arg);
    if (!types)
        return;

    if (!types->hasType(type)) {
        AutoEnterAnalysis enter(cx);

        InferSpew(ISpewOps, "externalType: setArg %p %u: %s",
                  script, arg, TypeSet::TypeString(type).get());
        types->addType(cx, type);
    }
}

/* static */ inline void
TypeScript::SetArgument(JSContext* cx, JSScript* script, unsigned arg, const js::Value& value)
{
    SetArgument(cx, script, arg, TypeSet::GetValueType(value));
}

inline
AutoKeepTypeScripts::AutoKeepTypeScripts(JSContext* cx)
  : zone_(cx->zone()->types),
    prev_(zone_.keepTypeScripts)
{
    zone_.keepTypeScripts = true;
}

inline
AutoKeepTypeScripts::~AutoKeepTypeScripts()
{
    MOZ_ASSERT(zone_.keepTypeScripts);
    zone_.keepTypeScripts = prev_;
}

/////////////////////////////////////////////////////////////////////
// TypeHashSet
/////////////////////////////////////////////////////////////////////

// Hashing code shared by objects in TypeSets and properties in ObjectGroups.
struct TypeHashSet
{
    // The sets of objects in a type set grow monotonically, are usually empty,
    // almost always small, and sometimes big. For empty or singleton sets, the
    // the pointer refers directly to the value.  For sets fitting into
    // SET_ARRAY_SIZE, an array of this length is used to store the elements.
    // For larger sets, a hash table filled to 25%-50% of capacity is used,
    // with collisions resolved by linear probing.
    static const unsigned SET_ARRAY_SIZE = 8;
    static const unsigned SET_CAPACITY_OVERFLOW = 1u << 30;

    // Get the capacity of a set with the given element count.
    static inline unsigned
    Capacity(unsigned count)
    {
        MOZ_ASSERT(count >= 2);
        MOZ_ASSERT(count < SET_CAPACITY_OVERFLOW);

        if (count <= SET_ARRAY_SIZE)
            return SET_ARRAY_SIZE;

        return 1u << (mozilla::FloorLog2(count) + 2);
    }

    // Compute the FNV hash for the low 32 bits of v.
    template <class T, class KEY>
    static inline uint32_t
    HashKey(T v)
    {
        uint32_t nv = KEY::keyBits(v);

        uint32_t hash = 84696351 ^ (nv & 0xff);
        hash = (hash * 16777619) ^ ((nv >> 8) & 0xff);
        hash = (hash * 16777619) ^ ((nv >> 16) & 0xff);
        return (hash * 16777619) ^ ((nv >> 24) & 0xff);
    }

    // Insert space for an element into the specified set and grow its capacity
    // if needed. returned value is an existing or new entry (nullptr if new).
    template <class T, class U, class KEY>
    static U**
    InsertTry(LifoAlloc& alloc, U**& values, unsigned& count, T key)
    {
        unsigned capacity = Capacity(count);
        unsigned insertpos = HashKey<T,KEY>(key) & (capacity - 1);

        MOZ_RELEASE_ASSERT(uintptr_t(values[-1]) == capacity);

        // Whether we are converting from a fixed array to hashtable.
        bool converting = (count == SET_ARRAY_SIZE);

        if (!converting) {
            while (values[insertpos] != nullptr) {
                if (KEY::getKey(values[insertpos]) == key)
                    return &values[insertpos];
                insertpos = (insertpos + 1) & (capacity - 1);
            }
        }

        if (count >= SET_CAPACITY_OVERFLOW)
            return nullptr;

        count++;
        unsigned newCapacity = Capacity(count);

        if (newCapacity == capacity) {
            MOZ_ASSERT(!converting);
            return &values[insertpos];
        }

        // Allocate an extra word right before the array storing the capacity,
        // for sanity checks.
        U** newValues = alloc.newArray<U*>(newCapacity + 1);
        if (!newValues)
            return nullptr;
        mozilla::PodZero(newValues, newCapacity + 1);

        newValues[0] = (U*)uintptr_t(newCapacity);
        newValues++;

        for (unsigned i = 0; i < capacity; i++) {
            if (values[i]) {
                unsigned pos = HashKey<T,KEY>(KEY::getKey(values[i])) & (newCapacity - 1);
                while (newValues[pos] != nullptr)
                    pos = (pos + 1) & (newCapacity - 1);
                newValues[pos] = values[i];
            }
        }

        values = newValues;

        insertpos = HashKey<T,KEY>(key) & (newCapacity - 1);
        while (values[insertpos] != nullptr)
            insertpos = (insertpos + 1) & (newCapacity - 1);
        return &values[insertpos];
    }

    // Insert an element into the specified set if it is not already there,
    // returning an entry which is nullptr if the element was not there.
    template <class T, class U, class KEY>
    static inline U**
    Insert(LifoAlloc& alloc, U**& values, unsigned& count, T key)
    {
        if (count == 0) {
            MOZ_ASSERT(values == nullptr);
            count++;
            return (U**) &values;
        }

        if (count == 1) {
            U* oldData = (U*) values;
            if (KEY::getKey(oldData) == key)
                return (U**) &values;

            // Allocate an extra word right before the array storing the
            // capacity, for sanity checks.
            values = alloc.newArray<U*>(SET_ARRAY_SIZE + 1);
            if (!values) {
                values = (U**) oldData;
                return nullptr;
            }
            mozilla::PodZero(values, SET_ARRAY_SIZE + 1);

            values[0] = (U*)uintptr_t(SET_ARRAY_SIZE);
            values++;

            count++;

            values[0] = oldData;
            return &values[1];
        }

        if (count <= SET_ARRAY_SIZE) {
            MOZ_RELEASE_ASSERT(uintptr_t(values[-1]) == SET_ARRAY_SIZE);

            for (unsigned i = 0; i < count; i++) {
                if (KEY::getKey(values[i]) == key)
                    return &values[i];
            }

            if (count < SET_ARRAY_SIZE) {
                count++;
                return &values[count - 1];
            }
        }

        return InsertTry<T,U,KEY>(alloc, values, count, key);
    }

    // Lookup an entry in a hash set, return nullptr if it does not exist.
    template <class T, class U, class KEY>
    static MOZ_ALWAYS_INLINE U*
    Lookup(U** values, unsigned count, T key)
    {
        if (count == 0)
            return nullptr;

        if (count == 1)
            return (KEY::getKey((U*) values) == key) ? (U*) values : nullptr;

        if (count <= SET_ARRAY_SIZE) {
            MOZ_RELEASE_ASSERT(uintptr_t(values[-1]) == SET_ARRAY_SIZE);
            for (unsigned i = 0; i < count; i++) {
                if (KEY::getKey(values[i]) == key)
                    return values[i];
            }
            return nullptr;
        }

        unsigned capacity = Capacity(count);
        unsigned pos = HashKey<T,KEY>(key) & (capacity - 1);

        MOZ_RELEASE_ASSERT(uintptr_t(values[-1]) == capacity);

        while (values[pos] != nullptr) {
            if (KEY::getKey(values[pos]) == key)
                return values[pos];
            pos = (pos + 1) & (capacity - 1);
        }

        return nullptr;
    }
};

/////////////////////////////////////////////////////////////////////
// TypeSet
/////////////////////////////////////////////////////////////////////

inline TypeSet::ObjectKey*
TypeSet::Type::objectKey() const
{
    MOZ_ASSERT(isObject());
    return (ObjectKey*) data;
}

inline JSObject*
TypeSet::Type::singleton() const
{
    return objectKey()->singleton();
}

inline ObjectGroup*
TypeSet::Type::group() const
{
    return objectKey()->group();
}

inline JSObject*
TypeSet::Type::singletonNoBarrier() const
{
    return objectKey()->singletonNoBarrier();
}

inline ObjectGroup*
TypeSet::Type::groupNoBarrier() const
{
    return objectKey()->groupNoBarrier();
}

inline void
TypeSet::Type::trace(JSTracer* trc)
{
    if (isSingletonUnchecked()) {
        JSObject* obj = singletonNoBarrier();
        TraceManuallyBarrieredEdge(trc, &obj, "TypeSet::Object");
        *this = TypeSet::ObjectType(obj);
    } else if (isGroupUnchecked()) {
        ObjectGroup* group = groupNoBarrier();
        TraceManuallyBarrieredEdge(trc, &group, "TypeSet::Group");
        *this = TypeSet::ObjectType(group);
    }
}

inline JSCompartment*
TypeSet::Type::maybeCompartment()
{
    if (isSingletonUnchecked())
        return singletonNoBarrier()->compartment();

    if (isGroupUnchecked())
        return groupNoBarrier()->compartment();

    return nullptr;
}

MOZ_ALWAYS_INLINE bool
TypeSet::hasType(Type type) const
{
    if (unknown())
        return true;

    if (type.isUnknown()) {
        return false;
    } else if (type.isPrimitive()) {
        return !!(flags & PrimitiveTypeFlag(type.primitive()));
    } else if (type.isAnyObject()) {
        return !!(flags & TYPE_FLAG_ANYOBJECT);
    } else {
        return !!(flags & TYPE_FLAG_ANYOBJECT) ||
               TypeHashSet::Lookup<ObjectKey*, ObjectKey, ObjectKey>
                   (objectSet, baseObjectCount(), type.objectKey()) != nullptr;
    }
}

inline void
TypeSet::setBaseObjectCount(uint32_t count)
{
    MOZ_ASSERT(count <= TYPE_FLAG_DOMOBJECT_COUNT_LIMIT);
    flags = (flags & ~TYPE_FLAG_OBJECT_COUNT_MASK)
          | (count << TYPE_FLAG_OBJECT_COUNT_SHIFT);
}

inline void
HeapTypeSet::newPropertyState(JSContext* cx)
{
    checkMagic();

    /* Propagate the change to all constraints. */
    if (!cx->helperThread()) {
        TypeConstraint* constraint = constraintList();
        while (constraint) {
            constraint->newPropertyState(cx, this);
            constraint = constraint->next();
        }
    } else {
        MOZ_ASSERT(!constraintList());
    }
}

inline void
HeapTypeSet::setNonDataProperty(JSContext* cx)
{
    checkMagic();

    if (flags & TYPE_FLAG_NON_DATA_PROPERTY)
        return;

    flags |= TYPE_FLAG_NON_DATA_PROPERTY;
    newPropertyState(cx);
}

inline void
HeapTypeSet::setNonWritableProperty(JSContext* cx)
{
    checkMagic();

    if (flags & TYPE_FLAG_NON_WRITABLE_PROPERTY)
        return;

    flags |= TYPE_FLAG_NON_WRITABLE_PROPERTY;
    newPropertyState(cx);
}

inline void
HeapTypeSet::setNonConstantProperty(JSContext* cx)
{
    checkMagic();

    if (flags & TYPE_FLAG_NON_CONSTANT_PROPERTY)
        return;

    flags |= TYPE_FLAG_NON_CONSTANT_PROPERTY;
    newPropertyState(cx);
}

inline unsigned
TypeSet::getObjectCount() const
{
    MOZ_ASSERT(!unknownObject());
    uint32_t count = baseObjectCount();
    if (count > TypeHashSet::SET_ARRAY_SIZE)
        return TypeHashSet::Capacity(count);
    return count;
}

inline TypeSet::ObjectKey*
TypeSet::getObject(unsigned i) const
{
    MOZ_ASSERT(i < getObjectCount());
    if (baseObjectCount() == 1) {
        MOZ_ASSERT(i == 0);
        return (ObjectKey*) objectSet;
    }
    return objectSet[i];
}

inline JSObject*
TypeSet::getSingleton(unsigned i) const
{
    ObjectKey* key = getObject(i);
    return (key && key->isSingleton()) ? key->singleton() : nullptr;
}

inline ObjectGroup*
TypeSet::getGroup(unsigned i) const
{
    ObjectKey* key = getObject(i);
    return (key && key->isGroup()) ? key->group() : nullptr;
}

inline JSObject*
TypeSet::getSingletonNoBarrier(unsigned i) const
{
    ObjectKey* key = getObject(i);
    return (key && key->isSingleton()) ? key->singletonNoBarrier() : nullptr;
}

inline ObjectGroup*
TypeSet::getGroupNoBarrier(unsigned i) const
{
    ObjectKey* key = getObject(i);
    return (key && key->isGroup()) ? key->groupNoBarrier() : nullptr;
}

inline bool
TypeSet::hasGroup(unsigned i) const
{
    return getGroupNoBarrier(i);
}

inline bool
TypeSet::hasSingleton(unsigned i) const
{
    return getSingletonNoBarrier(i);
}

inline const Class*
TypeSet::getObjectClass(unsigned i) const
{
    if (JSObject* object = getSingleton(i))
        return object->getClass();
    if (ObjectGroup* group = getGroup(i))
        return group->clasp();
    return nullptr;
}

/////////////////////////////////////////////////////////////////////
// ObjectGroup
/////////////////////////////////////////////////////////////////////

inline uint32_t
ObjectGroup::basePropertyCountDontCheckGeneration()
{
    uint32_t flags = flagsDontCheckGeneration();
    return (flags & OBJECT_FLAG_PROPERTY_COUNT_MASK) >> OBJECT_FLAG_PROPERTY_COUNT_SHIFT;
}

inline uint32_t
ObjectGroup::basePropertyCount()
{
    maybeSweep(nullptr);
    return basePropertyCountDontCheckGeneration();
}

inline void
ObjectGroup::setBasePropertyCount(uint32_t count)
{
    // Note: Callers must ensure they are performing threadsafe operations.
    MOZ_ASSERT(count <= OBJECT_FLAG_PROPERTY_COUNT_LIMIT);
    flags_ = (flags() & ~OBJECT_FLAG_PROPERTY_COUNT_MASK)
           | (count << OBJECT_FLAG_PROPERTY_COUNT_SHIFT);
}

inline HeapTypeSet*
ObjectGroup::getProperty(JSContext* cx, JSObject* obj, jsid id)
{
    MOZ_ASSERT(JSID_IS_VOID(id) || JSID_IS_EMPTY(id) || JSID_IS_STRING(id) || JSID_IS_SYMBOL(id));
    MOZ_ASSERT_IF(!JSID_IS_EMPTY(id), id == IdToTypeId(id));
    MOZ_ASSERT(!unknownProperties());
    MOZ_ASSERT_IF(obj, obj->group() == this);
    MOZ_ASSERT_IF(singleton(), obj);

    if (HeapTypeSet* types = maybeGetProperty(id))
        return types;

    Property* base = cx->typeLifoAlloc().new_<Property>(id);
    if (!base) {
        markUnknown(cx);
        return nullptr;
    }

    uint32_t propertyCount = basePropertyCount();
    Property** pprop = TypeHashSet::Insert<jsid, Property, Property>
                           (cx->typeLifoAlloc(), propertySet, propertyCount, id);
    if (!pprop) {
        markUnknown(cx);
        return nullptr;
    }

    MOZ_ASSERT(!*pprop);

    setBasePropertyCount(propertyCount);
    *pprop = base;

    updateNewPropertyTypes(cx, obj, id, &base->types);

    if (propertyCount == OBJECT_FLAG_PROPERTY_COUNT_LIMIT) {
        // We hit the maximum number of properties the object can have, mark
        // the object unknown so that new properties will not be added in the
        // future.
        markUnknown(cx);
    }

    base->types.checkMagic();
    return &base->types;
}

MOZ_ALWAYS_INLINE HeapTypeSet*
ObjectGroup::maybeGetPropertyDontCheckGeneration(jsid id)
{
    MOZ_ASSERT(JSID_IS_VOID(id) || JSID_IS_EMPTY(id) || JSID_IS_STRING(id) || JSID_IS_SYMBOL(id));
    MOZ_ASSERT_IF(!JSID_IS_EMPTY(id), id == IdToTypeId(id));
    MOZ_ASSERT(!unknownPropertiesDontCheckGeneration());

    Property* prop = TypeHashSet::Lookup<jsid, Property, Property>
                         (propertySet, basePropertyCountDontCheckGeneration(), id);

    if (!prop)
        return nullptr;

    prop->types.checkMagic();
    return &prop->types;
}

MOZ_ALWAYS_INLINE HeapTypeSet*
ObjectGroup::maybeGetProperty(jsid id)
{
    maybeSweep(nullptr);
    return maybeGetPropertyDontCheckGeneration(id);
}

inline unsigned
ObjectGroup::getPropertyCount()
{
    uint32_t count = basePropertyCount();
    if (count > TypeHashSet::SET_ARRAY_SIZE)
        return TypeHashSet::Capacity(count);
    return count;
}

inline ObjectGroup::Property*
ObjectGroup::getProperty(unsigned i)
{
    MOZ_ASSERT(i < getPropertyCount());
    Property* result;
    if (basePropertyCount() == 1) {
        MOZ_ASSERT(i == 0);
        result = (Property*) propertySet;
    } else {
        result = propertySet[i];
    }
    if (result)
        result->types.checkMagic();
    return result;
}

} // namespace js

inline js::TypeScript*
JSScript::types()
{
    maybeSweepTypes(nullptr);
    return types_;
}

inline bool
JSScript::ensureHasTypes(JSContext* cx, js::AutoKeepTypeScripts&)
{
    return types() || makeTypes(cx);
}

#endif /* vm_TypeInference_inl_h */
