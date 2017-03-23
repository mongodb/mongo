/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/TypeInference-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"

#include "jsapi.h"
#include "jscntxt.h"
#include "jsgc.h"
#include "jshashutil.h"
#include "jsobj.h"
#include "jsprf.h"
#include "jsscript.h"
#include "jsstr.h"
#include "prmjtime.h"

#include "gc/Marking.h"
#include "jit/BaselineJIT.h"
#include "jit/CompileInfo.h"
#include "jit/Ion.h"
#include "jit/IonAnalysis.h"
#include "jit/JitCompartment.h"
#include "js/MemoryMetrics.h"
#include "vm/HelperThreads.h"
#include "vm/Opcodes.h"
#include "vm/Shape.h"
#include "vm/UnboxedObject.h"

#include "jsatominlines.h"
#include "jsgcinlines.h"
#include "jsscriptinlines.h"

#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::DebugOnly;
using mozilla::Maybe;
using mozilla::PodArrayZero;
using mozilla::PodCopy;
using mozilla::PodZero;

static inline jsid
id_prototype(JSContext* cx)
{
    return NameToId(cx->names().prototype);
}

#ifdef DEBUG

static inline jsid
id___proto__(JSContext* cx)
{
    return NameToId(cx->names().proto);
}

static inline jsid
id_constructor(JSContext* cx)
{
    return NameToId(cx->names().constructor);
}

static inline jsid
id_caller(JSContext* cx)
{
    return NameToId(cx->names().caller);
}

const char*
js::TypeIdStringImpl(jsid id)
{
    if (JSID_IS_VOID(id))
        return "(index)";
    if (JSID_IS_EMPTY(id))
        return "(new)";
    if (JSID_IS_SYMBOL(id))
        return "(symbol)";
    static char bufs[4][100];
    static unsigned which = 0;
    which = (which + 1) & 3;
    PutEscapedString(bufs[which], 100, JSID_TO_FLAT_STRING(id), 0);
    return bufs[which];
}

#endif

/////////////////////////////////////////////////////////////////////
// Logging
/////////////////////////////////////////////////////////////////////

/* static */ const char*
TypeSet::NonObjectTypeString(TypeSet::Type type)
{
    if (type.isPrimitive()) {
        switch (type.primitive()) {
          case JSVAL_TYPE_UNDEFINED:
            return "void";
          case JSVAL_TYPE_NULL:
            return "null";
          case JSVAL_TYPE_BOOLEAN:
            return "bool";
          case JSVAL_TYPE_INT32:
            return "int";
          case JSVAL_TYPE_DOUBLE:
            return "float";
          case JSVAL_TYPE_STRING:
            return "string";
          case JSVAL_TYPE_SYMBOL:
            return "symbol";
          case JSVAL_TYPE_MAGIC:
            return "lazyargs";
          default:
            MOZ_CRASH("Bad type");
        }
    }
    if (type.isUnknown())
        return "unknown";

    MOZ_ASSERT(type.isAnyObject());
    return "object";
}

#ifdef DEBUG

static bool InferSpewActive(SpewChannel channel)
{
    static bool active[SPEW_COUNT];
    static bool checked = false;
    if (!checked) {
        checked = true;
        PodArrayZero(active);
        const char* env = getenv("INFERFLAGS");
        if (!env)
            return false;
        if (strstr(env, "ops"))
            active[ISpewOps] = true;
        if (strstr(env, "result"))
            active[ISpewResult] = true;
        if (strstr(env, "full")) {
            for (unsigned i = 0; i < SPEW_COUNT; i++)
                active[i] = true;
        }
    }
    return active[channel];
}

static bool InferSpewColorable()
{
    /* Only spew colors on xterm-color to not screw up emacs. */
    static bool colorable = false;
    static bool checked = false;
    if (!checked) {
        checked = true;
        const char* env = getenv("TERM");
        if (!env)
            return false;
        if (strcmp(env, "xterm-color") == 0 || strcmp(env, "xterm-256color") == 0)
            colorable = true;
    }
    return colorable;
}

const char*
js::InferSpewColorReset()
{
    if (!InferSpewColorable())
        return "";
    return "\x1b[0m";
}

const char*
js::InferSpewColor(TypeConstraint* constraint)
{
    /* Type constraints are printed out using foreground colors. */
    static const char * const colors[] = { "\x1b[31m", "\x1b[32m", "\x1b[33m",
                                           "\x1b[34m", "\x1b[35m", "\x1b[36m",
                                           "\x1b[37m" };
    if (!InferSpewColorable())
        return "";
    return colors[DefaultHasher<TypeConstraint*>::hash(constraint) % 7];
}

const char*
js::InferSpewColor(TypeSet* types)
{
    /* Type sets are printed out using bold colors. */
    static const char * const colors[] = { "\x1b[1;31m", "\x1b[1;32m", "\x1b[1;33m",
                                           "\x1b[1;34m", "\x1b[1;35m", "\x1b[1;36m",
                                           "\x1b[1;37m" };
    if (!InferSpewColorable())
        return "";
    return colors[DefaultHasher<TypeSet*>::hash(types) % 7];
}

/* static */ const char*
TypeSet::TypeString(TypeSet::Type type)
{
    if (type.isPrimitive() || type.isUnknown() || type.isAnyObject())
        return NonObjectTypeString(type);

    static char bufs[4][40];
    static unsigned which = 0;
    which = (which + 1) & 3;

    if (type.isSingleton())
        JS_snprintf(bufs[which], 40, "<0x%p>", (void*) type.singleton());
    else
        JS_snprintf(bufs[which], 40, "[0x%p]", (void*) type.group());

    return bufs[which];
}

/* static */ const char*
TypeSet::ObjectGroupString(ObjectGroup* group)
{
    return TypeString(TypeSet::ObjectType(group));
}

void
js::InferSpew(SpewChannel channel, const char* fmt, ...)
{
    if (!InferSpewActive(channel))
        return;

    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[infer] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

bool
js::ObjectGroupHasProperty(JSContext* cx, ObjectGroup* group, jsid id, const Value& value)
{
    /*
     * Check the correctness of the type information in the object's property
     * against an actual value.
     */
    if (!group->unknownProperties() && !value.isUndefined()) {
        id = IdToTypeId(id);

        /* Watch for properties which inference does not monitor. */
        if (id == id___proto__(cx) || id == id_constructor(cx) || id == id_caller(cx))
            return true;

        TypeSet::Type type = TypeSet::GetValueType(value);

        // Type set guards might miss when an object's group changes and its
        // properties become unknown.
        if (value.isObject() &&
            !value.toObject().hasLazyGroup() &&
            value.toObject().group()->flags() & OBJECT_FLAG_UNKNOWN_PROPERTIES)
        {
            return true;
        }

        AutoEnterAnalysis enter(cx);

        /*
         * We don't track types for properties inherited from prototypes which
         * haven't yet been accessed during analysis of the inheriting object.
         * Don't do the property instantiation now.
         */
        TypeSet* types = group->maybeGetProperty(id);
        if (!types)
            return true;

        if (!types->hasType(type)) {
            TypeFailure(cx, "Missing type in object %s %s: %s",
                        TypeSet::ObjectGroupString(group), TypeIdString(id),
                        TypeSet::TypeString(type));
        }
    }
    return true;
}

#endif

void
js::TypeFailure(JSContext* cx, const char* fmt, ...)
{
    char msgbuf[1024]; /* Larger error messages will be truncated */
    char errbuf[1024];

    va_list ap;
    va_start(ap, fmt);
    JS_vsnprintf(errbuf, sizeof(errbuf), fmt, ap);
    va_end(ap);

    JS_snprintf(msgbuf, sizeof(msgbuf), "[infer failure] %s", errbuf);

    /* Dump type state, even if INFERFLAGS is unset. */
    PrintTypes(cx, cx->compartment(), true);

    MOZ_ReportAssertionFailure(msgbuf, __FILE__, __LINE__);
    MOZ_CRASH();
}

/////////////////////////////////////////////////////////////////////
// TypeSet
/////////////////////////////////////////////////////////////////////

TemporaryTypeSet::TemporaryTypeSet(LifoAlloc* alloc, Type type)
{
    if (type.isUnknown()) {
        flags |= TYPE_FLAG_BASE_MASK;
    } else if (type.isPrimitive()) {
        flags = PrimitiveTypeFlag(type.primitive());
        if (flags == TYPE_FLAG_DOUBLE)
            flags |= TYPE_FLAG_INT32;
    } else if (type.isAnyObject()) {
        flags |= TYPE_FLAG_ANYOBJECT;
    } else  if (type.isGroup() && type.group()->unknownProperties()) {
        flags |= TYPE_FLAG_ANYOBJECT;
    } else {
        setBaseObjectCount(1);
        objectSet = reinterpret_cast<ObjectKey**>(type.objectKey());

        if (type.isGroup()) {
            ObjectGroup* ngroup = type.group();
            if (ngroup->newScript() && ngroup->newScript()->initializedGroup())
                addType(ObjectType(ngroup->newScript()->initializedGroup()), alloc);
        }
    }
}

bool
TypeSet::mightBeMIRType(jit::MIRType type)
{
    if (unknown())
        return true;

    if (type == jit::MIRType_Object)
        return unknownObject() || baseObjectCount() != 0;

    switch (type) {
      case jit::MIRType_Undefined:
        return baseFlags() & TYPE_FLAG_UNDEFINED;
      case jit::MIRType_Null:
        return baseFlags() & TYPE_FLAG_NULL;
      case jit::MIRType_Boolean:
        return baseFlags() & TYPE_FLAG_BOOLEAN;
      case jit::MIRType_Int32:
        return baseFlags() & TYPE_FLAG_INT32;
      case jit::MIRType_Float32: // Fall through, there's no JSVAL for Float32.
      case jit::MIRType_Double:
        return baseFlags() & TYPE_FLAG_DOUBLE;
      case jit::MIRType_String:
        return baseFlags() & TYPE_FLAG_STRING;
      case jit::MIRType_Symbol:
        return baseFlags() & TYPE_FLAG_SYMBOL;
      case jit::MIRType_MagicOptimizedArguments:
        return baseFlags() & TYPE_FLAG_LAZYARGS;
      case jit::MIRType_MagicHole:
      case jit::MIRType_MagicIsConstructing:
        // These magic constants do not escape to script and are not observed
        // in the type sets.
        //
        // The reason we can return false here is subtle: if Ion is asking the
        // type set if it has seen such a magic constant, then the MIR in
        // question is the most generic type, MIRType_Value. A magic constant
        // could only be emitted by a MIR of MIRType_Value if that MIR is a
        // phi, and we check that different magic constants do not flow to the
        // same join point in GuessPhiType.
        return false;
      default:
        MOZ_CRASH("Bad MIR type");
    }
}

bool
TypeSet::objectsAreSubset(TypeSet* other)
{
    if (other->unknownObject())
        return true;

    if (unknownObject())
        return false;

    for (unsigned i = 0; i < getObjectCount(); i++) {
        ObjectKey* key = getObject(i);
        if (!key)
            continue;
        if (!other->hasType(ObjectType(key)))
            return false;
    }

    return true;
}

bool
TypeSet::isSubset(const TypeSet* other) const
{
    if ((baseFlags() & other->baseFlags()) != baseFlags())
        return false;

    if (unknownObject()) {
        MOZ_ASSERT(other->unknownObject());
    } else {
        for (unsigned i = 0; i < getObjectCount(); i++) {
            ObjectKey* key = getObject(i);
            if (!key)
                continue;
            if (!other->hasType(ObjectType(key)))
                return false;
        }
    }

    return true;
}

bool
TypeSet::enumerateTypes(TypeList* list) const
{
    /* If any type is possible, there's no need to worry about specifics. */
    if (flags & TYPE_FLAG_UNKNOWN)
        return list->append(UnknownType());

    /* Enqueue type set members stored as bits. */
    for (TypeFlags flag = 1; flag < TYPE_FLAG_ANYOBJECT; flag <<= 1) {
        if (flags & flag) {
            Type type = PrimitiveType(TypeFlagPrimitive(flag));
            if (!list->append(type))
                return false;
        }
    }

    /* If any object is possible, skip specifics. */
    if (flags & TYPE_FLAG_ANYOBJECT)
        return list->append(AnyObjectType());

    /* Enqueue specific object types. */
    unsigned count = getObjectCount();
    for (unsigned i = 0; i < count; i++) {
        ObjectKey* key = getObject(i);
        if (key) {
            if (!list->append(ObjectType(key)))
                return false;
        }
    }

    return true;
}

inline bool
TypeSet::addTypesToConstraint(JSContext* cx, TypeConstraint* constraint)
{
    /*
     * Build all types in the set into a vector before triggering the
     * constraint, as doing so may modify this type set.
     */
    TypeList types;
    if (!enumerateTypes(&types))
        return false;

    for (unsigned i = 0; i < types.length(); i++)
        constraint->newType(cx, this, types[i]);

    return true;
}

bool
ConstraintTypeSet::addConstraint(JSContext* cx, TypeConstraint* constraint, bool callExisting)
{
    if (!constraint) {
        /* OOM failure while constructing the constraint. */
        return false;
    }

    MOZ_ASSERT(cx->zone()->types.activeAnalysis);

    InferSpew(ISpewOps, "addConstraint: %sT%p%s %sC%p%s %s",
              InferSpewColor(this), this, InferSpewColorReset(),
              InferSpewColor(constraint), constraint, InferSpewColorReset(),
              constraint->kind());

    MOZ_ASSERT(constraint->next == nullptr);
    constraint->next = constraintList;
    constraintList = constraint;

    if (callExisting)
        return addTypesToConstraint(cx, constraint);
    return true;
}

void
TypeSet::clearObjects()
{
    setBaseObjectCount(0);
    objectSet = nullptr;
}

void
TypeSet::addType(Type type, LifoAlloc* alloc)
{
    if (unknown())
        return;

    if (type.isUnknown()) {
        flags |= TYPE_FLAG_BASE_MASK;
        clearObjects();
        MOZ_ASSERT(unknown());
        return;
    }

    if (type.isPrimitive()) {
        TypeFlags flag = PrimitiveTypeFlag(type.primitive());
        if (flags & flag)
            return;

        /* If we add float to a type set it is also considered to contain int. */
        if (flag == TYPE_FLAG_DOUBLE)
            flag |= TYPE_FLAG_INT32;

        flags |= flag;
        return;
    }

    if (flags & TYPE_FLAG_ANYOBJECT)
        return;
    if (type.isAnyObject())
        goto unknownObject;

    {
        uint32_t objectCount = baseObjectCount();
        ObjectKey* key = type.objectKey();
        ObjectKey** pentry = TypeHashSet::Insert<ObjectKey*, ObjectKey, ObjectKey>
                                 (*alloc, objectSet, objectCount, key);
        if (!pentry)
            goto unknownObject;
        if (*pentry)
            return;
        *pentry = key;

        setBaseObjectCount(objectCount);

        // Limit the number of objects we track. There is a different limit
        // depending on whether the set only contains DOM objects, which can
        // have many different classes and prototypes but are still optimizable
        // by IonMonkey.
        if (objectCount >= TYPE_FLAG_OBJECT_COUNT_LIMIT) {
            JS_STATIC_ASSERT(TYPE_FLAG_DOMOBJECT_COUNT_LIMIT >= TYPE_FLAG_OBJECT_COUNT_LIMIT);
            // Examining the entire type set is only required when we first hit
            // the normal object limit.
            if (objectCount == TYPE_FLAG_OBJECT_COUNT_LIMIT) {
                for (unsigned i = 0; i < objectCount; i++) {
                    const Class* clasp = getObjectClass(i);
                    if (clasp && !clasp->isDOMClass())
                        goto unknownObject;
                }
            }

            // Make sure the newly added object is also a DOM object.
            if (!key->clasp()->isDOMClass())
                goto unknownObject;

            // Limit the number of DOM objects.
            if (objectCount == TYPE_FLAG_DOMOBJECT_COUNT_LIMIT)
                goto unknownObject;
        }
    }

    if (type.isGroup()) {
        ObjectGroup* ngroup = type.group();
        MOZ_ASSERT(!ngroup->singleton());
        if (ngroup->unknownProperties())
            goto unknownObject;

        // If we add a partially initialized group to a type set, add the
        // corresponding fully initialized group, as an object's group may change
        // from the former to the latter via the acquired properties analysis.
        if (ngroup->newScript() && ngroup->newScript()->initializedGroup())
            addType(ObjectType(ngroup->newScript()->initializedGroup()), alloc);
    }

    if (false) {
    unknownObject:
        flags |= TYPE_FLAG_ANYOBJECT;
        clearObjects();
    }
}

void
ConstraintTypeSet::addType(ExclusiveContext* cxArg, Type type)
{
    MOZ_ASSERT(cxArg->zone()->types.activeAnalysis);

    if (hasType(type))
        return;

    TypeSet::addType(type, &cxArg->typeLifoAlloc());

    if (type.isObjectUnchecked() && unknownObject())
        type = AnyObjectType();

    InferSpew(ISpewOps, "addType: %sT%p%s %s",
              InferSpewColor(this), this, InferSpewColorReset(),
              TypeString(type));

    /* Propagate the type to all constraints. */
    if (JSContext* cx = cxArg->maybeJSContext()) {
        TypeConstraint* constraint = constraintList;
        while (constraint) {
            constraint->newType(cx, this, type);
            constraint = constraint->next;
        }
    } else {
        MOZ_ASSERT(!constraintList);
    }
}

void
TypeSet::print()
{
    if (flags & TYPE_FLAG_NON_DATA_PROPERTY)
        fprintf(stderr, " [non-data]");

    if (flags & TYPE_FLAG_NON_WRITABLE_PROPERTY)
        fprintf(stderr, " [non-writable]");

    if (definiteProperty())
        fprintf(stderr, " [definite:%d]", definiteSlot());

    if (baseFlags() == 0 && !baseObjectCount()) {
        fprintf(stderr, " missing");
        return;
    }

    if (flags & TYPE_FLAG_UNKNOWN)
        fprintf(stderr, " unknown");
    if (flags & TYPE_FLAG_ANYOBJECT)
        fprintf(stderr, " object");

    if (flags & TYPE_FLAG_UNDEFINED)
        fprintf(stderr, " void");
    if (flags & TYPE_FLAG_NULL)
        fprintf(stderr, " null");
    if (flags & TYPE_FLAG_BOOLEAN)
        fprintf(stderr, " bool");
    if (flags & TYPE_FLAG_INT32)
        fprintf(stderr, " int");
    if (flags & TYPE_FLAG_DOUBLE)
        fprintf(stderr, " float");
    if (flags & TYPE_FLAG_STRING)
        fprintf(stderr, " string");
    if (flags & TYPE_FLAG_SYMBOL)
        fprintf(stderr, " symbol");
    if (flags & TYPE_FLAG_LAZYARGS)
        fprintf(stderr, " lazyargs");

    uint32_t objectCount = baseObjectCount();
    if (objectCount) {
        fprintf(stderr, " object[%u]", objectCount);

        unsigned count = getObjectCount();
        for (unsigned i = 0; i < count; i++) {
            ObjectKey* key = getObject(i);
            if (key)
                fprintf(stderr, " %s", TypeString(ObjectType(key)));
        }
    }
}

/* static */ void
TypeSet::readBarrier(const TypeSet* types)
{
    if (types->unknownObject())
        return;

    for (unsigned i = 0; i < types->getObjectCount(); i++) {
        if (ObjectKey* key = types->getObject(i)) {
            if (key->isSingleton())
                (void) key->singleton();
            else
                (void) key->group();
        }
    }
}

bool
TypeSet::clone(LifoAlloc* alloc, TemporaryTypeSet* result) const
{
    MOZ_ASSERT(result->empty());

    unsigned objectCount = baseObjectCount();
    unsigned capacity = (objectCount >= 2) ? TypeHashSet::Capacity(objectCount) : 0;

    ObjectKey** newSet;
    if (capacity) {
        newSet = alloc->newArray<ObjectKey*>(capacity);
        if (!newSet)
            return false;
        PodCopy(newSet, objectSet, capacity);
    }

    new(result) TemporaryTypeSet(flags, capacity ? newSet : objectSet);
    return true;
}

TemporaryTypeSet*
TypeSet::clone(LifoAlloc* alloc) const
{
    TemporaryTypeSet* res = alloc->new_<TemporaryTypeSet>();
    if (!res || !clone(alloc, res))
        return nullptr;
    return res;
}

TemporaryTypeSet*
TypeSet::filter(LifoAlloc* alloc, bool filterUndefined, bool filterNull) const
{
    TemporaryTypeSet* res = clone(alloc);
    if (!res)
        return nullptr;

    if (filterUndefined)
        res->flags = res->flags & ~TYPE_FLAG_UNDEFINED;

    if (filterNull)
        res->flags = res->flags & ~TYPE_FLAG_NULL;

    return res;
}

TemporaryTypeSet*
TypeSet::cloneObjectsOnly(LifoAlloc* alloc)
{
    TemporaryTypeSet* res = clone(alloc);
    if (!res)
        return nullptr;

    res->flags &= ~TYPE_FLAG_BASE_MASK | TYPE_FLAG_ANYOBJECT;

    return res;
}

TemporaryTypeSet*
TypeSet::cloneWithoutObjects(LifoAlloc* alloc)
{
    TemporaryTypeSet* res = alloc->new_<TemporaryTypeSet>();
    if (!res)
        return nullptr;

    res->flags = flags & ~TYPE_FLAG_ANYOBJECT;
    res->setBaseObjectCount(0);
    return res;
}

/* static */ TemporaryTypeSet*
TypeSet::unionSets(TypeSet* a, TypeSet* b, LifoAlloc* alloc)
{
    TemporaryTypeSet* res = alloc->new_<TemporaryTypeSet>(a->baseFlags() | b->baseFlags(),
                                                          static_cast<ObjectKey**>(nullptr));
    if (!res)
        return nullptr;

    if (!res->unknownObject()) {
        for (size_t i = 0; i < a->getObjectCount() && !res->unknownObject(); i++) {
            if (ObjectKey* key = a->getObject(i))
                res->addType(ObjectType(key), alloc);
        }
        for (size_t i = 0; i < b->getObjectCount() && !res->unknownObject(); i++) {
            if (ObjectKey* key = b->getObject(i))
                res->addType(ObjectType(key), alloc);
        }
    }

    return res;
}

/* static */ TemporaryTypeSet*
TypeSet::intersectSets(TemporaryTypeSet* a, TemporaryTypeSet* b, LifoAlloc* alloc)
{
    TemporaryTypeSet* res;
    res = alloc->new_<TemporaryTypeSet>(a->baseFlags() & b->baseFlags(),
                                        static_cast<ObjectKey**>(nullptr));
    if (!res)
        return nullptr;

    res->setBaseObjectCount(0);
    if (res->unknownObject())
        return res;

    MOZ_ASSERT(!a->unknownObject() || !b->unknownObject());

    if (a->unknownObject()) {
        for (size_t i = 0; i < b->getObjectCount(); i++) {
            if (b->getObject(i))
                res->addType(ObjectType(b->getObject(i)), alloc);
        }
        return res;
    }

    if (b->unknownObject()) {
        for (size_t i = 0; i < a->getObjectCount(); i++) {
            if (b->getObject(i))
                res->addType(ObjectType(a->getObject(i)), alloc);
        }
        return res;
    }

    MOZ_ASSERT(!a->unknownObject() && !b->unknownObject());

    for (size_t i = 0; i < a->getObjectCount(); i++) {
        for (size_t j = 0; j < b->getObjectCount(); j++) {
            if (b->getObject(j) != a->getObject(i))
                continue;
            if (!b->getObject(j))
                continue;
            res->addType(ObjectType(b->getObject(j)), alloc);
            break;
        }
    }

    return res;
}

/////////////////////////////////////////////////////////////////////
// Compiler constraints
/////////////////////////////////////////////////////////////////////

// Compiler constraints overview
//
// Constraints generated during Ion compilation capture assumptions made about
// heap properties that will trigger invalidation of the resulting Ion code if
// the constraint is violated. Constraints can only be attached to type sets on
// the main thread, so to allow compilation to occur almost entirely off thread
// the generation is split into two phases.
//
// During compilation, CompilerConstraint values are constructed in a list,
// recording the heap property type set which was read from and its expected
// contents, along with the assumption made about those contents.
//
// At the end of compilation, when linking the result on the main thread, the
// list of compiler constraints are read and converted to type constraints and
// attached to the type sets. If the property type sets have changed so that the
// assumptions no longer hold then the compilation is aborted and its result
// discarded.

// Superclass of all constraints generated during Ion compilation. These may
// be allocated off the main thread, using the current JIT context's allocator.
class CompilerConstraint
{
  public:
    // Property being queried by the compiler.
    HeapTypeSetKey property;

    // Contents of the property at the point when the query was performed. This
    // may differ from the actual property types later in compilation as the
    // main thread performs side effects.
    TemporaryTypeSet* expected;

    CompilerConstraint(LifoAlloc* alloc, const HeapTypeSetKey& property)
      : property(property),
        expected(property.maybeTypes() ? property.maybeTypes()->clone(alloc) : nullptr)
    {}

    // Generate the type constraint recording the assumption made by this
    // compilation. Returns true if the assumption originally made still holds.
    virtual bool generateTypeConstraint(JSContext* cx, RecompileInfo recompileInfo) = 0;
};

class js::CompilerConstraintList
{
  public:
    struct FrozenScript
    {
        JSScript* script;
        TemporaryTypeSet* thisTypes;
        TemporaryTypeSet* argTypes;
        TemporaryTypeSet* bytecodeTypes;
    };

  private:

    // OOM during generation of some constraint.
    bool failed_;

    // Allocator used for constraints.
    LifoAlloc* alloc_;

    // Constraints generated on heap properties.
    Vector<CompilerConstraint*, 0, jit::JitAllocPolicy> constraints;

    // Scripts whose stack type sets were frozen for the compilation.
    Vector<FrozenScript, 1, jit::JitAllocPolicy> frozenScripts;

  public:
    explicit CompilerConstraintList(jit::TempAllocator& alloc)
      : failed_(false),
        alloc_(alloc.lifoAlloc()),
        constraints(alloc),
        frozenScripts(alloc)
    {}

    void add(CompilerConstraint* constraint) {
        if (!constraint || !constraints.append(constraint))
            setFailed();
    }

    void freezeScript(JSScript* script,
                      TemporaryTypeSet* thisTypes,
                      TemporaryTypeSet* argTypes,
                      TemporaryTypeSet* bytecodeTypes)
    {
        FrozenScript entry;
        entry.script = script;
        entry.thisTypes = thisTypes;
        entry.argTypes = argTypes;
        entry.bytecodeTypes = bytecodeTypes;
        if (!frozenScripts.append(entry))
            setFailed();
    }

    size_t length() {
        return constraints.length();
    }

    CompilerConstraint* get(size_t i) {
        return constraints[i];
    }

    size_t numFrozenScripts() {
        return frozenScripts.length();
    }

    const FrozenScript& frozenScript(size_t i) {
        return frozenScripts[i];
    }

    bool failed() {
        return failed_;
    }
    void setFailed() {
        failed_ = true;
    }
    LifoAlloc* alloc() const {
        return alloc_;
    }
};

CompilerConstraintList*
js::NewCompilerConstraintList(jit::TempAllocator& alloc)
{
    return alloc.lifoAlloc()->new_<CompilerConstraintList>(alloc);
}

/* static */ bool
TypeScript::FreezeTypeSets(CompilerConstraintList* constraints, JSScript* script,
                           TemporaryTypeSet** pThisTypes,
                           TemporaryTypeSet** pArgTypes,
                           TemporaryTypeSet** pBytecodeTypes)
{
    LifoAlloc* alloc = constraints->alloc();
    StackTypeSet* existing = script->types()->typeArray();

    size_t count = NumTypeSets(script);
    TemporaryTypeSet* types = alloc->newArrayUninitialized<TemporaryTypeSet>(count);
    if (!types)
        return false;
    PodZero(types, count);

    for (size_t i = 0; i < count; i++) {
        if (!existing[i].clone(alloc, &types[i]))
            return false;
    }

    *pThisTypes = types + (ThisTypes(script) - existing);
    *pArgTypes = (script->functionNonDelazifying() && script->functionNonDelazifying()->nargs())
                 ? (types + (ArgTypes(script, 0) - existing))
                 : nullptr;
    *pBytecodeTypes = types;

    constraints->freezeScript(script, *pThisTypes, *pArgTypes, *pBytecodeTypes);
    return true;
}

namespace {

template <typename T>
class CompilerConstraintInstance : public CompilerConstraint
{
    T data;

  public:
    CompilerConstraintInstance<T>(LifoAlloc* alloc, const HeapTypeSetKey& property, const T& data)
      : CompilerConstraint(alloc, property), data(data)
    {}

    bool generateTypeConstraint(JSContext* cx, RecompileInfo recompileInfo);
};

// Constraint generated from a CompilerConstraint when linking the compilation.
template <typename T>
class TypeCompilerConstraint : public TypeConstraint
{
    // Compilation which this constraint may invalidate.
    RecompileInfo compilation;

    T data;

  public:
    TypeCompilerConstraint<T>(RecompileInfo compilation, const T& data)
      : compilation(compilation), data(data)
    {}

    const char* kind() { return data.kind(); }

    void newType(JSContext* cx, TypeSet* source, TypeSet::Type type) {
        if (data.invalidateOnNewType(type))
            cx->zone()->types.addPendingRecompile(cx, compilation);
    }

    void newPropertyState(JSContext* cx, TypeSet* source) {
        if (data.invalidateOnNewPropertyState(source))
            cx->zone()->types.addPendingRecompile(cx, compilation);
    }

    void newObjectState(JSContext* cx, ObjectGroup* group) {
        // Note: Once the object has unknown properties, no more notifications
        // will be sent on changes to its state, so always invalidate any
        // associated compilations.
        if (group->unknownProperties() || data.invalidateOnNewObjectState(group))
            cx->zone()->types.addPendingRecompile(cx, compilation);
    }

    bool sweep(TypeZone& zone, TypeConstraint** res) {
        if (data.shouldSweep() || compilation.shouldSweep(zone))
            return false;
        *res = zone.typeLifoAlloc.new_<TypeCompilerConstraint<T> >(compilation, data);
        return true;
    }
};

template <typename T>
bool
CompilerConstraintInstance<T>::generateTypeConstraint(JSContext* cx, RecompileInfo recompileInfo)
{
    if (property.object()->unknownProperties())
        return false;

    if (!property.instantiate(cx))
        return false;

    if (!data.constraintHolds(cx, property, expected))
        return false;

    return property.maybeTypes()->addConstraint(cx, cx->typeLifoAlloc().new_<TypeCompilerConstraint<T> >(recompileInfo, data),
                                                /* callExisting = */ false);
}

} /* anonymous namespace */

const Class*
TypeSet::ObjectKey::clasp()
{
    return isGroup() ? group()->clasp() : singleton()->getClass();
}

TaggedProto
TypeSet::ObjectKey::proto()
{
    return isGroup() ? group()->proto() : singleton()->getTaggedProto();
}

TypeNewScript*
TypeSet::ObjectKey::newScript()
{
    if (isGroup() && group()->newScript())
        return group()->newScript();
    return nullptr;
}

ObjectGroup*
TypeSet::ObjectKey::maybeGroup()
{
    if (isGroup())
        return group();
    if (!singleton()->hasLazyGroup())
        return singleton()->group();
    return nullptr;
}

bool
TypeSet::ObjectKey::unknownProperties()
{
    if (ObjectGroup* group = maybeGroup())
        return group->unknownProperties();
    return false;
}

HeapTypeSetKey
TypeSet::ObjectKey::property(jsid id)
{
    MOZ_ASSERT(!unknownProperties());

    HeapTypeSetKey property;
    property.object_ = this;
    property.id_ = id;
    if (ObjectGroup* group = maybeGroup())
        property.maybeTypes_ = group->maybeGetProperty(id);

    return property;
}

void
TypeSet::ObjectKey::ensureTrackedProperty(JSContext* cx, jsid id)
{
    // If we are accessing a lazily defined property which actually exists in
    // the VM and has not been instantiated yet, instantiate it now if we are
    // on the main thread and able to do so.
    if (!JSID_IS_VOID(id) && !JSID_IS_EMPTY(id)) {
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
        if (isSingleton()) {
            JSObject* obj = singleton();
            if (obj->isNative() && obj->as<NativeObject>().containsPure(id))
                EnsureTrackPropertyTypes(cx, obj, id);
        }
    }
}

bool
HeapTypeSetKey::instantiate(JSContext* cx)
{
    if (maybeTypes())
        return true;
    if (object()->isSingleton() && !object()->singleton()->getGroup(cx)) {
        cx->clearPendingException();
        return false;
    }
    maybeTypes_ = object()->maybeGroup()->getProperty(cx, id());
    return maybeTypes_ != nullptr;
}

static bool
CheckFrozenTypeSet(JSContext* cx, TemporaryTypeSet* frozen, StackTypeSet* actual)
{
    // Return whether the types frozen for a script during compilation are
    // still valid. Also check for any new types added to the frozen set during
    // compilation, and add them to the actual stack type sets. These new types
    // indicate places where the compiler relaxed its possible inputs to be
    // more tolerant of potential new types.

    if (!actual->isSubset(frozen))
        return false;

    if (!frozen->isSubset(actual)) {
        TypeSet::TypeList list;
        frozen->enumerateTypes(&list);

        for (size_t i = 0; i < list.length(); i++)
            actual->addType(cx, list[i]);
    }

    return true;
}

namespace {

/*
 * As for TypeConstraintFreeze, but describes an implicit freeze constraint
 * added for stack types within a script. Applies to all compilations of the
 * script, not just a single one.
 */
class TypeConstraintFreezeStack : public TypeConstraint
{
    JSScript* script_;

  public:
    explicit TypeConstraintFreezeStack(JSScript* script)
        : script_(script)
    {}

    const char* kind() { return "freezeStack"; }

    void newType(JSContext* cx, TypeSet* source, TypeSet::Type type) {
        /*
         * Unlike TypeConstraintFreeze, triggering this constraint once does
         * not disable it on future changes to the type set.
         */
        cx->zone()->types.addPendingRecompile(cx, script_);
    }

    bool sweep(TypeZone& zone, TypeConstraint** res) {
        if (IsScriptAboutToBeFinalized(&script_))
            return false;
        *res = zone.typeLifoAlloc.new_<TypeConstraintFreezeStack>(script_);
        return true;
    }
};

} /* anonymous namespace */

bool
js::FinishCompilation(JSContext* cx, HandleScript script, CompilerConstraintList* constraints,
                         RecompileInfo* precompileInfo)
{
    if (constraints->failed())
        return false;

    CompilerOutput co(script);

    TypeZone& types = cx->zone()->types;
    if (!types.compilerOutputs) {
        types.compilerOutputs = cx->new_<TypeZone::CompilerOutputVector>();
        if (!types.compilerOutputs)
            return false;
    }

#ifdef DEBUG
    for (size_t i = 0; i < types.compilerOutputs->length(); i++) {
        const CompilerOutput& co = (*types.compilerOutputs)[i];
        MOZ_ASSERT_IF(co.isValid(), co.script() != script);
    }
#endif

    uint32_t index = types.compilerOutputs->length();
    if (!types.compilerOutputs->append(co)) {
        js_ReportOutOfMemory(cx);
        return false;
    }

    *precompileInfo = RecompileInfo(index, types.generation);

    bool succeeded = true;

    for (size_t i = 0; i < constraints->length(); i++) {
        CompilerConstraint* constraint = constraints->get(i);
        if (!constraint->generateTypeConstraint(cx, *precompileInfo))
            succeeded = false;
    }

    for (size_t i = 0; i < constraints->numFrozenScripts(); i++) {
        const CompilerConstraintList::FrozenScript& entry = constraints->frozenScript(i);
        if (!entry.script->types()) {
            succeeded = false;
            break;
        }

        // It could happen that one of the compiled scripts was made a
        // debuggee mid-compilation (e.g., via setting a breakpoint). If so,
        // throw away the compilation.
        if (entry.script->isDebuggee()) {
            succeeded = false;
            break;
        }

        if (!CheckFrozenTypeSet(cx, entry.thisTypes, TypeScript::ThisTypes(entry.script)))
            succeeded = false;
        unsigned nargs = entry.script->functionNonDelazifying()
                         ? entry.script->functionNonDelazifying()->nargs()
                         : 0;
        for (size_t i = 0; i < nargs; i++) {
            if (!CheckFrozenTypeSet(cx, &entry.argTypes[i], TypeScript::ArgTypes(entry.script, i)))
                succeeded = false;
        }
        for (size_t i = 0; i < entry.script->nTypeSets(); i++) {
            if (!CheckFrozenTypeSet(cx, &entry.bytecodeTypes[i], &entry.script->types()->typeArray()[i]))
                succeeded = false;
        }

        // If necessary, add constraints to trigger invalidation on the script
        // after any future changes to the stack type sets.
        if (entry.script->hasFreezeConstraints())
            continue;
        entry.script->setHasFreezeConstraints();

        size_t count = TypeScript::NumTypeSets(entry.script);

        StackTypeSet* array = entry.script->types()->typeArray();
        for (size_t i = 0; i < count; i++) {
            if (!array[i].addConstraint(cx, cx->typeLifoAlloc().new_<TypeConstraintFreezeStack>(entry.script), false))
                succeeded = false;
        }
    }

    if (!succeeded || types.compilerOutputs->back().pendingInvalidation()) {
        types.compilerOutputs->back().invalidate();
        script->resetWarmUpCounter();
        return false;
    }

    return true;
}

static void
CheckDefinitePropertiesTypeSet(JSContext* cx, TemporaryTypeSet* frozen, StackTypeSet* actual)
{
    // The definite properties analysis happens on the main thread, so no new
    // types can have been added to actual. The analysis may have updated the
    // contents of |frozen| though with new speculative types, and these need
    // to be reflected in |actual| for AddClearDefiniteFunctionUsesInScript
    // to work.
    if (!frozen->isSubset(actual)) {
        TypeSet::TypeList list;
        frozen->enumerateTypes(&list);

        for (size_t i = 0; i < list.length(); i++)
            actual->addType(cx, list[i]);
    }
}

void
js::FinishDefinitePropertiesAnalysis(JSContext* cx, CompilerConstraintList* constraints)
{
#ifdef DEBUG
    // Assert no new types have been added to the StackTypeSets. Do this before
    // calling CheckDefinitePropertiesTypeSet, as it may add new types to the
    // StackTypeSets and break these invariants if a script is inlined more
    // than once. See also CheckDefinitePropertiesTypeSet.
    for (size_t i = 0; i < constraints->numFrozenScripts(); i++) {
        const CompilerConstraintList::FrozenScript& entry = constraints->frozenScript(i);
        JSScript* script = entry.script;
        MOZ_ASSERT(script->types());

        MOZ_ASSERT(TypeScript::ThisTypes(script)->isSubset(entry.thisTypes));

        unsigned nargs = entry.script->functionNonDelazifying()
                         ? entry.script->functionNonDelazifying()->nargs()
                         : 0;
        for (size_t j = 0; j < nargs; j++)
            MOZ_ASSERT(TypeScript::ArgTypes(script, j)->isSubset(&entry.argTypes[j]));

        for (size_t j = 0; j < script->nTypeSets(); j++)
            MOZ_ASSERT(script->types()->typeArray()[j].isSubset(&entry.bytecodeTypes[j]));
    }
#endif

    for (size_t i = 0; i < constraints->numFrozenScripts(); i++) {
        const CompilerConstraintList::FrozenScript& entry = constraints->frozenScript(i);
        JSScript* script = entry.script;
        if (!script->types())
            MOZ_CRASH();

        CheckDefinitePropertiesTypeSet(cx, entry.thisTypes, TypeScript::ThisTypes(script));

        unsigned nargs = script->functionNonDelazifying()
                         ? script->functionNonDelazifying()->nargs()
                         : 0;
        for (size_t j = 0; j < nargs; j++)
            CheckDefinitePropertiesTypeSet(cx, &entry.argTypes[j], TypeScript::ArgTypes(script, j));

        for (size_t j = 0; j < script->nTypeSets(); j++)
            CheckDefinitePropertiesTypeSet(cx, &entry.bytecodeTypes[j], &script->types()->typeArray()[j]);
    }
}

namespace {

// Constraint which triggers recompilation of a script if any type is added to a type set. */
class ConstraintDataFreeze
{
  public:
    ConstraintDataFreeze() {}

    const char* kind() { return "freeze"; }

    bool invalidateOnNewType(TypeSet::Type type) { return true; }
    bool invalidateOnNewPropertyState(TypeSet* property) { return true; }
    bool invalidateOnNewObjectState(ObjectGroup* group) { return false; }

    bool constraintHolds(JSContext* cx,
                         const HeapTypeSetKey& property, TemporaryTypeSet* expected)
    {
        return expected
               ? property.maybeTypes()->isSubset(expected)
               : property.maybeTypes()->empty();
    }

    bool shouldSweep() { return false; }
};

} /* anonymous namespace */

void
HeapTypeSetKey::freeze(CompilerConstraintList* constraints)
{
    LifoAlloc* alloc = constraints->alloc();

    typedef CompilerConstraintInstance<ConstraintDataFreeze> T;
    constraints->add(alloc->new_<T>(alloc, *this, ConstraintDataFreeze()));
}

static inline jit::MIRType
GetMIRTypeFromTypeFlags(TypeFlags flags)
{
    switch (flags) {
      case TYPE_FLAG_UNDEFINED:
        return jit::MIRType_Undefined;
      case TYPE_FLAG_NULL:
        return jit::MIRType_Null;
      case TYPE_FLAG_BOOLEAN:
        return jit::MIRType_Boolean;
      case TYPE_FLAG_INT32:
        return jit::MIRType_Int32;
      case (TYPE_FLAG_INT32 | TYPE_FLAG_DOUBLE):
        return jit::MIRType_Double;
      case TYPE_FLAG_STRING:
        return jit::MIRType_String;
      case TYPE_FLAG_SYMBOL:
        return jit::MIRType_Symbol;
      case TYPE_FLAG_LAZYARGS:
        return jit::MIRType_MagicOptimizedArguments;
      case TYPE_FLAG_ANYOBJECT:
        return jit::MIRType_Object;
      default:
        return jit::MIRType_Value;
    }
}

jit::MIRType
TemporaryTypeSet::getKnownMIRType()
{
    TypeFlags flags = baseFlags();
    jit::MIRType type;

    if (baseObjectCount())
        type = flags ? jit::MIRType_Value : jit::MIRType_Object;
    else
        type = GetMIRTypeFromTypeFlags(flags);

    /*
     * If the type set is totally empty then it will be treated as unknown,
     * but we still need to record the dependency as adding a new type can give
     * it a definite type tag. This is not needed if there are enough types
     * that the exact tag is unknown, as it will stay unknown as more types are
     * added to the set.
     */
    DebugOnly<bool> empty = flags == 0 && baseObjectCount() == 0;
    MOZ_ASSERT_IF(empty, type == jit::MIRType_Value);

    return type;
}

jit::MIRType
HeapTypeSetKey::knownMIRType(CompilerConstraintList* constraints)
{
    TypeSet* types = maybeTypes();

    if (!types || types->unknown())
        return jit::MIRType_Value;

    TypeFlags flags = types->baseFlags() & ~TYPE_FLAG_ANYOBJECT;
    jit::MIRType type;

    if (types->unknownObject() || types->getObjectCount())
        type = flags ? jit::MIRType_Value : jit::MIRType_Object;
    else
        type = GetMIRTypeFromTypeFlags(flags);

    if (type != jit::MIRType_Value)
        freeze(constraints);

    /*
     * If the type set is totally empty then it will be treated as unknown,
     * but we still need to record the dependency as adding a new type can give
     * it a definite type tag. This is not needed if there are enough types
     * that the exact tag is unknown, as it will stay unknown as more types are
     * added to the set.
     */
    MOZ_ASSERT_IF(types->empty(), type == jit::MIRType_Value);

    return type;
}

bool
HeapTypeSetKey::isOwnProperty(CompilerConstraintList* constraints,
                              bool allowEmptyTypesForGlobal/* = false*/)
{
    if (maybeTypes() && (!maybeTypes()->empty() || maybeTypes()->nonDataProperty()))
        return true;
    if (object()->isSingleton()) {
        JSObject* obj = object()->singleton();
        MOZ_ASSERT(CanHaveEmptyPropertyTypesForOwnProperty(obj) == obj->is<GlobalObject>());
        if (!allowEmptyTypesForGlobal) {
            if (CanHaveEmptyPropertyTypesForOwnProperty(obj))
                return true;
        }
    }
    freeze(constraints);
    return false;
}

bool
HeapTypeSetKey::knownSubset(CompilerConstraintList* constraints, const HeapTypeSetKey& other)
{
    if (!maybeTypes() || maybeTypes()->empty()) {
        freeze(constraints);
        return true;
    }
    if (!other.maybeTypes() || !maybeTypes()->isSubset(other.maybeTypes()))
        return false;
    freeze(constraints);
    return true;
}

JSObject*
TemporaryTypeSet::maybeSingleton()
{
    if (baseFlags() != 0 || baseObjectCount() != 1)
        return nullptr;

    return getSingleton(0);
}

JSObject*
HeapTypeSetKey::singleton(CompilerConstraintList* constraints)
{
    HeapTypeSet* types = maybeTypes();

    if (!types || types->nonDataProperty() || types->baseFlags() != 0 || types->getObjectCount() != 1)
        return nullptr;

    JSObject* obj = types->getSingleton(0);

    if (obj)
        freeze(constraints);

    return obj;
}

bool
HeapTypeSetKey::needsBarrier(CompilerConstraintList* constraints)
{
    TypeSet* types = maybeTypes();
    if (!types)
        return false;
    bool result = types->unknownObject()
               || types->getObjectCount() > 0
               || types->hasAnyFlag(TYPE_FLAG_STRING | TYPE_FLAG_SYMBOL);
    if (!result)
        freeze(constraints);
    return result;
}

namespace {

// Constraint which triggers recompilation if an object acquires particular flags.
class ConstraintDataFreezeObjectFlags
{
  public:
    // Flags we are watching for on this object.
    ObjectGroupFlags flags;

    explicit ConstraintDataFreezeObjectFlags(ObjectGroupFlags flags)
      : flags(flags)
    {
        MOZ_ASSERT(flags);
    }

    const char* kind() { return "freezeObjectFlags"; }

    bool invalidateOnNewType(TypeSet::Type type) { return false; }
    bool invalidateOnNewPropertyState(TypeSet* property) { return false; }
    bool invalidateOnNewObjectState(ObjectGroup* group) {
        return group->hasAnyFlags(flags);
    }

    bool constraintHolds(JSContext* cx,
                         const HeapTypeSetKey& property, TemporaryTypeSet* expected)
    {
        return !invalidateOnNewObjectState(property.object()->maybeGroup());
    }

    bool shouldSweep() { return false; }
};

} /* anonymous namespace */

bool
TypeSet::ObjectKey::hasFlags(CompilerConstraintList* constraints, ObjectGroupFlags flags)
{
    MOZ_ASSERT(flags);

    if (ObjectGroup* group = maybeGroup()) {
        if (group->hasAnyFlags(flags))
            return true;
    }

    HeapTypeSetKey objectProperty = property(JSID_EMPTY);
    LifoAlloc* alloc = constraints->alloc();

    typedef CompilerConstraintInstance<ConstraintDataFreezeObjectFlags> T;
    constraints->add(alloc->new_<T>(alloc, objectProperty, ConstraintDataFreezeObjectFlags(flags)));
    return false;
}

bool
TypeSet::ObjectKey::hasStableClassAndProto(CompilerConstraintList* constraints)
{
    return !hasFlags(constraints, OBJECT_FLAG_UNKNOWN_PROPERTIES);
}

bool
TemporaryTypeSet::hasObjectFlags(CompilerConstraintList* constraints, ObjectGroupFlags flags)
{
    if (unknownObject())
        return true;

    /*
     * Treat type sets containing no objects as having all object flags,
     * to spare callers from having to check this.
     */
    if (baseObjectCount() == 0)
        return true;

    unsigned count = getObjectCount();
    for (unsigned i = 0; i < count; i++) {
        ObjectKey* key = getObject(i);
        if (key && key->hasFlags(constraints, flags))
            return true;
    }

    return false;
}

gc::InitialHeap
ObjectGroup::initialHeap(CompilerConstraintList* constraints)
{
    // If this object is not required to be pretenured but could be in the
    // future, add a constraint to trigger recompilation if the requirement
    // changes.

    if (shouldPreTenure())
        return gc::TenuredHeap;

    if (!canPreTenure())
        return gc::DefaultHeap;

    HeapTypeSetKey objectProperty = TypeSet::ObjectKey::get(this)->property(JSID_EMPTY);
    LifoAlloc* alloc = constraints->alloc();

    typedef CompilerConstraintInstance<ConstraintDataFreezeObjectFlags> T;
    constraints->add(alloc->new_<T>(alloc, objectProperty, ConstraintDataFreezeObjectFlags(OBJECT_FLAG_PRE_TENURE)));

    return gc::DefaultHeap;
}

namespace {

// Constraint which triggers recompilation on any type change in an inlined
// script. The freeze constraints added to stack type sets will only directly
// invalidate the script containing those stack type sets. To invalidate code
// for scripts into which the base script was inlined, ObjectStateChange is used.
class ConstraintDataFreezeObjectForInlinedCall
{
  public:
    ConstraintDataFreezeObjectForInlinedCall()
    {}

    const char* kind() { return "freezeObjectForInlinedCall"; }

    bool invalidateOnNewType(TypeSet::Type type) { return false; }
    bool invalidateOnNewPropertyState(TypeSet* property) { return false; }
    bool invalidateOnNewObjectState(ObjectGroup* group) {
        // We don't keep track of the exact dependencies the caller has on its
        // inlined scripts' type sets, so always invalidate the caller.
        return true;
    }

    bool constraintHolds(JSContext* cx,
                         const HeapTypeSetKey& property, TemporaryTypeSet* expected)
    {
        return true;
    }

    bool shouldSweep() { return false; }
};

// Constraint which triggers recompilation when a typed array's data becomes
// invalid.
class ConstraintDataFreezeObjectForTypedArrayData
{
    void* viewData;
    uint32_t length;

  public:
    explicit ConstraintDataFreezeObjectForTypedArrayData(TypedArrayObject& tarray)
      : viewData(tarray.viewData()),
        length(tarray.length())
    {}

    const char* kind() { return "freezeObjectForTypedArrayData"; }

    bool invalidateOnNewType(TypeSet::Type type) { return false; }
    bool invalidateOnNewPropertyState(TypeSet* property) { return false; }
    bool invalidateOnNewObjectState(ObjectGroup* group) {
        TypedArrayObject& tarray = group->singleton()->as<TypedArrayObject>();
        return tarray.viewData() != viewData || tarray.length() != length;
    }

    bool constraintHolds(JSContext* cx,
                         const HeapTypeSetKey& property, TemporaryTypeSet* expected)
    {
        return !invalidateOnNewObjectState(property.object()->maybeGroup());
    }

    bool shouldSweep() {
        // Note: |viewData| is only used for equality testing.
        return false;
    }
};

// Constraint which triggers recompilation if an unboxed object in some group
// is converted to a native object.
class ConstraintDataFreezeObjectForUnboxedConvertedToNative
{
  public:
    ConstraintDataFreezeObjectForUnboxedConvertedToNative()
    {}

    const char* kind() { return "freezeObjectForUnboxedConvertedToNative"; }

    bool invalidateOnNewType(TypeSet::Type type) { return false; }
    bool invalidateOnNewPropertyState(TypeSet* property) { return false; }
    bool invalidateOnNewObjectState(ObjectGroup* group) {
        return group->unboxedLayout().nativeGroup() != nullptr;
    }

    bool constraintHolds(JSContext* cx,
                         const HeapTypeSetKey& property, TemporaryTypeSet* expected)
    {
        return !invalidateOnNewObjectState(property.object()->maybeGroup());
    }

    bool shouldSweep() { return false; }
};

} /* anonymous namespace */

void
TypeSet::ObjectKey::watchStateChangeForInlinedCall(CompilerConstraintList* constraints)
{
    HeapTypeSetKey objectProperty = property(JSID_EMPTY);
    LifoAlloc* alloc = constraints->alloc();

    typedef CompilerConstraintInstance<ConstraintDataFreezeObjectForInlinedCall> T;
    constraints->add(alloc->new_<T>(alloc, objectProperty, ConstraintDataFreezeObjectForInlinedCall()));
}

void
TypeSet::ObjectKey::watchStateChangeForTypedArrayData(CompilerConstraintList* constraints)
{
    TypedArrayObject& tarray = singleton()->as<TypedArrayObject>();
    HeapTypeSetKey objectProperty = property(JSID_EMPTY);
    LifoAlloc* alloc = constraints->alloc();

    typedef CompilerConstraintInstance<ConstraintDataFreezeObjectForTypedArrayData> T;
    constraints->add(alloc->new_<T>(alloc, objectProperty,
                                    ConstraintDataFreezeObjectForTypedArrayData(tarray)));
}

void
TypeSet::ObjectKey::watchStateChangeForUnboxedConvertedToNative(CompilerConstraintList* constraints)
{
    HeapTypeSetKey objectProperty = property(JSID_EMPTY);
    LifoAlloc* alloc = constraints->alloc();

    typedef CompilerConstraintInstance<ConstraintDataFreezeObjectForUnboxedConvertedToNative> T;
    constraints->add(alloc->new_<T>(alloc, objectProperty,
                                    ConstraintDataFreezeObjectForUnboxedConvertedToNative()));
}

static void
ObjectStateChange(ExclusiveContext* cxArg, ObjectGroup* group, bool markingUnknown)
{
    if (group->unknownProperties())
        return;

    /* All constraints listening to state changes are on the empty id. */
    HeapTypeSet* types = group->maybeGetProperty(JSID_EMPTY);

    /* Mark as unknown after getting the types, to avoid assertion. */
    if (markingUnknown)
        group->addFlags(OBJECT_FLAG_DYNAMIC_MASK | OBJECT_FLAG_UNKNOWN_PROPERTIES);

    if (types) {
        if (JSContext* cx = cxArg->maybeJSContext()) {
            TypeConstraint* constraint = types->constraintList;
            while (constraint) {
                constraint->newObjectState(cx, group);
                constraint = constraint->next;
            }
        } else {
            MOZ_ASSERT(!types->constraintList);
        }
    }
}

namespace {

class ConstraintDataFreezePropertyState
{
  public:
    enum Which {
        NON_DATA,
        NON_WRITABLE
    } which;

    explicit ConstraintDataFreezePropertyState(Which which)
      : which(which)
    {}

    const char* kind() { return (which == NON_DATA) ? "freezeNonDataProperty" : "freezeNonWritableProperty"; }

    bool invalidateOnNewType(TypeSet::Type type) { return false; }
    bool invalidateOnNewPropertyState(TypeSet* property) {
        return (which == NON_DATA)
               ? property->nonDataProperty()
               : property->nonWritableProperty();
    }
    bool invalidateOnNewObjectState(ObjectGroup* group) { return false; }

    bool constraintHolds(JSContext* cx,
                         const HeapTypeSetKey& property, TemporaryTypeSet* expected)
    {
        return !invalidateOnNewPropertyState(property.maybeTypes());
    }

    bool shouldSweep() { return false; }
};

} /* anonymous namespace */

bool
HeapTypeSetKey::nonData(CompilerConstraintList* constraints)
{
    if (maybeTypes() && maybeTypes()->nonDataProperty())
        return true;

    LifoAlloc* alloc = constraints->alloc();

    typedef CompilerConstraintInstance<ConstraintDataFreezePropertyState> T;
    constraints->add(alloc->new_<T>(alloc, *this,
                                    ConstraintDataFreezePropertyState(ConstraintDataFreezePropertyState::NON_DATA)));
    return false;
}

bool
HeapTypeSetKey::nonWritable(CompilerConstraintList* constraints)
{
    if (maybeTypes() && maybeTypes()->nonWritableProperty())
        return true;

    LifoAlloc* alloc = constraints->alloc();

    typedef CompilerConstraintInstance<ConstraintDataFreezePropertyState> T;
    constraints->add(alloc->new_<T>(alloc, *this,
                                    ConstraintDataFreezePropertyState(ConstraintDataFreezePropertyState::NON_WRITABLE)));
    return false;
}

namespace {

class ConstraintDataConstantProperty
{
  public:
    explicit ConstraintDataConstantProperty() {}

    const char* kind() { return "constantProperty"; }

    bool invalidateOnNewType(TypeSet::Type type) { return false; }
    bool invalidateOnNewPropertyState(TypeSet* property) {
        return property->nonConstantProperty();
    }
    bool invalidateOnNewObjectState(ObjectGroup* group) { return false; }

    bool constraintHolds(JSContext* cx,
                         const HeapTypeSetKey& property, TemporaryTypeSet* expected)
    {
        return !invalidateOnNewPropertyState(property.maybeTypes());
    }

    bool shouldSweep() { return false; }
};

} /* anonymous namespace */

bool
HeapTypeSetKey::constant(CompilerConstraintList* constraints, Value* valOut)
{
    if (nonData(constraints))
        return false;

    // Only singleton object properties can be marked as constants.
    JSObject* obj = object()->singleton();
    if (!obj || !obj->isNative())
        return false;

    if (maybeTypes() && maybeTypes()->nonConstantProperty())
        return false;

    // Get the current value of the property.
    Shape* shape = obj->as<NativeObject>().lookupPure(id());
    if (!shape || !shape->hasDefaultGetter() || !shape->hasSlot() || shape->hadOverwrite())
        return false;

    Value val = obj->as<NativeObject>().getSlot(shape->slot());

    // If the value is a pointer to an object in the nursery, don't optimize.
    if (val.isGCThing() && IsInsideNursery(val.toGCThing()))
        return false;

    // If the value is a string that's not atomic, don't optimize.
    if (val.isString() && !val.toString()->isAtom())
        return false;

    *valOut = val;

    LifoAlloc* alloc = constraints->alloc();
    typedef CompilerConstraintInstance<ConstraintDataConstantProperty> T;
    constraints->add(alloc->new_<T>(alloc, *this, ConstraintDataConstantProperty()));
    return true;
}

// A constraint that never triggers recompilation.
class ConstraintDataInert
{
  public:
    explicit ConstraintDataInert() {}

    const char* kind() { return "inert"; }

    bool invalidateOnNewType(TypeSet::Type type) { return false; }
    bool invalidateOnNewPropertyState(TypeSet* property) { return false; }
    bool invalidateOnNewObjectState(ObjectGroup* group) { return false; }

    bool constraintHolds(JSContext* cx,
                         const HeapTypeSetKey& property, TemporaryTypeSet* expected)
    {
        return true;
    }

    bool shouldSweep() { return false; }
};

bool
HeapTypeSetKey::couldBeConstant(CompilerConstraintList* constraints)
{
    // Only singleton object properties can be marked as constants.
    if (!object()->isSingleton())
        return false;

    if (!maybeTypes() || !maybeTypes()->nonConstantProperty())
        return true;

    // It is possible for a property that was not marked as constant to
    // 'become' one, if we throw away the type property during a GC and
    // regenerate it with the constant flag set. ObjectGroup::sweep only removes
    // type properties if they have no constraints attached to them, so add
    // inert constraints to pin these properties in place.

    LifoAlloc* alloc = constraints->alloc();
    typedef CompilerConstraintInstance<ConstraintDataInert> T;
    constraints->add(alloc->new_<T>(alloc, *this, ConstraintDataInert()));

    return false;
}

bool
TemporaryTypeSet::filtersType(const TemporaryTypeSet* other, Type filteredType) const
{
    if (other->unknown())
        return unknown();

    for (TypeFlags flag = 1; flag < TYPE_FLAG_ANYOBJECT; flag <<= 1) {
        Type type = PrimitiveType(TypeFlagPrimitive(flag));
        if (type != filteredType && other->hasType(type) && !hasType(type))
            return false;
    }

    if (other->unknownObject())
        return unknownObject();

    for (size_t i = 0; i < other->getObjectCount(); i++) {
        ObjectKey* key = other->getObject(i);
        if (key) {
            Type type = ObjectType(key);
            if (type != filteredType && !hasType(type))
                return false;
        }
    }

    return true;
}

TemporaryTypeSet::DoubleConversion
TemporaryTypeSet::convertDoubleElements(CompilerConstraintList* constraints)
{
    if (unknownObject() || !getObjectCount())
        return AmbiguousDoubleConversion;

    bool alwaysConvert = true;
    bool maybeConvert = false;
    bool dontConvert = false;

    for (unsigned i = 0; i < getObjectCount(); i++) {
        ObjectKey* key = getObject(i);
        if (!key)
            continue;

        if (key->unknownProperties()) {
            alwaysConvert = false;
            continue;
        }

        HeapTypeSetKey property = key->property(JSID_VOID);
        property.freeze(constraints);

        // We can't convert to double elements for objects which do not have
        // double in their element types (as the conversion may render the type
        // information incorrect), nor for non-array objects (as their elements
        // may point to emptyObjectElements, which cannot be converted).
        if (!property.maybeTypes() ||
            !property.maybeTypes()->hasType(DoubleType()) ||
            key->clasp() != &ArrayObject::class_)
        {
            dontConvert = true;
            alwaysConvert = false;
            continue;
        }

        // Only bother with converting known packed arrays whose possible
        // element types are int or double. Other arrays require type tests
        // when elements are accessed regardless of the conversion.
        if (property.knownMIRType(constraints) == jit::MIRType_Double &&
            !key->hasFlags(constraints, OBJECT_FLAG_NON_PACKED))
        {
            maybeConvert = true;
        } else {
            alwaysConvert = false;
        }
    }

    MOZ_ASSERT_IF(alwaysConvert, maybeConvert);

    if (maybeConvert && dontConvert)
        return AmbiguousDoubleConversion;
    if (alwaysConvert)
        return AlwaysConvertToDoubles;
    if (maybeConvert)
        return MaybeConvertToDoubles;
    return DontConvertToDoubles;
}

const Class*
TemporaryTypeSet::getKnownClass(CompilerConstraintList* constraints)
{
    if (unknownObject())
        return nullptr;

    const Class* clasp = nullptr;
    unsigned count = getObjectCount();

    for (unsigned i = 0; i < count; i++) {
        const Class* nclasp = getObjectClass(i);
        if (!nclasp)
            continue;

        if (getObject(i)->unknownProperties())
            return nullptr;

        if (clasp && clasp != nclasp)
            return nullptr;
        clasp = nclasp;
    }

    if (clasp) {
        for (unsigned i = 0; i < count; i++) {
            ObjectKey* key = getObject(i);
            if (key && !key->hasStableClassAndProto(constraints))
                return nullptr;
        }
    }

    return clasp;
}

TemporaryTypeSet::ForAllResult
TemporaryTypeSet::forAllClasses(CompilerConstraintList* constraints,
                                bool (*func)(const Class* clasp))
{
    if (unknownObject())
        return ForAllResult::MIXED;

    unsigned count = getObjectCount();
    if (count == 0)
        return ForAllResult::EMPTY;

    bool true_results = false;
    bool false_results = false;
    for (unsigned i = 0; i < count; i++) {
        const Class* clasp = getObjectClass(i);
        if (!clasp)
            continue;
        if (!getObject(i)->hasStableClassAndProto(constraints))
            return ForAllResult::MIXED;
        if (func(clasp)) {
            true_results = true;
            if (false_results)
                return ForAllResult::MIXED;
        }
        else {
            false_results = true;
            if (true_results)
                return ForAllResult::MIXED;
        }
    }

    MOZ_ASSERT(true_results != false_results);

    return true_results ? ForAllResult::ALL_TRUE : ForAllResult::ALL_FALSE;
}

Scalar::Type
TemporaryTypeSet::getTypedArrayType(CompilerConstraintList* constraints)
{
    const Class* clasp = getKnownClass(constraints);

    if (clasp && IsTypedArrayClass(clasp))
        return (Scalar::Type) (clasp - &TypedArrayObject::classes[0]);
    return Scalar::MaxTypedArrayViewType;
}

Scalar::Type
TemporaryTypeSet::getSharedTypedArrayType(CompilerConstraintList* constraints)
{
    const Class* clasp = getKnownClass(constraints);

    if (clasp && IsSharedTypedArrayClass(clasp))
        return (Scalar::Type) (clasp - &SharedTypedArrayObject::classes[0]);
    return Scalar::MaxTypedArrayViewType;
}

bool
TemporaryTypeSet::isDOMClass(CompilerConstraintList* constraints)
{
    if (unknownObject())
        return false;

    unsigned count = getObjectCount();
    for (unsigned i = 0; i < count; i++) {
        const Class* clasp = getObjectClass(i);
        if (!clasp)
            continue;
        if (!clasp->isDOMClass() || !getObject(i)->hasStableClassAndProto(constraints))
            return false;
    }

    return count > 0;
}

bool
TemporaryTypeSet::maybeCallable(CompilerConstraintList* constraints)
{
    if (!maybeObject())
        return false;

    if (unknownObject())
        return true;

    unsigned count = getObjectCount();
    for (unsigned i = 0; i < count; i++) {
        const Class* clasp = getObjectClass(i);
        if (!clasp)
            continue;
        if (clasp->isProxy() || clasp->nonProxyCallable())
            return true;
        if (!getObject(i)->hasStableClassAndProto(constraints))
            return true;
    }

    return false;
}

bool
TemporaryTypeSet::maybeEmulatesUndefined(CompilerConstraintList* constraints)
{
    if (!maybeObject())
        return false;

    if (unknownObject())
        return true;

    unsigned count = getObjectCount();
    for (unsigned i = 0; i < count; i++) {
        // The object emulates undefined if clasp->emulatesUndefined() or if
        // it's a WrapperObject, see EmulatesUndefined. Since all wrappers are
        // proxies, we can just check for that.
        const Class* clasp = getObjectClass(i);
        if (!clasp)
            continue;
        if (clasp->emulatesUndefined() || clasp->isProxy())
            return true;
        if (!getObject(i)->hasStableClassAndProto(constraints))
            return true;
    }

    return false;
}

bool
TemporaryTypeSet::getCommonPrototype(CompilerConstraintList* constraints, JSObject** proto)
{
    if (unknownObject())
        return false;

    *proto = nullptr;
    bool isFirst = true;
    unsigned count = getObjectCount();

    for (unsigned i = 0; i < count; i++) {
        ObjectKey* key = getObject(i);
        if (!key)
            continue;

        if (key->unknownProperties())
            return false;

        TaggedProto nproto = key->proto();
        if (isFirst) {
            if (nproto.isLazy())
                return false;
            *proto = nproto.toObjectOrNull();
            isFirst = false;
        } else {
            if (nproto != TaggedProto(*proto))
                return false;
        }
    }

    // Guard against mutating __proto__.
    for (unsigned i = 0; i < count; i++) {
        if (ObjectKey* key = getObject(i))
            JS_ALWAYS_TRUE(key->hasStableClassAndProto(constraints));
    }

    return true;
}

bool
TemporaryTypeSet::propertyNeedsBarrier(CompilerConstraintList* constraints, jsid id)
{
    if (unknownObject())
        return true;

    for (unsigned i = 0; i < getObjectCount(); i++) {
        ObjectKey* key = getObject(i);
        if (!key)
            continue;

        if (key->unknownProperties())
            return true;

        HeapTypeSetKey property = key->property(id);
        if (property.needsBarrier(constraints))
            return true;
    }

    return false;
}

static inline bool
ClassCanHaveExtraProperties(const Class* clasp)
{
    return clasp->resolve
        || clasp->ops.lookupProperty
        || clasp->ops.getProperty
        || IsAnyTypedArrayClass(clasp);
}

static bool
PrototypeHasIndexedProperty(CompilerConstraintList* constraints, JSObject* obj)
{
    do {
        TypeSet::ObjectKey* key = TypeSet::ObjectKey::get(obj);
        if (ClassCanHaveExtraProperties(key->clasp()))
            return true;
        if (key->unknownProperties())
            return true;
        HeapTypeSetKey index = key->property(JSID_VOID);
        if (index.nonData(constraints) || index.isOwnProperty(constraints))
            return true;
        obj = obj->getProto();
    } while (obj);

    return false;
}

bool
js::ArrayPrototypeHasIndexedProperty(CompilerConstraintList* constraints, JSScript* script)
{
    if (JSObject* proto = script->global().maybeGetArrayPrototype())
        return PrototypeHasIndexedProperty(constraints, proto);
    return true;
}

bool
js::TypeCanHaveExtraIndexedProperties(CompilerConstraintList* constraints,
                                      TemporaryTypeSet* types)
{
    const Class* clasp = types->getKnownClass(constraints);

    // Note: typed arrays have indexed properties not accounted for by type
    // information, though these are all in bounds and will be accounted for
    // by JIT paths.
    if (!clasp || (ClassCanHaveExtraProperties(clasp) && !IsAnyTypedArrayClass(clasp)))
        return true;

    if (types->hasObjectFlags(constraints, OBJECT_FLAG_SPARSE_INDEXES))
        return true;

    JSObject* proto;
    if (!types->getCommonPrototype(constraints, &proto))
        return true;

    if (!proto)
        return false;

    return PrototypeHasIndexedProperty(constraints, proto);
}

void
TypeZone::processPendingRecompiles(FreeOp* fop, RecompileInfoVector& recompiles)
{
    MOZ_ASSERT(!recompiles.empty());

    /*
     * Steal the list of scripts to recompile, to make sure we don't try to
     * recursively recompile them.
     */
    RecompileInfoVector pending;
    for (size_t i = 0; i < recompiles.length(); i++) {
        if (!pending.append(recompiles[i]))
            CrashAtUnhandlableOOM("processPendingRecompiles");
    }
    recompiles.clear();

    jit::Invalidate(*this, fop, pending);

    MOZ_ASSERT(recompiles.empty());
}

void
TypeZone::addPendingRecompile(JSContext* cx, const RecompileInfo& info)
{
    CompilerOutput* co = info.compilerOutput(cx);
    if (!co || !co->isValid() || co->pendingInvalidation())
        return;

    InferSpew(ISpewOps, "addPendingRecompile: %p:%s:%d",
              co->script(), co->script()->filename(), co->script()->lineno());

    co->setPendingInvalidation();

    if (!cx->zone()->types.activeAnalysis->pendingRecompiles.append(info))
        CrashAtUnhandlableOOM("Could not update pendingRecompiles");
}

void
TypeZone::addPendingRecompile(JSContext* cx, JSScript* script)
{
    MOZ_ASSERT(script);

    CancelOffThreadIonCompile(cx->compartment(), script);

    // Let the script warm up again before attempting another compile.
    if (jit::IsBaselineEnabled(cx))
        script->resetWarmUpCounter();

    if (script->hasIonScript())
        addPendingRecompile(cx, script->ionScript()->recompileInfo());

    // When one script is inlined into another the caller listens to state
    // changes on the callee's script, so trigger these to force recompilation
    // of any such callers.
    if (script->functionNonDelazifying() && !script->functionNonDelazifying()->hasLazyGroup())
        ObjectStateChange(cx, script->functionNonDelazifying()->group(), false);
}

void
js::PrintTypes(JSContext* cx, JSCompartment* comp, bool force)
{
#ifdef DEBUG
    gc::AutoSuppressGC suppressGC(cx);
    JSAutoRequest request(cx);

    Zone* zone = comp->zone();
    AutoEnterAnalysis enter(nullptr, zone);

    if (!force && !InferSpewActive(ISpewResult))
        return;

    for (gc::ZoneCellIter i(zone, gc::FINALIZE_SCRIPT); !i.done(); i.next()) {
        RootedScript script(cx, i.get<JSScript>());
        if (script->types())
            script->types()->printTypes(cx, script);
    }

    for (gc::ZoneCellIter i(zone, gc::FINALIZE_OBJECT_GROUP); !i.done(); i.next()) {
        ObjectGroup* group = i.get<ObjectGroup>();
        group->print();
    }
#endif
}

/////////////////////////////////////////////////////////////////////
// ObjectGroup
/////////////////////////////////////////////////////////////////////

static inline void
UpdatePropertyType(ExclusiveContext* cx, HeapTypeSet* types, NativeObject* obj, Shape* shape,
                   bool indexed)
{
    MOZ_ASSERT(obj->isSingleton() && !obj->hasLazyGroup());

    if (!shape->writable())
        types->setNonWritableProperty(cx);

    if (shape->hasGetterValue() || shape->hasSetterValue()) {
        types->setNonDataProperty(cx);
        types->TypeSet::addType(TypeSet::UnknownType(), &cx->typeLifoAlloc());
    } else if (shape->hasDefaultGetter() && shape->hasSlot()) {
        if (!indexed && types->canSetDefinite(shape->slot()))
            types->setDefinite(shape->slot());

        const Value& value = obj->getSlot(shape->slot());

        /*
         * Don't add initial undefined types for properties of global objects
         * that are not collated into the JSID_VOID property (see propertySet
         * comment).
         *
         * Also don't add untracked values (initial uninitialized lexical
         * magic values and optimized out values) as appearing in CallObjects.
         */
        MOZ_ASSERT_IF(TypeSet::IsUntrackedValue(value), obj->is<CallObject>());
        if ((indexed || !value.isUndefined() || !CanHaveEmptyPropertyTypesForOwnProperty(obj)) &&
            !TypeSet::IsUntrackedValue(value))
        {
            TypeSet::Type type = TypeSet::GetValueType(value);
            types->TypeSet::addType(type, &cx->typeLifoAlloc());
        }

        if (indexed || shape->hadOverwrite()) {
            types->setNonConstantProperty(cx);
        } else {
            InferSpew(ISpewOps, "typeSet: %sT%p%s property %s %s - setConstant",
                      InferSpewColor(types), types, InferSpewColorReset(),
                      TypeSet::ObjectGroupString(obj->group()), TypeIdString(shape->propid()));
        }
    }
}

void
ObjectGroup::updateNewPropertyTypes(ExclusiveContext* cx, jsid id, HeapTypeSet* types)
{
    InferSpew(ISpewOps, "typeSet: %sT%p%s property %s %s",
              InferSpewColor(types), types, InferSpewColorReset(),
              TypeSet::ObjectGroupString(this), TypeIdString(id));

    if (!singleton() || !singleton()->isNative()) {
        types->setNonConstantProperty(cx);
        return;
    }

    NativeObject* obj = &singleton()->as<NativeObject>();

    /*
     * Fill the property in with any type the object already has in an own
     * property. We are only interested in plain native properties and
     * dense elements which don't go through a barrier when read by the VM
     * or jitcode.
     */

    if (JSID_IS_VOID(id)) {
        /* Go through all shapes on the object to get integer-valued properties. */
        RootedShape shape(cx, obj->lastProperty());
        while (!shape->isEmptyShape()) {
            if (JSID_IS_VOID(IdToTypeId(shape->propid())))
                UpdatePropertyType(cx, types, obj, shape, true);
            shape = shape->previous();
        }

        /* Also get values of any dense elements in the object. */
        for (size_t i = 0; i < obj->getDenseInitializedLength(); i++) {
            const Value& value = obj->getDenseElement(i);
            if (!value.isMagic(JS_ELEMENTS_HOLE)) {
                TypeSet::Type type = TypeSet::GetValueType(value);
                types->TypeSet::addType(type, &cx->typeLifoAlloc());
            }
        }
    } else if (!JSID_IS_EMPTY(id)) {
        RootedId rootedId(cx, id);
        Shape* shape = obj->lookup(cx, rootedId);
        if (shape)
            UpdatePropertyType(cx, types, obj, shape, false);
    }

    if (obj->watched()) {
        /*
         * Mark the property as non-data, to inhibit optimizations on it
         * and avoid bypassing the watchpoint handler.
         */
        types->setNonDataProperty(cx);
    }
}

bool
ObjectGroup::addDefiniteProperties(ExclusiveContext* cx, Shape* shape)
{
    if (unknownProperties())
        return true;

    // Mark all properties of shape as definite properties of this group.
    AutoEnterAnalysis enter(cx);

    while (!shape->isEmptyShape()) {
        jsid id = IdToTypeId(shape->propid());
        if (!JSID_IS_VOID(id)) {
            MOZ_ASSERT_IF(shape->slot() >= shape->numFixedSlots(),
                          shape->numFixedSlots() == NativeObject::MAX_FIXED_SLOTS);
            TypeSet* types = getProperty(cx, id);
            if (!types)
                return false;
            if (types->canSetDefinite(shape->slot()))
                types->setDefinite(shape->slot());
        }

        shape = shape->previous();
    }

    return true;
}

bool
ObjectGroup::matchDefiniteProperties(HandleObject obj)
{
    unsigned count = getPropertyCount();
    for (unsigned i = 0; i < count; i++) {
        Property* prop = getProperty(i);
        if (!prop)
            continue;
        if (prop->types.definiteProperty()) {
            unsigned slot = prop->types.definiteSlot();

            bool found = false;
            Shape* shape = obj->lastProperty();
            while (!shape->isEmptyShape()) {
                if (shape->slot() == slot && shape->propid() == prop->id) {
                    found = true;
                    break;
                }
                shape = shape->previous();
            }
            if (!found)
                return false;
        }
    }

    return true;
}

void
js::AddTypePropertyId(ExclusiveContext* cx, ObjectGroup* group, jsid id, TypeSet::Type type)
{
    MOZ_ASSERT(id == IdToTypeId(id));

    if (group->unknownProperties())
        return;

    AutoEnterAnalysis enter(cx);

    HeapTypeSet* types = group->getProperty(cx, id);
    if (!types)
        return;

    // Clear any constant flag if it exists.
    if (!types->empty() && !types->nonConstantProperty()) {
        InferSpew(ISpewOps, "constantMutated: %sT%p%s %s",
                  InferSpewColor(types), types, InferSpewColorReset(), TypeSet::TypeString(type));
        types->setNonConstantProperty(cx);
    }

    if (types->hasType(type))
        return;

    InferSpew(ISpewOps, "externalType: property %s %s: %s",
              TypeSet::ObjectGroupString(group), TypeIdString(id), TypeSet::TypeString(type));
    types->addType(cx, type);

    // If this addType caused the type set to be marked as containing any
    // object, make sure that is reflected in other type sets the addType is
    // propagated to below.
    if (type.isObjectUnchecked() && types->unknownObject())
        type = TypeSet::AnyObjectType();

    // Propagate new types from partially initialized groups to fully
    // initialized groups for the acquired properties analysis. Note that we
    // don't need to do this for other property changes, as these will also be
    // reflected via shape changes on the object that will prevent the object
    // from acquiring the fully initialized group.
    if (group->newScript() && group->newScript()->initializedGroup())
        AddTypePropertyId(cx, group->newScript()->initializedGroup(), id, type);

    // Maintain equivalent type information for unboxed object groups and their
    // corresponding native group. Since type sets might contain the unboxed
    // group but not the native group, this ensures optimizations based on the
    // unboxed group are valid for the native group.
    if (group->maybeUnboxedLayout() && group->maybeUnboxedLayout()->nativeGroup())
        AddTypePropertyId(cx, group->maybeUnboxedLayout()->nativeGroup(), id, type);
    if (ObjectGroup* unboxedGroup = group->maybeOriginalUnboxedGroup())
        AddTypePropertyId(cx, unboxedGroup, id, type);
}

void
js::AddTypePropertyId(ExclusiveContext* cx, ObjectGroup* group, jsid id, const Value& value)
{
    AddTypePropertyId(cx, group, id, TypeSet::GetValueType(value));
}

void
ObjectGroup::markPropertyNonData(ExclusiveContext* cx, jsid id)
{
    AutoEnterAnalysis enter(cx);

    id = IdToTypeId(id);

    HeapTypeSet* types = getProperty(cx, id);
    if (types)
        types->setNonDataProperty(cx);
}

void
ObjectGroup::markPropertyNonWritable(ExclusiveContext* cx, jsid id)
{
    AutoEnterAnalysis enter(cx);

    id = IdToTypeId(id);

    HeapTypeSet* types = getProperty(cx, id);
    if (types)
        types->setNonWritableProperty(cx);
}

bool
ObjectGroup::isPropertyNonData(jsid id)
{
    TypeSet* types = maybeGetProperty(id);
    if (types)
        return types->nonDataProperty();
    return false;
}

bool
ObjectGroup::isPropertyNonWritable(jsid id)
{
    TypeSet* types = maybeGetProperty(id);
    if (types)
        return types->nonWritableProperty();
    return false;
}

void
ObjectGroup::markStateChange(ExclusiveContext* cxArg)
{
    if (unknownProperties())
        return;

    AutoEnterAnalysis enter(cxArg);
    HeapTypeSet* types = maybeGetProperty(JSID_EMPTY);
    if (types) {
        if (JSContext* cx = cxArg->maybeJSContext()) {
            TypeConstraint* constraint = types->constraintList;
            while (constraint) {
                constraint->newObjectState(cx, this);
                constraint = constraint->next;
            }
        } else {
            MOZ_ASSERT(!types->constraintList);
        }
    }
}

void
ObjectGroup::setFlags(ExclusiveContext* cx, ObjectGroupFlags flags)
{
    if (hasAllFlags(flags))
        return;

    AutoEnterAnalysis enter(cx);

    if (singleton()) {
        /* Make sure flags are consistent with persistent object state. */
        MOZ_ASSERT_IF(flags & OBJECT_FLAG_ITERATED,
                      singleton()->lastProperty()->hasObjectFlag(BaseShape::ITERATED_SINGLETON));
    }

    addFlags(flags);

    InferSpew(ISpewOps, "%s: setFlags 0x%x", TypeSet::ObjectGroupString(this), flags);

    ObjectStateChange(cx, this, false);

    // Propagate flag changes from partially to fully initialized groups for the
    // acquired properties analysis.
    if (newScript() && newScript()->initializedGroup())
        newScript()->initializedGroup()->setFlags(cx, flags);

    // Propagate flag changes between unboxed and corresponding native groups.
    if (maybeUnboxedLayout() && maybeUnboxedLayout()->nativeGroup())
        maybeUnboxedLayout()->nativeGroup()->setFlags(cx, flags);
    if (ObjectGroup* unboxedGroup = maybeOriginalUnboxedGroup())
        unboxedGroup->setFlags(cx, flags);
}

void
ObjectGroup::markUnknown(ExclusiveContext* cx)
{
    AutoEnterAnalysis enter(cx);

    MOZ_ASSERT(cx->zone()->types.activeAnalysis);
    MOZ_ASSERT(!unknownProperties());

    InferSpew(ISpewOps, "UnknownProperties: %s", TypeSet::ObjectGroupString(this));

    clearNewScript(cx);
    ObjectStateChange(cx, this, true);

    /*
     * Existing constraints may have already been added to this object, which we need
     * to do the right thing for. We can't ensure that we will mark all unknown
     * objects before they have been accessed, as the __proto__ of a known object
     * could be dynamically set to an unknown object, and we can decide to ignore
     * properties of an object during analysis (i.e. hashmaps). Adding unknown for
     * any properties accessed already accounts for possible values read from them.
     */

    unsigned count = getPropertyCount();
    for (unsigned i = 0; i < count; i++) {
        Property* prop = getProperty(i);
        if (prop) {
            prop->types.addType(cx, TypeSet::UnknownType());
            prop->types.setNonDataProperty(cx);
        }
    }
}

TypeNewScript*
ObjectGroup::anyNewScript()
{
    if (newScript())
        return newScript();
    if (maybeUnboxedLayout())
        return unboxedLayout().newScript();
    return nullptr;
}

void
ObjectGroup::detachNewScript(bool writeBarrier)
{
    // Clear the TypeNewScript from this ObjectGroup and, if it has been
    // analyzed, remove it from the newObjectGroups table so that it will not be
    // produced by calling 'new' on the associated function anymore.
    // The TypeNewScript is not actually destroyed.
    TypeNewScript* newScript = anyNewScript();
    MOZ_ASSERT(newScript);

    if (newScript->analyzed()) {
        newScript->function()->compartment()->objectGroups.removeDefaultNewGroup(nullptr, proto(),
                                                                                 newScript->function());
    }

    if (this->newScript())
        setAddendum(Addendum_None, nullptr, writeBarrier);
    else
        unboxedLayout().setNewScript(nullptr, writeBarrier);
}

void
ObjectGroup::maybeClearNewScriptOnOOM()
{
    MOZ_ASSERT(zone()->isGCSweepingOrCompacting());

    if (!isMarked())
        return;

    TypeNewScript* newScript = anyNewScript();
    if (!newScript)
        return;

    addFlags(OBJECT_FLAG_NEW_SCRIPT_CLEARED);

    // This method is called during GC sweeping, so don't trigger pre barriers.
    detachNewScript(/* writeBarrier = */ false);

    js_delete(newScript);
}

void
ObjectGroup::clearNewScript(ExclusiveContext* cx)
{
    TypeNewScript* newScript = anyNewScript();
    if (!newScript)
        return;

    AutoEnterAnalysis enter(cx);

    // Invalidate any Ion code constructing objects of this type.
    setFlags(cx, OBJECT_FLAG_NEW_SCRIPT_CLEARED);

    // Mark the constructing function as having its 'new' script cleared, so we
    // will not try to construct another one later.
    if (!newScript->function()->setNewScriptCleared(cx))
        cx->recoverFromOutOfMemory();

    detachNewScript(/* writeBarrier = */ true);

    if (cx->isJSContext()) {
        bool found = newScript->rollbackPartiallyInitializedObjects(cx->asJSContext(), this);

        // If we managed to rollback any partially initialized objects, then
        // any definite properties we added due to analysis of the new script
        // are now invalid, so remove them. If there weren't any partially
        // initialized objects then we don't need to change type information,
        // as no more objects of this type will be created and the 'new' script
        // analysis was still valid when older objects were created.
        if (found) {
            for (unsigned i = 0; i < getPropertyCount(); i++) {
                Property* prop = getProperty(i);
                if (!prop)
                    continue;
                if (prop->types.definiteProperty())
                    prop->types.setNonDataProperty(cx);
            }
        }
    } else {
        // Threads with an ExclusiveContext are not allowed to run scripts.
        MOZ_ASSERT(!cx->perThreadData->runtimeIfOnOwnerThread() ||
                   !cx->perThreadData->runtimeIfOnOwnerThread()->activation());
    }

    js_delete(newScript);
    markStateChange(cx);
}

void
ObjectGroup::print()
{
    TaggedProto tagged(proto());
    fprintf(stderr, "%s : %s",
            TypeSet::ObjectGroupString(this),
            tagged.isObject() ? TypeSet::TypeString(TypeSet::ObjectType(tagged.toObject()))
                              : (tagged.isLazy() ? "(lazy)" : "(null)"));

    if (unknownProperties()) {
        fprintf(stderr, " unknown");
    } else {
        if (!hasAnyFlags(OBJECT_FLAG_SPARSE_INDEXES))
            fprintf(stderr, " dense");
        if (!hasAnyFlags(OBJECT_FLAG_NON_PACKED))
            fprintf(stderr, " packed");
        if (!hasAnyFlags(OBJECT_FLAG_LENGTH_OVERFLOW))
            fprintf(stderr, " noLengthOverflow");
        if (hasAnyFlags(OBJECT_FLAG_ITERATED))
            fprintf(stderr, " iterated");
        if (maybeInterpretedFunction())
            fprintf(stderr, " ifun");
    }

    unsigned count = getPropertyCount();

    if (count == 0) {
        fprintf(stderr, " {}\n");
        return;
    }

    fprintf(stderr, " {");

    if (newScript()) {
        if (newScript()->analyzed()) {
            fprintf(stderr, "\n    newScript %d properties",
                    (int) newScript()->templateObject()->slotSpan());
            if (newScript()->initializedGroup()) {
                fprintf(stderr, " initializedGroup %p with %d properties",
                        newScript()->initializedGroup(), (int) newScript()->initializedShape()->slotSpan());
            }
        } else {
            fprintf(stderr, "\n    newScript unanalyzed");
        }
    }

    for (unsigned i = 0; i < count; i++) {
        Property* prop = getProperty(i);
        if (prop) {
            fprintf(stderr, "\n    %s:", TypeIdString(prop->id));
            prop->types.print();
        }
    }

    fprintf(stderr, "\n}\n");
}

/////////////////////////////////////////////////////////////////////
// Type Analysis
/////////////////////////////////////////////////////////////////////

/*
 * Persistent constraint clearing out newScript and definite properties from
 * an object should a property on another object get a getter or setter.
 */
class TypeConstraintClearDefiniteGetterSetter : public TypeConstraint
{
  public:
    ObjectGroup* group;

    explicit TypeConstraintClearDefiniteGetterSetter(ObjectGroup* group)
      : group(group)
    {}

    const char* kind() { return "clearDefiniteGetterSetter"; }

    void newPropertyState(JSContext* cx, TypeSet* source) {
        /*
         * Clear out the newScript shape and definite property information from
         * an object if the source type set could be a setter or could be
         * non-writable.
         */
        if (source->nonDataProperty() || source->nonWritableProperty())
            group->clearNewScript(cx);
    }

    void newType(JSContext* cx, TypeSet* source, TypeSet::Type type) {}

    bool sweep(TypeZone& zone, TypeConstraint** res) {
        if (IsObjectGroupAboutToBeFinalized(&group))
            return false;
        *res = zone.typeLifoAlloc.new_<TypeConstraintClearDefiniteGetterSetter>(group);
        return true;
    }
};

bool
js::AddClearDefiniteGetterSetterForPrototypeChain(JSContext* cx, ObjectGroup* group, HandleId id)
{
    /*
     * Ensure that if the properties named here could have a getter, setter or
     * a permanent property in any transitive prototype, the definite
     * properties get cleared from the group.
     */
    RootedObject proto(cx, group->proto().toObjectOrNull());
    while (proto) {
        ObjectGroup* protoGroup = proto->getGroup(cx);
        if (!protoGroup || protoGroup->unknownProperties())
            return false;
        HeapTypeSet* protoTypes = protoGroup->getProperty(cx, id);
        if (!protoTypes || protoTypes->nonDataProperty() || protoTypes->nonWritableProperty())
            return false;
        if (!protoTypes->addConstraint(cx, cx->typeLifoAlloc().new_<TypeConstraintClearDefiniteGetterSetter>(group)))
            return false;
        proto = proto->getProto();
    }
    return true;
}

/*
 * Constraint which clears definite properties on a group should a type set
 * contain any types other than a single object.
 */
class TypeConstraintClearDefiniteSingle : public TypeConstraint
{
  public:
    ObjectGroup* group;

    explicit TypeConstraintClearDefiniteSingle(ObjectGroup* group)
      : group(group)
    {}

    const char* kind() { return "clearDefiniteSingle"; }

    void newType(JSContext* cx, TypeSet* source, TypeSet::Type type) {
        if (source->baseFlags() || source->getObjectCount() > 1)
            group->clearNewScript(cx);
    }

    bool sweep(TypeZone& zone, TypeConstraint** res) {
        if (IsObjectGroupAboutToBeFinalized(&group))
            return false;
        *res = zone.typeLifoAlloc.new_<TypeConstraintClearDefiniteSingle>(group);
        return true;
    }
};

bool
js::AddClearDefiniteFunctionUsesInScript(JSContext* cx, ObjectGroup* group,
                                            JSScript* script, JSScript* calleeScript)
{
    // Look for any uses of the specified calleeScript in type sets for
    // |script|, and add constraints to ensure that if the type sets' contents
    // change then the definite properties are cleared from the type.
    // This ensures that the inlining performed when the definite properties
    // analysis was done is stable. We only need to look at type sets which
    // contain a single object, as IonBuilder does not inline polymorphic sites
    // during the definite properties analysis.

    TypeSet::ObjectKey* calleeKey =
        TypeSet::ObjectType(calleeScript->functionNonDelazifying()).objectKey();

    unsigned count = TypeScript::NumTypeSets(script);
    StackTypeSet* typeArray = script->types()->typeArray();

    for (unsigned i = 0; i < count; i++) {
        StackTypeSet* types = &typeArray[i];
        if (!types->unknownObject() && types->getObjectCount() == 1) {
            if (calleeKey != types->getObject(0)) {
                // Also check if the object is the Function.call or
                // Function.apply native. IonBuilder uses the presence of these
                // functions during inlining.
                JSObject* singleton = types->getSingleton(0);
                if (!singleton || !singleton->is<JSFunction>())
                    continue;
                JSFunction* fun = &singleton->as<JSFunction>();
                if (!fun->isNative())
                    continue;
                if (fun->native() != js_fun_call && fun->native() != js_fun_apply)
                    continue;
            }
            // This is a type set that might have been used when inlining
            // |calleeScript| into |script|.
            if (!types->addConstraint(cx, cx->typeLifoAlloc().new_<TypeConstraintClearDefiniteSingle>(group)))
                return false;
        }
    }

    return true;
}

/////////////////////////////////////////////////////////////////////
// Interface functions
/////////////////////////////////////////////////////////////////////

void
js::TypeMonitorCallSlow(JSContext* cx, JSObject* callee, const CallArgs& args, bool constructing)
{
    unsigned nargs = callee->as<JSFunction>().nargs();
    JSScript* script = callee->as<JSFunction>().nonLazyScript();

    if (!constructing)
        TypeScript::SetThis(cx, script, args.thisv());

    /*
     * Add constraints going up to the minimum of the actual and formal count.
     * If there are more actuals than formals the later values can only be
     * accessed through the arguments object, which is monitored.
     */
    unsigned arg = 0;
    for (; arg < args.length() && arg < nargs; arg++)
        TypeScript::SetArgument(cx, script, arg, args[arg]);

    /* Watch for fewer actuals than formals to the call. */
    for (; arg < nargs; arg++)
        TypeScript::SetArgument(cx, script, arg, UndefinedValue());
}

static inline bool
IsAboutToBeFinalized(TypeSet::ObjectKey** keyp)
{
    // Mask out the low bit indicating whether this is a group or JS object.
    uintptr_t flagBit = uintptr_t(*keyp) & 1;
    gc::Cell* tmp = reinterpret_cast<gc::Cell*>(uintptr_t(*keyp) & ~1);
    bool isAboutToBeFinalized = IsCellAboutToBeFinalized(&tmp);
    *keyp = reinterpret_cast<TypeSet::ObjectKey*>(uintptr_t(tmp) | flagBit);
    return isAboutToBeFinalized;
}

void
js::FillBytecodeTypeMap(JSScript* script, uint32_t* bytecodeMap)
{
    uint32_t added = 0;
    for (jsbytecode* pc = script->code(); pc < script->codeEnd(); pc += GetBytecodeLength(pc)) {
        JSOp op = JSOp(*pc);
        if (js_CodeSpec[op].format & JOF_TYPESET) {
            bytecodeMap[added++] = script->pcToOffset(pc);
            if (added == script->nTypeSets())
                break;
        }
    }
    MOZ_ASSERT(added == script->nTypeSets());
}

void
js::TypeMonitorResult(JSContext* cx, JSScript* script, jsbytecode* pc, const js::Value& rval)
{
    /* Allow the non-TYPESET scenario to simplify stubs used in compound opcodes. */
    if (!(js_CodeSpec[*pc].format & JOF_TYPESET))
        return;

    if (!script->hasBaselineScript())
        return;

    AutoEnterAnalysis enter(cx);

    TypeSet::Type type = TypeSet::GetValueType(rval);
    StackTypeSet* types = TypeScript::BytecodeTypes(script, pc);
    if (types->hasType(type))
        return;

    InferSpew(ISpewOps, "bytecodeType: %p %05u: %s",
              script, script->pcToOffset(pc), TypeSet::TypeString(type));
    types->addType(cx, type);
}

/////////////////////////////////////////////////////////////////////
// TypeScript
/////////////////////////////////////////////////////////////////////

bool
JSScript::makeTypes(JSContext* cx)
{
    MOZ_ASSERT(!types_);

    AutoEnterAnalysis enter(cx);

    unsigned count = TypeScript::NumTypeSets(this);

    TypeScript* typeScript = (TypeScript*)
        zone()->pod_calloc<uint8_t>(TypeScript::SizeIncludingTypeArray(count));
    if (!typeScript)
        return false;

    types_ = typeScript;
    setTypesGeneration(cx->zone()->types.generation);

#ifdef DEBUG
    StackTypeSet* typeArray = typeScript->typeArray();
    for (unsigned i = 0; i < nTypeSets(); i++) {
        InferSpew(ISpewOps, "typeSet: %sT%p%s bytecode%u %p",
                  InferSpewColor(&typeArray[i]), &typeArray[i], InferSpewColorReset(),
                  i, this);
    }
    TypeSet* thisTypes = TypeScript::ThisTypes(this);
    InferSpew(ISpewOps, "typeSet: %sT%p%s this %p",
              InferSpewColor(thisTypes), thisTypes, InferSpewColorReset(),
              this);
    unsigned nargs = functionNonDelazifying() ? functionNonDelazifying()->nargs() : 0;
    for (unsigned i = 0; i < nargs; i++) {
        TypeSet* types = TypeScript::ArgTypes(this, i);
        InferSpew(ISpewOps, "typeSet: %sT%p%s arg%u %p",
                  InferSpewColor(types), types, InferSpewColorReset(),
                  i, this);
    }
#endif

    return true;
}

/* static */ bool
JSFunction::setTypeForScriptedFunction(ExclusiveContext* cx, HandleFunction fun,
                                       bool singleton /* = false */)
{
    if (singleton) {
        if (!setSingleton(cx, fun))
            return false;
    } else {
        RootedObject funProto(cx, fun->getProto());
        Rooted<TaggedProto> taggedProto(cx, TaggedProto(funProto));
        ObjectGroup* group = ObjectGroupCompartment::makeGroup(cx, &JSFunction::class_,
                                                               taggedProto);
        if (!group)
            return false;

        fun->setGroup(group);
        group->setInterpretedFunction(fun);
    }

    return true;
}

/////////////////////////////////////////////////////////////////////
// PreliminaryObjectArray
/////////////////////////////////////////////////////////////////////

void
PreliminaryObjectArray::registerNewObject(JSObject* res)
{
    // The preliminary object pointers are weak, and won't be swept properly
    // during nursery collections, so the preliminary objects need to be
    // initially tenured.
    MOZ_ASSERT(!IsInsideNursery(res));

    for (size_t i = 0; i < COUNT; i++) {
        if (!objects[i]) {
            objects[i] = res;
            return;
        }
    }

    MOZ_CRASH("There should be room for registering the new object");
}

bool
PreliminaryObjectArray::full() const
{
    for (size_t i = 0; i < COUNT; i++) {
        if (!objects[i])
            return false;
    }
    return true;
}

void
PreliminaryObjectArray::sweep()
{
    // All objects in the array are weak, so clear any that are about to be
    // destroyed.
    for (size_t i = 0; i < COUNT; i++) {
        JSObject** ptr = &objects[i];
        if (*ptr && IsObjectAboutToBeFinalized(ptr))
            *ptr = nullptr;
    }
}

/////////////////////////////////////////////////////////////////////
// TypeNewScript
/////////////////////////////////////////////////////////////////////

// Make a TypeNewScript for |group|, and set it up to hold the initial
// PRELIMINARY_OBJECT_COUNT objects created with the group.
/* static */ void
TypeNewScript::make(JSContext* cx, ObjectGroup* group, JSFunction* fun)
{
    MOZ_ASSERT(cx->zone()->types.activeAnalysis);
    MOZ_ASSERT(!group->newScript());
    MOZ_ASSERT(!group->maybeUnboxedLayout());

    if (group->unknownProperties())
        return;

    ScopedJSDeletePtr<TypeNewScript> newScript(cx->new_<TypeNewScript>());
    if (!newScript)
        return;

    newScript->function_ = fun;

    newScript->preliminaryObjects = group->zone()->new_<PreliminaryObjectArray>();
    if (!newScript->preliminaryObjects)
        return;

    group->setNewScript(newScript.forget());

    gc::TraceTypeNewScript(group);
}

size_t
TypeNewScript::sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const
{
    size_t n = mallocSizeOf(this);
    n += mallocSizeOf(preliminaryObjects);
    n += mallocSizeOf(initializerList);
    return n;
}

void
TypeNewScript::registerNewObject(PlainObject* res)
{
    MOZ_ASSERT(!analyzed());

    // New script objects must have the maximum number of fixed slots, so that
    // we can adjust their shape later to match the number of fixed slots used
    // by the template object we eventually create.
    MOZ_ASSERT(res->numFixedSlots() == NativeObject::MAX_FIXED_SLOTS);

    preliminaryObjects->registerNewObject(res);
}

// Return whether shape consists entirely of plain data properties.
static bool
OnlyHasDataProperties(Shape* shape)
{
    MOZ_ASSERT(!shape->inDictionary());

    while (!shape->isEmptyShape()) {
        if (!shape->isDataDescriptor() ||
            !shape->configurable() ||
            !shape->enumerable() ||
            !shape->writable() ||
            !shape->hasSlot())
        {
            return false;
        }
        shape = shape->previous();
    }

    return true;
}

// Find the most recent common ancestor of two shapes, or an empty shape if
// the two shapes have no common ancestor.
static Shape*
CommonPrefix(Shape* first, Shape* second)
{
    MOZ_ASSERT(OnlyHasDataProperties(first));
    MOZ_ASSERT(OnlyHasDataProperties(second));

    while (first->slotSpan() > second->slotSpan())
        first = first->previous();
    while (second->slotSpan() > first->slotSpan())
        second = second->previous();

    while (first != second && !first->isEmptyShape()) {
        first = first->previous();
        second = second->previous();
    }

    return first;
}

static bool
ChangeObjectFixedSlotCount(JSContext* cx, PlainObject* obj, gc::AllocKind allocKind)
{
    MOZ_ASSERT(OnlyHasDataProperties(obj->lastProperty()));

    Shape* newShape = ReshapeForParentAndAllocKind(cx, obj->lastProperty(),
                                                   obj->getTaggedProto(), obj->getParent(),
                                                   allocKind);
    if (!newShape)
        return false;

    obj->setLastPropertyShrinkFixedSlots(newShape);
    return true;
}

namespace {

struct DestroyTypeNewScript
{
    JSContext* cx;
    ObjectGroup* group;

    DestroyTypeNewScript(JSContext* cx, ObjectGroup* group)
      : cx(cx), group(group)
    {}

    ~DestroyTypeNewScript() {
        if (group)
            group->clearNewScript(cx);
    }
};

} // anonymous namespace

bool
TypeNewScript::maybeAnalyze(JSContext* cx, ObjectGroup* group, bool* regenerate, bool force)
{
    // Perform the new script properties analysis if necessary, returning
    // whether the new group table was updated and group needs to be refreshed.
    MOZ_ASSERT(this == group->newScript());

    // Make sure there aren't dead references in preliminaryObjects. This can
    // clear out the new script information on OOM.
    group->maybeSweep(nullptr);
    if (!group->newScript())
        return true;

    if (regenerate)
        *regenerate = false;

    if (analyzed()) {
        // The analyses have already been performed.
        return true;
    }

    // Don't perform the analyses until sufficient preliminary objects have
    // been allocated.
    if (!force && !preliminaryObjects->full())
        return true;

    AutoEnterAnalysis enter(cx);

    // Any failures after this point will clear out this TypeNewScript.
    DestroyTypeNewScript destroyNewScript(cx, group);

    // Compute the greatest common shape prefix and the largest slot span of
    // the preliminary objects.
    Shape* prefixShape = nullptr;
    size_t maxSlotSpan = 0;
    for (size_t i = 0; i < PreliminaryObjectArray::COUNT; i++) {
        JSObject* objBase = preliminaryObjects->get(i);
        if (!objBase)
            continue;
        PlainObject* obj = &objBase->as<PlainObject>();

        // For now, we require all preliminary objects to have only simple
        // lineages of plain data properties.
        Shape* shape = obj->lastProperty();
        if (shape->inDictionary() ||
            !OnlyHasDataProperties(shape) ||
            shape->getObjectFlags() != 0 ||
            shape->getObjectMetadata() != nullptr)
        {
            return true;
        }

        maxSlotSpan = Max<size_t>(maxSlotSpan, obj->slotSpan());

        if (prefixShape) {
            MOZ_ASSERT(shape->numFixedSlots() == prefixShape->numFixedSlots());
            prefixShape = CommonPrefix(prefixShape, shape);
        } else {
            prefixShape = shape;
        }
        if (prefixShape->isEmptyShape()) {
            // The preliminary objects don't have any common properties.
            return true;
        }
    }
    if (!prefixShape)
        return true;

    gc::AllocKind kind = gc::GetGCObjectKind(maxSlotSpan);

    if (kind != gc::GetGCObjectKind(NativeObject::MAX_FIXED_SLOTS)) {
        // The template object will have a different allocation kind from the
        // preliminary objects that have already been constructed. Optimizing
        // definite property accesses requires both that the property is
        // definitely in a particular slot and that the object has a specific
        // number of fixed slots. So, adjust the shape and slot layout of all
        // the preliminary objects so that their structure matches that of the
        // template object. Also recompute the prefix shape, as it reflects the
        // old number of fixed slots.
        Shape* newPrefixShape = nullptr;
        for (size_t i = 0; i < PreliminaryObjectArray::COUNT; i++) {
            JSObject* objBase = preliminaryObjects->get(i);
            if (!objBase)
                continue;
            PlainObject* obj = &objBase->as<PlainObject>();
            if (!ChangeObjectFixedSlotCount(cx, obj, kind))
                return false;
            if (newPrefixShape) {
                MOZ_ASSERT(CommonPrefix(obj->lastProperty(), newPrefixShape) == newPrefixShape);
            } else {
                newPrefixShape = obj->lastProperty();
                while (newPrefixShape->slotSpan() > prefixShape->slotSpan())
                    newPrefixShape = newPrefixShape->previous();
            }
        }
        prefixShape = newPrefixShape;
    }

    RootedObjectGroup groupRoot(cx, group);
    templateObject_ = NewObjectWithGroup<PlainObject>(cx, groupRoot, cx->global(), kind,
                                                      MaybeSingletonObject);
    if (!templateObject_)
        return false;

    Vector<Initializer> initializerVector(cx);

    RootedPlainObject templateRoot(cx, templateObject());
    if (!jit::AnalyzeNewScriptDefiniteProperties(cx, function(), group, templateRoot, &initializerVector))
        return false;

    if (!group->newScript())
        return true;

    MOZ_ASSERT(OnlyHasDataProperties(templateObject()->lastProperty()));

    if (templateObject()->slotSpan() != 0) {
        // Make sure that all definite properties found are reflected in the
        // prefix shape. Otherwise, the constructor behaved differently before
        // we baseline compiled it and started observing types. Compare
        // property names rather than looking at the shapes directly, as the
        // allocation kind and other non-property parts of the template and
        // existing objects may differ.
        if (templateObject()->slotSpan() > prefixShape->slotSpan())
            return true;
        {
            Shape* shape = prefixShape;
            while (shape->slotSpan() != templateObject()->slotSpan())
                shape = shape->previous();
            Shape* templateShape = templateObject()->lastProperty();
            while (!shape->isEmptyShape()) {
                if (shape->slot() != templateShape->slot())
                    return true;
                if (shape->propid() != templateShape->propid())
                    return true;
                shape = shape->previous();
                templateShape = templateShape->previous();
            }
            if (!templateShape->isEmptyShape())
                return true;
        }

        Initializer done(Initializer::DONE, 0);

        if (!initializerVector.append(done))
            return false;

        initializerList = group->zone()->pod_calloc<Initializer>(initializerVector.length());
        if (!initializerList)
            return false;
        PodCopy(initializerList, initializerVector.begin(), initializerVector.length());
    }

    // Try to use an unboxed representation for the group.
    if (!TryConvertToUnboxedLayout(cx, templateObject()->lastProperty(), group, preliminaryObjects))
        return false;

    js_delete(preliminaryObjects);
    preliminaryObjects = nullptr;

    if (group->maybeUnboxedLayout()) {
        // An unboxed layout was constructed for the group, and this has already
        // been hooked into it.
        MOZ_ASSERT(group->unboxedLayout().newScript() == this);
        destroyNewScript.group = nullptr;

        // Clear out the template object, which is not used for TypeNewScripts
        // with an unboxed layout. Currently it is a mutant object with a
        // non-native group and native shape, so make it safe for GC by changing
        // its group to the default for its prototype.
        ObjectGroup* plainGroup = ObjectGroup::defaultNewGroup(cx, &PlainObject::class_,
                                                               group->proto());
        if (!plainGroup)
            CrashAtUnhandlableOOM("TypeNewScript::maybeAnalyze");
        templateObject_->setGroup(plainGroup);
        templateObject_ = nullptr;

        return true;
    }

    if (prefixShape->slotSpan() == templateObject()->slotSpan()) {
        // The definite properties analysis found exactly the properties that
        // are held in common by the preliminary objects. No further analysis
        // is needed.
        if (!group->addDefiniteProperties(cx, templateObject()->lastProperty()))
            return false;

        destroyNewScript.group = nullptr;
        return true;
    }

    // There are more properties consistently added to objects of this group
    // than were discovered by the definite properties analysis. Use the
    // existing group to represent fully initialized objects with all
    // definite properties in the prefix shape, and make a new group to
    // represent partially initialized objects.
    MOZ_ASSERT(prefixShape->slotSpan() > templateObject()->slotSpan());

    ObjectGroupFlags initialFlags = group->flags() & OBJECT_FLAG_DYNAMIC_MASK;

    Rooted<TaggedProto> protoRoot(cx, group->proto());
    ObjectGroup* initialGroup = ObjectGroupCompartment::makeGroup(cx, group->clasp(), protoRoot,
                                                                  initialFlags);
    if (!initialGroup)
        return false;

    if (!initialGroup->addDefiniteProperties(cx, templateObject()->lastProperty()))
        return false;
    if (!group->addDefiniteProperties(cx, prefixShape))
        return false;

    cx->compartment()->objectGroups.replaceDefaultNewGroup(nullptr, group->proto(), function(),
                                                           initialGroup);

    templateObject()->setGroup(initialGroup);

    // Transfer this TypeNewScript from the fully initialized group to the
    // partially initialized group.
    group->setNewScript(nullptr);
    initialGroup->setNewScript(this);

    initializedShape_ = prefixShape;
    initializedGroup_ = group;

    destroyNewScript.group = nullptr;

    if (regenerate)
        *regenerate = true;
    return true;
}

bool
TypeNewScript::rollbackPartiallyInitializedObjects(JSContext* cx, ObjectGroup* group)
{
    // If we cleared this new script while in the middle of initializing an
    // object, it will still have the new script's shape and reflect the no
    // longer correct state of the object once its initialization is completed.
    // We can't detect the possibility of this statically while remaining
    // robust, but the new script keeps track of where each property is
    // initialized so we can walk the stack and fix up any such objects.
    // Return whether any objects were modified.

    if (!initializerList)
        return false;

    bool found = false;

    RootedFunction function(cx, this->function());
    Vector<uint32_t, 32> pcOffsets(cx);
    for (ScriptFrameIter iter(cx); !iter.done(); ++iter) {
        pcOffsets.append(iter.script()->pcToOffset(iter.pc()));

        if (!iter.isConstructing() || !iter.matchCallee(cx, function))
            continue;

        Value thisv = iter.thisv(cx);
        if (!thisv.isObject() ||
            thisv.toObject().hasLazyGroup() ||
            thisv.toObject().group() != group)
        {
            continue;
        }

        if (thisv.toObject().is<UnboxedPlainObject>() &&
            !UnboxedPlainObject::convertToNative(cx, &thisv.toObject()))
        {
            CrashAtUnhandlableOOM("rollbackPartiallyInitializedObjects");
        }

        // Found a matching frame.
        RootedPlainObject obj(cx, &thisv.toObject().as<PlainObject>());

        // Whether all identified 'new' properties have been initialized.
        bool finished = false;

        // If not finished, number of properties that have been added.
        uint32_t numProperties = 0;

        // Whether the current SETPROP is within an inner frame which has
        // finished entirely.
        bool pastProperty = false;

        // Index in pcOffsets of the outermost frame.
        int callDepth = pcOffsets.length() - 1;

        // Index in pcOffsets of the frame currently being checked for a SETPROP.
        int setpropDepth = callDepth;

        for (Initializer* init = initializerList;; init++) {
            if (init->kind == Initializer::SETPROP) {
                if (!pastProperty && pcOffsets[setpropDepth] < init->offset) {
                    // Have not yet reached this setprop.
                    break;
                }
                // This setprop has executed, reset state for the next one.
                numProperties++;
                pastProperty = false;
                setpropDepth = callDepth;
            } else if (init->kind == Initializer::SETPROP_FRAME) {
                if (!pastProperty) {
                    if (pcOffsets[setpropDepth] < init->offset) {
                        // Have not yet reached this inner call.
                        break;
                    } else if (pcOffsets[setpropDepth] > init->offset) {
                        // Have advanced past this inner call.
                        pastProperty = true;
                    } else if (setpropDepth == 0) {
                        // Have reached this call but not yet in it.
                        break;
                    } else {
                        // Somewhere inside this inner call.
                        setpropDepth--;
                    }
                }
            } else {
                MOZ_ASSERT(init->kind == Initializer::DONE);
                finished = true;
                break;
            }
        }

        if (!finished) {
            (void) NativeObject::rollbackProperties(cx, obj, numProperties);
            found = true;
        }
    }

    return found;
}

void
TypeNewScript::trace(JSTracer* trc)
{
    MarkObject(trc, &function_, "TypeNewScript_function");

    if (templateObject_)
        MarkObject(trc, &templateObject_, "TypeNewScript_templateObject");

    if (initializedShape_)
        MarkShape(trc, &initializedShape_, "TypeNewScript_initializedShape");

    if (initializedGroup_)
        MarkObjectGroup(trc, &initializedGroup_, "TypeNewScript_initializedGroup");
}

void
TypeNewScript::sweep()
{
    if (preliminaryObjects)
        preliminaryObjects->sweep();
}

/////////////////////////////////////////////////////////////////////
// Tracing
/////////////////////////////////////////////////////////////////////

void
ConstraintTypeSet::sweep(Zone* zone, AutoClearTypeInferenceStateOnOOM& oom)
{
    MOZ_ASSERT(zone->isGCSweepingOrCompacting());

    // IsAboutToBeFinalized doesn't work right on tenured objects when called
    // during a minor collection.
    MOZ_ASSERT(!zone->runtimeFromMainThread()->isHeapMinorCollecting());

    /*
     * Purge references to objects that are no longer live. Type sets hold
     * only weak references. For type sets containing more than one object,
     * live entries in the object hash need to be copied to the zone's
     * new arena.
     */
    unsigned objectCount = baseObjectCount();
    if (objectCount >= 2) {
        unsigned oldCapacity = TypeHashSet::Capacity(objectCount);
        ObjectKey** oldArray = objectSet;

        clearObjects();
        objectCount = 0;
        for (unsigned i = 0; i < oldCapacity; i++) {
            ObjectKey* key = oldArray[i];
            if (!key)
                continue;
            if (!IsAboutToBeFinalized(&key)) {
                ObjectKey** pentry =
                    TypeHashSet::Insert<ObjectKey*, ObjectKey, ObjectKey>
                        (zone->types.typeLifoAlloc, objectSet, objectCount, key);
                if (pentry) {
                    *pentry = key;
                } else {
                    oom.setOOM();
                    flags |= TYPE_FLAG_ANYOBJECT;
                    clearObjects();
                    objectCount = 0;
                    break;
                }
            } else if (key->isGroup() && key->group()->unknownPropertiesDontCheckGeneration()) {
                // Object sets containing objects with unknown properties might
                // not be complete. Mark the type set as unknown, which it will
                // be treated as during Ion compilation.
                //
                // Note that we don't have to do this when the type set might
                // be missing the native group corresponding to an unboxed
                // object group. In this case, the native group points to the
                // unboxed object group via its addendum, so as long as objects
                // with either group exist, neither group will be finalized.
                flags |= TYPE_FLAG_ANYOBJECT;
                clearObjects();
                objectCount = 0;
                break;
            }
        }
        setBaseObjectCount(objectCount);
    } else if (objectCount == 1) {
        ObjectKey* key = (ObjectKey*) objectSet;
        if (!IsAboutToBeFinalized(&key)) {
            objectSet = reinterpret_cast<ObjectKey**>(key);
        } else {
            // As above, mark type sets containing objects with unknown
            // properties as unknown.
            if (key->isGroup() && key->group()->unknownPropertiesDontCheckGeneration())
                flags |= TYPE_FLAG_ANYOBJECT;
            objectSet = nullptr;
            setBaseObjectCount(0);
        }
    }

    /*
     * Type constraints only hold weak references. Copy constraints referring
     * to data that is still live into the zone's new arena.
     */
    TypeConstraint* constraint = constraintList;
    constraintList = nullptr;
    while (constraint) {
        TypeConstraint* copy;
        if (constraint->sweep(zone->types, &copy)) {
            if (copy) {
                copy->next = constraintList;
                constraintList = copy;
            } else {
                oom.setOOM();
            }
        }
        constraint = constraint->next;
    }
}

inline void
ObjectGroup::clearProperties()
{
    setBasePropertyCount(0);
    propertySet = nullptr;
}

#ifdef DEBUG
bool
ObjectGroup::needsSweep()
{
    // Note: this can be called off thread during compacting GCs, in which case
    // nothing will be running on the main thread.
    return generation() != zoneFromAnyThread()->types.generation;
}
#endif

static void
EnsureHasAutoClearTypeInferenceStateOnOOM(AutoClearTypeInferenceStateOnOOM*& oom, Zone* zone,
                                          Maybe<AutoClearTypeInferenceStateOnOOM>& fallback)
{
    if (!oom) {
        if (zone->types.activeAnalysis) {
            oom = &zone->types.activeAnalysis->oom;
        } else {
            fallback.emplace(zone);
            oom = &fallback.ref();
        }
    }
}

/*
 * Before sweeping the arenas themselves, scan all groups in a compartment to
 * fixup weak references: property type sets referencing dead JS and type
 * objects, and singleton JS objects whose type is not referenced elsewhere.
 * This is done either incrementally as part of the sweep, or on demand as type
 * objects are accessed before their contents have been swept.
 */
void
ObjectGroup::maybeSweep(AutoClearTypeInferenceStateOnOOM* oom)
{
    if (generation() == zoneFromAnyThread()->types.generation) {
        // No sweeping required.
        return;
    }

    setGeneration(zone()->types.generation);

    MOZ_ASSERT(zone()->isGCSweepingOrCompacting());
    MOZ_ASSERT(!zone()->runtimeFromMainThread()->isHeapMinorCollecting());

    Maybe<AutoClearTypeInferenceStateOnOOM> fallbackOOM;
    EnsureHasAutoClearTypeInferenceStateOnOOM(oom, zone(), fallbackOOM);

    if (maybeUnboxedLayout() && unboxedLayout().newScript())
        unboxedLayout().newScript()->sweep();

    if (newScript())
        newScript()->sweep();

    LifoAlloc& typeLifoAlloc = zone()->types.typeLifoAlloc;

    /*
     * Properties were allocated from the old arena, and need to be copied over
     * to the new one.
     */
    unsigned propertyCount = basePropertyCount();
    if (propertyCount >= 2) {
        unsigned oldCapacity = TypeHashSet::Capacity(propertyCount);
        Property** oldArray = propertySet;

        clearProperties();
        propertyCount = 0;
        for (unsigned i = 0; i < oldCapacity; i++) {
            Property* prop = oldArray[i];
            if (prop) {
                if (singleton() && !prop->types.constraintList && !zone()->isPreservingCode()) {
                    /*
                     * Don't copy over properties of singleton objects when their
                     * presence will not be required by jitcode or type constraints
                     * (i.e. for the definite properties analysis). The contents of
                     * these type sets will be regenerated as necessary.
                     */
                    continue;
                }

                Property* newProp = typeLifoAlloc.new_<Property>(*prop);
                if (newProp) {
                    Property** pentry = TypeHashSet::Insert<jsid, Property, Property>
                                            (typeLifoAlloc, propertySet, propertyCount, prop->id);
                    if (pentry) {
                        *pentry = newProp;
                        newProp->types.sweep(zone(), *oom);
                        continue;
                    }
                }

                oom->setOOM();
                addFlags(OBJECT_FLAG_DYNAMIC_MASK | OBJECT_FLAG_UNKNOWN_PROPERTIES);
                clearProperties();
                return;
            }
        }
        setBasePropertyCount(propertyCount);
    } else if (propertyCount == 1) {
        Property* prop = (Property*) propertySet;
        if (singleton() && !prop->types.constraintList && !zone()->isPreservingCode()) {
            // Skip, as above.
            clearProperties();
        } else {
            Property* newProp = typeLifoAlloc.new_<Property>(*prop);
            if (newProp) {
                propertySet = (Property**) newProp;
                newProp->types.sweep(zone(), *oom);
            } else {
                oom->setOOM();
                addFlags(OBJECT_FLAG_DYNAMIC_MASK | OBJECT_FLAG_UNKNOWN_PROPERTIES);
                clearProperties();
                return;
            }
        }
    }
}

/* static */ void
JSScript::maybeSweepTypes(AutoClearTypeInferenceStateOnOOM* oom)
{
    if (!types_ || typesGeneration() == zone()->types.generation)
        return;

    setTypesGeneration(zone()->types.generation);

    MOZ_ASSERT(zone()->isGCSweepingOrCompacting());
    MOZ_ASSERT(!zone()->runtimeFromMainThread()->isHeapMinorCollecting());

    Maybe<AutoClearTypeInferenceStateOnOOM> fallbackOOM;
    EnsureHasAutoClearTypeInferenceStateOnOOM(oom, zone(), fallbackOOM);

    TypeZone& types = zone()->types;

    // Destroy all type information attached to the script if desired. We can
    // only do this if nothing has been compiled for the script, which will be
    // the case unless the script has been compiled since we started sweeping.
    if (types.sweepReleaseTypes &&
        !hasBaselineScript() &&
        !hasIonScript())
    {
        types_->destroy();
        types_ = nullptr;

        // Freeze constraints on stack type sets need to be regenerated the
        // next time the script is analyzed.
        hasFreezeConstraints_ = false;

        return;
    }

    unsigned num = TypeScript::NumTypeSets(this);
    StackTypeSet* typeArray = types_->typeArray();

    // Remove constraints and references to dead objects from stack type sets.
    for (unsigned i = 0; i < num; i++)
        typeArray[i].sweep(zone(), *oom);

    if (oom->hadOOM()) {
        // It's possible we OOM'd while copying freeze constraints, so they
        // need to be regenerated.
        hasFreezeConstraints_ = false;
    }

    // Update the recompile indexes in any IonScripts still on the script.
    if (hasIonScript())
        ionScript()->recompileInfoRef().shouldSweep(types);
}

void
TypeScript::destroy()
{
    js_free(this);
}

void
Zone::addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                             size_t* typePool,
                             size_t* baselineStubsOptimized)
{
    *typePool += types.typeLifoAlloc.sizeOfExcludingThis(mallocSizeOf);
    if (jitZone()) {
        *baselineStubsOptimized +=
            jitZone()->optimizedStubSpace()->sizeOfExcludingThis(mallocSizeOf);
    }
}

TypeZone::TypeZone(Zone* zone)
  : zone_(zone),
    typeLifoAlloc(TYPE_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
    generation(0),
    compilerOutputs(nullptr),
    sweepTypeLifoAlloc(TYPE_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
    sweepCompilerOutputs(nullptr),
    sweepReleaseTypes(false),
    activeAnalysis(nullptr)
{
}

TypeZone::~TypeZone()
{
    js_delete(compilerOutputs);
    js_delete(sweepCompilerOutputs);
}

void
TypeZone::beginSweep(FreeOp* fop, bool releaseTypes, AutoClearTypeInferenceStateOnOOM& oom)
{
    MOZ_ASSERT(zone()->isGCSweepingOrCompacting());
    MOZ_ASSERT(!sweepCompilerOutputs);
    MOZ_ASSERT(!sweepReleaseTypes);

    sweepReleaseTypes = releaseTypes;

    // Clear the analysis pool, but don't release its data yet. While sweeping
    // types any live data will be allocated into the pool.
    sweepTypeLifoAlloc.steal(&typeLifoAlloc);

    // Sweep any invalid or dead compiler outputs, and keep track of the new
    // index for remaining live outputs.
    if (compilerOutputs) {
        CompilerOutputVector* newCompilerOutputs = nullptr;
        for (size_t i = 0; i < compilerOutputs->length(); i++) {
            CompilerOutput& output = (*compilerOutputs)[i];
            if (output.isValid()) {
                JSScript* script = output.script();
                if (IsScriptAboutToBeFinalized(&script)) {
                    script->ionScript()->recompileInfoRef() = RecompileInfo();
                    output.invalidate();
                } else {
                    CompilerOutput newOutput(script);

                    if (!newCompilerOutputs)
                        newCompilerOutputs = js_new<CompilerOutputVector>();
                    if (newCompilerOutputs && newCompilerOutputs->append(newOutput)) {
                        output.setSweepIndex(newCompilerOutputs->length() - 1);
                    } else {
                        oom.setOOM();
                        script->ionScript()->recompileInfoRef() = RecompileInfo();
                        output.invalidate();
                    }
                }
            }
        }
        sweepCompilerOutputs = compilerOutputs;
        compilerOutputs = newCompilerOutputs;
    }

    // All existing RecompileInfos are stale and will be updated to the new
    // compiler outputs list later during the sweep. Don't worry about overflow
    // here, since stale indexes will persist only until the sweep finishes.
    generation++;
}

void
TypeZone::endSweep(JSRuntime* rt)
{
    js_delete(sweepCompilerOutputs);
    sweepCompilerOutputs = nullptr;

    sweepReleaseTypes = false;

    rt->gc.freeAllLifoBlocksAfterSweeping(&sweepTypeLifoAlloc);
}

void
TypeZone::clearAllNewScriptsOnOOM()
{
    for (gc::ZoneCellIterUnderGC iter(zone(), gc::FINALIZE_OBJECT_GROUP);
         !iter.done(); iter.next())
    {
        ObjectGroup* group = iter.get<ObjectGroup>();
        if (!IsObjectGroupAboutToBeFinalized(&group))
            group->maybeClearNewScriptOnOOM();
    }
}

AutoClearTypeInferenceStateOnOOM::~AutoClearTypeInferenceStateOnOOM()
{
    if (oom) {
        zone->setPreservingCode(false);
        zone->discardJitCode(zone->runtimeFromMainThread()->defaultFreeOp());
        zone->types.clearAllNewScriptsOnOOM();
    }
}

#ifdef DEBUG
void
TypeScript::printTypes(JSContext* cx, HandleScript script) const
{
    MOZ_ASSERT(script->types() == this);

    if (!script->hasBaselineScript())
        return;

    AutoEnterAnalysis enter(nullptr, script->zone());

    if (script->functionNonDelazifying())
        fprintf(stderr, "Function");
    else if (script->isForEval())
        fprintf(stderr, "Eval");
    else
        fprintf(stderr, "Main");
    fprintf(stderr, " %p %s:%d ", script.get(), script->filename(), (int) script->lineno());

    if (script->functionNonDelazifying()) {
        if (js::PropertyName* name = script->functionNonDelazifying()->name())
            name->dumpCharsNoNewline();
    }

    fprintf(stderr, "\n    this:");
    TypeScript::ThisTypes(script)->print();

    for (unsigned i = 0;
         script->functionNonDelazifying() && i < script->functionNonDelazifying()->nargs();
         i++)
    {
        fprintf(stderr, "\n    arg%u:", i);
        TypeScript::ArgTypes(script, i)->print();
    }
    fprintf(stderr, "\n");

    for (jsbytecode* pc = script->code(); pc < script->codeEnd(); pc += GetBytecodeLength(pc)) {
        {
            fprintf(stderr, "%p:", script.get());
            Sprinter sprinter(cx);
            if (!sprinter.init())
                return;
            js_Disassemble1(cx, script, pc, script->pcToOffset(pc), true, &sprinter);
            fprintf(stderr, "%s", sprinter.string());
        }

        if (js_CodeSpec[*pc].format & JOF_TYPESET) {
            StackTypeSet* types = TypeScript::BytecodeTypes(script, pc);
            fprintf(stderr, "  typeset %u:", unsigned(types - typeArray()));
            types->print();
            fprintf(stderr, "\n");
        }
    }

    fprintf(stderr, "\n");
}
#endif /* DEBUG */
