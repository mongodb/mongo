/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ObjectGroup.h"

#include "jshashutil.h"
#include "jsobj.h"

#include "gc/StoreBuffer.h"
#include "gc/Zone.h"
#include "vm/ArrayObject.h"
#include "vm/UnboxedObject.h"

#include "jsobjinlines.h"

#include "vm/UnboxedObject-inl.h"

using namespace js;

using mozilla::DebugOnly;
using mozilla::PodZero;

/////////////////////////////////////////////////////////////////////
// ObjectGroup
/////////////////////////////////////////////////////////////////////

ObjectGroup::ObjectGroup(const Class* clasp, TaggedProto proto, JSCompartment* comp,
                         ObjectGroupFlags initialFlags)
{
    PodZero(this);

    /* Windows may not appear on prototype chains. */
    MOZ_ASSERT_IF(proto.isObject(), !IsWindow(proto.toObject()));

    this->clasp_ = clasp;
    this->proto_ = proto;
    this->compartment_ = comp;
    this->flags_ = initialFlags;

    setGeneration(zone()->types.generation);
}

void
ObjectGroup::finalize(FreeOp* fop)
{
    if (newScriptDontCheckGeneration())
        newScriptDontCheckGeneration()->clear();
    fop->delete_(newScriptDontCheckGeneration());
    fop->delete_(maybeUnboxedLayoutDontCheckGeneration());
    if (maybePreliminaryObjectsDontCheckGeneration())
        maybePreliminaryObjectsDontCheckGeneration()->clear();
    fop->delete_(maybePreliminaryObjectsDontCheckGeneration());
}

void
ObjectGroup::setProtoUnchecked(TaggedProto proto)
{
    proto_ = proto;
    MOZ_ASSERT_IF(proto_.isObject() && proto_.toObject()->isNative(),
                  proto_.toObject()->isDelegate());
}

void
ObjectGroup::setProto(TaggedProto proto)
{
    MOZ_ASSERT(singleton());
    setProtoUnchecked(proto);
}

size_t
ObjectGroup::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const
{
    size_t n = 0;
    if (TypeNewScript* newScript = newScriptDontCheckGeneration())
        n += newScript->sizeOfIncludingThis(mallocSizeOf);
    if (UnboxedLayout* layout = maybeUnboxedLayoutDontCheckGeneration())
        n += layout->sizeOfIncludingThis(mallocSizeOf);
    return n;
}

void
ObjectGroup::setAddendum(AddendumKind kind, void* addendum, bool writeBarrier /* = true */)
{
    MOZ_ASSERT(!needsSweep());
    MOZ_ASSERT(kind <= (OBJECT_FLAG_ADDENDUM_MASK >> OBJECT_FLAG_ADDENDUM_SHIFT));

    if (writeBarrier) {
        // Manually trigger barriers if we are clearing new script or
        // preliminary object information. Other addendums are immutable.
        switch (addendumKind()) {
          case Addendum_PreliminaryObjects:
            PreliminaryObjectArrayWithTemplate::writeBarrierPre(maybePreliminaryObjects());
            break;
          case Addendum_NewScript:
            TypeNewScript::writeBarrierPre(newScript());
            break;
          case Addendum_None:
            break;
          default:
            MOZ_ASSERT(addendumKind() == kind);
        }
    }

    flags_ &= ~OBJECT_FLAG_ADDENDUM_MASK;
    flags_ |= kind << OBJECT_FLAG_ADDENDUM_SHIFT;
    addendum_ = addendum;
}

/* static */ bool
ObjectGroup::useSingletonForClone(JSFunction* fun)
{
    if (!fun->isInterpreted())
        return false;

    if (fun->isArrow())
        return false;

    if (fun->isSingleton())
        return false;

    /*
     * When a function is being used as a wrapper for another function, it
     * improves precision greatly to distinguish between different instances of
     * the wrapper; otherwise we will conflate much of the information about
     * the wrapped functions.
     *
     * An important example is the Class.create function at the core of the
     * Prototype.js library, which looks like:
     *
     * var Class = {
     *   create: function() {
     *     return function() {
     *       this.initialize.apply(this, arguments);
     *     }
     *   }
     * };
     *
     * Each instance of the innermost function will have a different wrapped
     * initialize method. We capture this, along with similar cases, by looking
     * for short scripts which use both .apply and arguments. For such scripts,
     * whenever creating a new instance of the function we both give that
     * instance a singleton type and clone the underlying script.
     */

    uint32_t begin, end;
    if (fun->hasScript()) {
        if (!fun->nonLazyScript()->usesArgumentsApplyAndThis())
            return false;
        begin = fun->nonLazyScript()->sourceStart();
        end = fun->nonLazyScript()->sourceEnd();
    } else {
        if (!fun->lazyScript()->usesArgumentsApplyAndThis())
            return false;
        begin = fun->lazyScript()->begin();
        end = fun->lazyScript()->end();
    }

    return end - begin <= 100;
}

/* static */ bool
ObjectGroup::useSingletonForNewObject(JSContext* cx, JSScript* script, jsbytecode* pc)
{
    /*
     * Make a heuristic guess at a use of JSOP_NEW that the constructed object
     * should have a fresh group. We do this when the NEW is immediately
     * followed by a simple assignment to an object's .prototype field.
     * This is designed to catch common patterns for subclassing in JS:
     *
     * function Super() { ... }
     * function Sub1() { ... }
     * function Sub2() { ... }
     *
     * Sub1.prototype = new Super();
     * Sub2.prototype = new Super();
     *
     * Using distinct groups for the particular prototypes of Sub1 and
     * Sub2 lets us continue to distinguish the two subclasses and any extra
     * properties added to those prototype objects.
     */
    if (script->isGenerator())
        return false;
    if (JSOp(*pc) != JSOP_NEW)
        return false;
    pc += JSOP_NEW_LENGTH;
    if (JSOp(*pc) == JSOP_SETPROP) {
        if (script->getName(pc) == cx->names().prototype)
            return true;
    }
    return false;
}

/* static */ bool
ObjectGroup::useSingletonForAllocationSite(JSScript* script, jsbytecode* pc, JSProtoKey key)
{
    // The return value of this method can either be tested like a boolean or
    // passed to a NewObject method.
    JS_STATIC_ASSERT(GenericObject == 0);

    /*
     * Objects created outside loops in global and eval scripts should have
     * singleton types. For now this is only done for plain objects and typed
     * arrays, but not normal arrays.
     */

    if (script->functionNonDelazifying() && !script->treatAsRunOnce())
        return GenericObject;

    if (key != JSProto_Object &&
        !(key >= JSProto_Int8Array && key <= JSProto_Uint8ClampedArray))
    {
        return GenericObject;
    }

    // All loops in the script will have a try note indicating their boundary.

    if (!script->hasTrynotes())
        return SingletonObject;

    unsigned offset = script->pcToOffset(pc);

    JSTryNote* tn = script->trynotes()->vector;
    JSTryNote* tnlimit = tn + script->trynotes()->length;
    for (; tn < tnlimit; tn++) {
        if (tn->kind != JSTRY_FOR_IN && tn->kind != JSTRY_FOR_OF && tn->kind != JSTRY_LOOP)
            continue;

        unsigned startOffset = script->mainOffset() + tn->start;
        unsigned endOffset = startOffset + tn->length;

        if (offset >= startOffset && offset < endOffset)
            return GenericObject;
    }

    return SingletonObject;
}

/* static */ bool
ObjectGroup::useSingletonForAllocationSite(JSScript* script, jsbytecode* pc, const Class* clasp)
{
    return useSingletonForAllocationSite(script, pc, JSCLASS_CACHED_PROTO_KEY(clasp));
}

/////////////////////////////////////////////////////////////////////
// JSObject
/////////////////////////////////////////////////////////////////////

bool
JSObject::shouldSplicePrototype(JSContext* cx)
{
    /*
     * During bootstrapping, if inference is enabled we need to make sure not
     * to splice a new prototype in for Function.prototype or the global
     * object if their __proto__ had previously been set to null, as this
     * will change the prototype for all other objects with the same type.
     */
    if (getProto() != nullptr)
        return false;
    return isSingleton();
}

bool
JSObject::splicePrototype(JSContext* cx, const Class* clasp, Handle<TaggedProto> proto)
{
    MOZ_ASSERT(cx->compartment() == compartment());

    RootedObject self(cx, this);

    /*
     * For singleton groups representing only a single JSObject, the proto
     * can be rearranged as needed without destroying type information for
     * the old or new types.
     */
    MOZ_ASSERT(self->isSingleton());

    // Windows may not appear on prototype chains.
    MOZ_ASSERT_IF(proto.isObject(), !IsWindow(proto.toObject()));

    if (proto.isObject() && !proto.toObject()->setDelegate(cx))
        return false;

    // Force type instantiation when splicing lazy group.
    RootedObjectGroup group(cx, self->getGroup(cx));
    if (!group)
        return false;
    RootedObjectGroup protoGroup(cx, nullptr);
    if (proto.isObject()) {
        protoGroup = proto.toObject()->getGroup(cx);
        if (!protoGroup)
            return false;
    }

    group->setClasp(clasp);
    group->setProto(proto);
    return true;
}

/* static */ ObjectGroup*
JSObject::makeLazyGroup(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(obj->hasLazyGroup());
    MOZ_ASSERT(cx->compartment() == obj->compartment());

    /* De-lazification of functions can GC, so we need to do it up here. */
    if (obj->is<JSFunction>() && obj->as<JSFunction>().isInterpretedLazy()) {
        RootedFunction fun(cx, &obj->as<JSFunction>());
        if (!fun->getOrCreateScript(cx))
            return nullptr;
    }

    // Find flags which need to be specified immediately on the object.
    // Don't track whether singletons are packed.
    ObjectGroupFlags initialFlags = OBJECT_FLAG_SINGLETON | OBJECT_FLAG_NON_PACKED;

    if (obj->isIteratedSingleton())
        initialFlags |= OBJECT_FLAG_ITERATED;

    if (obj->isIndexed())
        initialFlags |= OBJECT_FLAG_SPARSE_INDEXES;

    if (obj->is<ArrayObject>() && obj->as<ArrayObject>().length() > INT32_MAX)
        initialFlags |= OBJECT_FLAG_LENGTH_OVERFLOW;

    Rooted<TaggedProto> proto(cx, obj->getTaggedProto());
    ObjectGroup* group = ObjectGroupCompartment::makeGroup(cx, obj->getClass(), proto,
                                                           initialFlags);
    if (!group)
        return nullptr;

    AutoEnterAnalysis enter(cx);

    /* Fill in the type according to the state of this object. */

    if (obj->is<JSFunction>() && obj->as<JSFunction>().isInterpreted())
        group->setInterpretedFunction(&obj->as<JSFunction>());

    obj->group_ = group;

    return group;
}

/* static */ bool
JSObject::setNewGroupUnknown(JSContext* cx, const js::Class* clasp, JS::HandleObject obj)
{
    ObjectGroup::setDefaultNewGroupUnknown(cx, clasp, obj);
    return obj->setFlags(cx, BaseShape::NEW_GROUP_UNKNOWN);
}

/////////////////////////////////////////////////////////////////////
// ObjectGroupCompartment NewTable
/////////////////////////////////////////////////////////////////////

/*
 * Entries for the per-compartment set of groups which are the default
 * types to use for some prototype. An optional associated object is used which
 * allows multiple groups to be created with the same prototype. The
 * associated object may be a function (for types constructed with 'new') or a
 * type descriptor (for typed objects). These entries are also used for the set
 * of lazy groups in the compartment, which use a null associated object
 * (though there are only a few of these per compartment).
 */
struct ObjectGroupCompartment::NewEntry
{
    ReadBarrieredObjectGroup group;

    // Note: This pointer is only used for equality and does not need a read barrier.
    JSObject* associated;

    NewEntry(ObjectGroup* group, JSObject* associated)
      : group(group), associated(associated)
    {}

    struct Lookup {
        const Class* clasp;
        TaggedProto hashProto;
        TaggedProto matchProto;
        JSObject* associated;

        Lookup(const Class* clasp, TaggedProto proto, JSObject* associated)
          : clasp(clasp), hashProto(proto), matchProto(proto), associated(associated)
        {}

        /*
         * For use by generational post barriers only.  Look up an entry whose
         * proto has been moved, but was hashed with the original value.
         */
        Lookup(const Class* clasp, TaggedProto hashProto, TaggedProto matchProto, JSObject* associated)
            : clasp(clasp), hashProto(hashProto), matchProto(matchProto), associated(associated)
        {}
    };

    static inline HashNumber hash(const Lookup& lookup) {
        return PointerHasher<JSObject*, 3>::hash(lookup.hashProto.raw()) ^
               PointerHasher<const Class*, 3>::hash(lookup.clasp) ^
               PointerHasher<JSObject*, 3>::hash(lookup.associated);
    }

    static inline bool match(const NewEntry& key, const Lookup& lookup) {
        return key.group.unbarrieredGet()->proto() == lookup.matchProto &&
               (!lookup.clasp || key.group.unbarrieredGet()->clasp() == lookup.clasp) &&
               key.associated == lookup.associated;
    }

    static void rekey(NewEntry& k, const NewEntry& newKey) { k = newKey; }
};

// This class is used to add a post barrier on a NewTable entry, as the key is
// calculated from a prototype object which may be moved by generational GC.
class ObjectGroupCompartment::NewTableRef : public gc::BufferableRef
{
    NewTable* table;
    const Class* clasp;
    JSObject* proto;
    JSObject* associated;

  public:
    NewTableRef(NewTable* table, const Class* clasp, JSObject* proto, JSObject* associated)
        : table(table), clasp(clasp), proto(proto), associated(associated)
    {}

    void trace(JSTracer* trc) override {
        JSObject* prior = proto;
        TraceManuallyBarrieredEdge(trc, &proto, "newObjectGroups set prototype");
        if (prior == proto)
            return;

        NewTable::Ptr p = table->lookup(NewTable::Lookup(clasp, TaggedProto(prior),
                                                         TaggedProto(proto),
                                                         associated));
        if (!p)
            return;

        table->rekeyAs(NewTable::Lookup(clasp, TaggedProto(prior), TaggedProto(proto), associated),
                       NewTable::Lookup(clasp, TaggedProto(proto), associated), *p);
    }
};

/* static */ void
ObjectGroupCompartment::newTablePostBarrier(ExclusiveContext* cx, NewTable* table,
                                            const Class* clasp, TaggedProto proto,
                                            JSObject* associated)
{
    MOZ_ASSERT_IF(associated, !IsInsideNursery(associated));

    if (!proto.isObject())
        return;

    if (!cx->isJSContext()) {
        MOZ_ASSERT(!IsInsideNursery(proto.toObject()));
        return;
    }

    if (IsInsideNursery(proto.toObject())) {
        gc::StoreBuffer& sb = cx->asJSContext()->runtime()->gc.storeBuffer;
        sb.putGeneric(NewTableRef(table, clasp, proto.toObject(), associated));
    }
}

/* static */ ObjectGroup*
ObjectGroup::defaultNewGroup(ExclusiveContext* cx, const Class* clasp,
                             TaggedProto proto, JSObject* associated)
{
    MOZ_ASSERT_IF(associated, proto.isObject());
    MOZ_ASSERT_IF(proto.isObject(), cx->isInsideCurrentCompartment(proto.toObject()));

    // A null lookup clasp is used for 'new' groups with an associated
    // function. The group starts out as a plain object but might mutate into an
    // unboxed plain object.
    MOZ_ASSERT_IF(!clasp, !!associated);

    AutoEnterAnalysis enter(cx);

    ObjectGroupCompartment::NewTable*& table = cx->compartment()->objectGroups.defaultNewTable;

    if (!table) {
        table = cx->new_<ObjectGroupCompartment::NewTable>();
        if (!table || !table->init()) {
            js_delete(table);
            table = nullptr;
            ReportOutOfMemory(cx);
            return nullptr;
        }
    }

    if (associated && !associated->is<TypeDescr>()) {
        MOZ_ASSERT(!clasp);
        if (associated->is<JSFunction>()) {

            // Canonicalize new functions to use the original one associated with its script.
            JSFunction* fun = &associated->as<JSFunction>();
            if (fun->hasScript())
                associated = fun->nonLazyScript()->functionNonDelazifying();
            else if (fun->isInterpretedLazy() && !fun->isSelfHostedBuiltin())
                associated = fun->lazyScript()->functionNonDelazifying();
            else
                associated = nullptr;

            // If we have previously cleared the 'new' script information for this
            // function, don't try to construct another one.
            if (associated && associated->wasNewScriptCleared())
                associated = nullptr;

        } else {
            associated = nullptr;
        }

        if (!associated)
            clasp = &PlainObject::class_;
    }

    if (proto.isObject() && !proto.toObject()->isDelegate()) {
        RootedObject protoObj(cx, proto.toObject());
        if (!protoObj->setDelegate(cx))
            return nullptr;

        // Objects which are prototypes of one another should be singletons, so
        // that their type information can be tracked more precisely. Limit
        // this group change to plain objects, to avoid issues with other types
        // of singletons like typed arrays.
        if (protoObj->is<PlainObject>() && !protoObj->isSingleton()) {
            if (!JSObject::changeToSingleton(cx->asJSContext(), protoObj))
                return nullptr;
        }
    }

    ObjectGroupCompartment::NewTable::AddPtr p =
        table->lookupForAdd(ObjectGroupCompartment::NewEntry::Lookup(clasp, proto, associated));
    if (p) {
        ObjectGroup* group = p->group;
        MOZ_ASSERT_IF(clasp, group->clasp() == clasp);
        MOZ_ASSERT_IF(!clasp, group->clasp() == &PlainObject::class_ ||
                              group->clasp() == &UnboxedPlainObject::class_);
        MOZ_ASSERT(group->proto() == proto);
        return group;
    }

    ObjectGroupFlags initialFlags = 0;
    if (!proto.isObject() || proto.toObject()->isNewGroupUnknown())
        initialFlags = OBJECT_FLAG_DYNAMIC_MASK;

    Rooted<TaggedProto> protoRoot(cx, proto);
    ObjectGroup* group = ObjectGroupCompartment::makeGroup(cx, clasp ? clasp : &PlainObject::class_,
                                                           protoRoot, initialFlags);
    if (!group)
        return nullptr;

    if (!table->add(p, ObjectGroupCompartment::NewEntry(group, associated))) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    ObjectGroupCompartment::newTablePostBarrier(cx, table, clasp, proto, associated);

    if (proto.isObject()) {
        RootedObject obj(cx, proto.toObject());

        if (associated) {
            if (associated->is<JSFunction>()) {
                if (!TypeNewScript::make(cx->asJSContext(), group, &associated->as<JSFunction>()))
                    return nullptr;
            } else {
                group->setTypeDescr(&associated->as<TypeDescr>());
            }
        }

        /*
         * Some builtin objects have slotful native properties baked in at
         * creation via the Shape::{insert,get}initialShape mechanism. Since
         * these properties are never explicitly defined on new objects, update
         * the type information for them here.
         */

        const JSAtomState& names = cx->names();

        if (obj->is<RegExpObject>()) {
            AddTypePropertyId(cx, group, nullptr, NameToId(names.source), TypeSet::StringType());
            AddTypePropertyId(cx, group, nullptr, NameToId(names.global), TypeSet::BooleanType());
            AddTypePropertyId(cx, group, nullptr, NameToId(names.ignoreCase), TypeSet::BooleanType());
            AddTypePropertyId(cx, group, nullptr, NameToId(names.multiline), TypeSet::BooleanType());
            AddTypePropertyId(cx, group, nullptr, NameToId(names.sticky), TypeSet::BooleanType());
            AddTypePropertyId(cx, group, nullptr, NameToId(names.lastIndex), TypeSet::Int32Type());
        }

        if (obj->is<StringObject>())
            AddTypePropertyId(cx, group, nullptr, NameToId(names.length), TypeSet::Int32Type());

        if (obj->is<ErrorObject>()) {
            AddTypePropertyId(cx, group, nullptr, NameToId(names.fileName), TypeSet::StringType());
            AddTypePropertyId(cx, group, nullptr, NameToId(names.lineNumber), TypeSet::Int32Type());
            AddTypePropertyId(cx, group, nullptr, NameToId(names.columnNumber), TypeSet::Int32Type());
            AddTypePropertyId(cx, group, nullptr, NameToId(names.stack), TypeSet::StringType());
        }
    }

    return group;
}

/* static */ ObjectGroup*
ObjectGroup::lazySingletonGroup(ExclusiveContext* cx, const Class* clasp, TaggedProto proto)
{
    MOZ_ASSERT_IF(proto.isObject(), cx->compartment() == proto.toObject()->compartment());

    ObjectGroupCompartment::NewTable*& table = cx->compartment()->objectGroups.lazyTable;

    if (!table) {
        table = cx->new_<ObjectGroupCompartment::NewTable>();
        if (!table || !table->init()) {
            ReportOutOfMemory(cx);
            js_delete(table);
            table = nullptr;
            return nullptr;
        }
    }

    ObjectGroupCompartment::NewTable::AddPtr p =
        table->lookupForAdd(ObjectGroupCompartment::NewEntry::Lookup(clasp, proto, nullptr));
    if (p) {
        ObjectGroup* group = p->group;
        MOZ_ASSERT(group->lazy());

        return group;
    }

    AutoEnterAnalysis enter(cx);

    Rooted<TaggedProto> protoRoot(cx, proto);
    ObjectGroup* group =
        ObjectGroupCompartment::makeGroup(cx, clasp, protoRoot,
                                          OBJECT_FLAG_SINGLETON | OBJECT_FLAG_LAZY_SINGLETON);
    if (!group)
        return nullptr;

    if (!table->add(p, ObjectGroupCompartment::NewEntry(group, nullptr))) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    ObjectGroupCompartment::newTablePostBarrier(cx, table, clasp, proto, nullptr);

    return group;
}

/* static */ void
ObjectGroup::setDefaultNewGroupUnknown(JSContext* cx, const Class* clasp, HandleObject obj)
{
    // If the object already has a new group, mark that group as unknown.
    ObjectGroupCompartment::NewTable* table = cx->compartment()->objectGroups.defaultNewTable;
    if (table) {
        Rooted<TaggedProto> taggedProto(cx, TaggedProto(obj));
        ObjectGroupCompartment::NewTable::Ptr p =
            table->lookup(ObjectGroupCompartment::NewEntry::Lookup(clasp, taggedProto, nullptr));
        if (p)
            MarkObjectGroupUnknownProperties(cx, p->group);
    }
}

#ifdef DEBUG
/* static */ bool
ObjectGroup::hasDefaultNewGroup(JSObject* proto, const Class* clasp, ObjectGroup* group)
{
    ObjectGroupCompartment::NewTable* table = proto->compartment()->objectGroups.defaultNewTable;

    if (table) {
        ObjectGroupCompartment::NewTable::Ptr p =
            table->lookup(ObjectGroupCompartment::NewEntry::Lookup(clasp, TaggedProto(proto), nullptr));
        return p && p->group == group;
    }
    return false;
}
#endif /* DEBUG */

inline const Class*
GetClassForProtoKey(JSProtoKey key)
{
    switch (key) {
      case JSProto_Null:
      case JSProto_Object:
        return &PlainObject::class_;
      case JSProto_Array:
        return &ArrayObject::class_;

      case JSProto_Number:
        return &NumberObject::class_;
      case JSProto_Boolean:
        return &BooleanObject::class_;
      case JSProto_String:
        return &StringObject::class_;
      case JSProto_Symbol:
        return &SymbolObject::class_;
      case JSProto_RegExp:
        return &RegExpObject::class_;

      case JSProto_Int8Array:
      case JSProto_Uint8Array:
      case JSProto_Int16Array:
      case JSProto_Uint16Array:
      case JSProto_Int32Array:
      case JSProto_Uint32Array:
      case JSProto_Float32Array:
      case JSProto_Float64Array:
      case JSProto_Uint8ClampedArray:
        return &TypedArrayObject::classes[key - JSProto_Int8Array];

      case JSProto_ArrayBuffer:
        return &ArrayBufferObject::class_;

      case JSProto_SharedArrayBuffer:
        return &SharedArrayBufferObject::class_;

      case JSProto_DataView:
        return &DataViewObject::class_;

      default:
        MOZ_CRASH("Bad proto key");
    }
}

/* static */ ObjectGroup*
ObjectGroup::defaultNewGroup(JSContext* cx, JSProtoKey key)
{
    RootedObject proto(cx);
    if (key != JSProto_Null && !GetBuiltinPrototype(cx, key, &proto))
        return nullptr;
    return defaultNewGroup(cx, GetClassForProtoKey(key), TaggedProto(proto.get()));
}

/////////////////////////////////////////////////////////////////////
// ObjectGroupCompartment ArrayObjectTable
/////////////////////////////////////////////////////////////////////

struct ObjectGroupCompartment::ArrayObjectKey : public DefaultHasher<ArrayObjectKey>
{
    TypeSet::Type type;

    ArrayObjectKey()
      : type(TypeSet::UndefinedType())
    {}

    explicit ArrayObjectKey(TypeSet::Type type)
      : type(type)
    {}

    static inline uint32_t hash(const ArrayObjectKey& v) {
        return v.type.raw();
    }

    static inline bool match(const ArrayObjectKey& v1, const ArrayObjectKey& v2) {
        return v1.type == v2.type;
    }

    bool operator==(const ArrayObjectKey& other) {
        return type == other.type;
    }

    bool operator!=(const ArrayObjectKey& other) {
        return !(*this == other);
    }
};

static inline bool
NumberTypes(TypeSet::Type a, TypeSet::Type b)
{
    return (a.isPrimitive(JSVAL_TYPE_INT32) || a.isPrimitive(JSVAL_TYPE_DOUBLE))
        && (b.isPrimitive(JSVAL_TYPE_INT32) || b.isPrimitive(JSVAL_TYPE_DOUBLE));
}

/*
 * As for GetValueType, but requires object types to be non-singletons with
 * their default prototype. These are the only values that should appear in
 * arrays and objects whose type can be fixed.
 */
static inline TypeSet::Type
GetValueTypeForTable(const Value& v)
{
    TypeSet::Type type = TypeSet::GetValueType(v);
    MOZ_ASSERT(!type.isSingleton());
    return type;
}

/* static */ JSObject*
ObjectGroup::newArrayObject(ExclusiveContext* cx,
                            const Value* vp, size_t length,
                            NewObjectKind newKind, NewArrayKind arrayKind)
{
    MOZ_ASSERT(newKind != SingletonObject);

    // If we are making a copy on write array, don't try to adjust the group as
    // getOrFixupCopyOnWriteObject will do this before any objects are copied
    // from this one.
    if (arrayKind == NewArrayKind::CopyOnWrite) {
        ArrayObject* obj = NewDenseCopiedArray(cx, length, vp, nullptr, newKind);
        if (!obj || !ObjectElements::MakeElementsCopyOnWrite(cx, obj))
            return nullptr;
        return obj;
    }

    // Get a type which captures all the elements in the array to be created.
    Rooted<TypeSet::Type> elementType(cx, TypeSet::UnknownType());
    if (arrayKind != NewArrayKind::UnknownIndex && length != 0) {
        elementType = GetValueTypeForTable(vp[0]);
        for (unsigned i = 1; i < length; i++) {
            TypeSet::Type ntype = GetValueTypeForTable(vp[i]);
            if (ntype != elementType) {
                if (NumberTypes(elementType, ntype)) {
                    elementType = TypeSet::DoubleType();
                } else {
                    elementType = TypeSet::UnknownType();
                    break;
                }
            }
        }
    }

    ObjectGroupCompartment::ArrayObjectTable*& table =
        cx->compartment()->objectGroups.arrayObjectTable;

    if (!table) {
        table = cx->new_<ObjectGroupCompartment::ArrayObjectTable>();
        if (!table || !table->init()) {
            ReportOutOfMemory(cx);
            js_delete(table);
            table = nullptr;
            return nullptr;
        }
    }

    ObjectGroupCompartment::ArrayObjectKey key(elementType);
    DependentAddPtr<ObjectGroupCompartment::ArrayObjectTable> p(cx, *table, key);

    RootedObjectGroup group(cx);
    if (p) {
        group = p->value();
    } else {
        RootedObject proto(cx);
        if (!GetBuiltinPrototype(cx, JSProto_Array, &proto))
            return nullptr;
        Rooted<TaggedProto> taggedProto(cx, TaggedProto(proto));
        group = ObjectGroupCompartment::makeGroup(cx, &ArrayObject::class_, taggedProto);
        if (!group)
            return nullptr;

        AddTypePropertyId(cx, group, nullptr, JSID_VOID, elementType);

        if (elementType != TypeSet::UnknownType()) {
            // Keep track of the initial objects we create with this type.
            // If the initial ones have a consistent shape and property types, we
            // will try to use an unboxed layout for the group.
            PreliminaryObjectArrayWithTemplate* preliminaryObjects =
                cx->new_<PreliminaryObjectArrayWithTemplate>(nullptr);
            if (!preliminaryObjects)
                return nullptr;
            group->setPreliminaryObjects(preliminaryObjects);
        }

        if (!p.add(cx, *table, ObjectGroupCompartment::ArrayObjectKey(elementType), group))
            return nullptr;
    }

    // The type of the elements being added will already be reflected in type
    // information, but make sure when creating an unboxed array that the
    // common element type is suitable for the unboxed representation.
    ShouldUpdateTypes updateTypes = ShouldUpdateTypes::DontUpdate;
    if (group->maybePreliminaryObjects())
        group->maybePreliminaryObjects()->maybeAnalyze(cx, group);
    if (group->maybeUnboxedLayout()) {
        switch (group->unboxedLayout().elementType()) {
          case JSVAL_TYPE_BOOLEAN:
            if (elementType != TypeSet::BooleanType())
                updateTypes = ShouldUpdateTypes::Update;
            break;
          case JSVAL_TYPE_INT32:
            if (elementType != TypeSet::Int32Type())
                updateTypes = ShouldUpdateTypes::Update;
            break;
          case JSVAL_TYPE_DOUBLE:
            if (elementType != TypeSet::Int32Type() && elementType != TypeSet::DoubleType())
                updateTypes = ShouldUpdateTypes::Update;
            break;
          case JSVAL_TYPE_STRING:
            if (elementType != TypeSet::StringType())
                updateTypes = ShouldUpdateTypes::Update;
            break;
          case JSVAL_TYPE_OBJECT:
            if (elementType != TypeSet::NullType() && !elementType.get().isObjectUnchecked())
                updateTypes = ShouldUpdateTypes::Update;
            break;
          default:
            MOZ_CRASH();
        }
    }

    return NewCopiedArrayTryUseGroup(cx, group, vp, length, newKind, updateTypes);
}

// Try to change the group of |source| to match that of |target|.
static bool
GiveObjectGroup(ExclusiveContext* cx, JSObject* source, JSObject* target)
{
    MOZ_ASSERT(source->group() != target->group());

    if (!target->is<ArrayObject>() && !target->is<UnboxedArrayObject>())
        return true;

    if (target->group()->maybePreliminaryObjects()) {
        bool force = IsInsideNursery(source);
        target->group()->maybePreliminaryObjects()->maybeAnalyze(cx, target->group(), force);
    }

    if (target->is<ArrayObject>()) {
        ObjectGroup* sourceGroup = source->group();

        if (source->is<UnboxedArrayObject>()) {
            Shape* shape = target->as<ArrayObject>().lastProperty();
            if (!UnboxedArrayObject::convertToNativeWithGroup(cx, source, target->group(), shape))
                return false;
        } else if (source->is<ArrayObject>()) {
            source->setGroup(target->group());
        } else {
            return true;
        }

        if (sourceGroup->maybePreliminaryObjects())
            sourceGroup->maybePreliminaryObjects()->unregisterObject(source);
        if (target->group()->maybePreliminaryObjects())
            target->group()->maybePreliminaryObjects()->registerNewObject(source);

        for (size_t i = 0; i < source->as<ArrayObject>().getDenseInitializedLength(); i++) {
            Value v = source->as<ArrayObject>().getDenseElement(i);
            AddTypePropertyId(cx, source->group(), source, JSID_VOID, v);
        }

        return true;
    }

    if (target->is<UnboxedArrayObject>()) {
        if (!source->is<UnboxedArrayObject>())
            return true;
        if (source->as<UnboxedArrayObject>().elementType() != JSVAL_TYPE_INT32)
            return true;
        if (target->as<UnboxedArrayObject>().elementType() != JSVAL_TYPE_DOUBLE)
            return true;

        return source->as<UnboxedArrayObject>().convertInt32ToDouble(cx, target->group());
    }

    return true;
}

static bool
SameGroup(JSObject* first, JSObject* second)
{
    return first->group() == second->group();
}

// When generating a multidimensional array of literals, such as
// [[1,2],[3,4],[5.5,6.5]], try to ensure that each element of the array has
// the same group. This is mainly important when the elements might have
// different native vs. unboxed layouts, or different unboxed layouts, and
// accessing the heterogenous layouts from JIT code will be much slower than
// if they were homogenous.
//
// To do this, with each new array element we compare it with one of the
// previous ones, and try to mutate the group of the new element to fit that
// of the old element. If this isn't possible, the groups for all old elements
// are mutated to fit that of the new element.
bool
js::CombineArrayElementTypes(ExclusiveContext* cx, JSObject* newObj,
                             const Value* compare, size_t ncompare)
{
    if (!ncompare || !compare[0].isObject())
        return true;

    JSObject* oldObj = &compare[0].toObject();
    if (SameGroup(oldObj, newObj))
        return true;

    if (!GiveObjectGroup(cx, newObj, oldObj))
        return false;

    if (SameGroup(oldObj, newObj))
        return true;

    if (!GiveObjectGroup(cx, oldObj, newObj))
        return false;

    if (SameGroup(oldObj, newObj)) {
        for (size_t i = 1; i < ncompare; i++) {
            if (compare[i].isObject() && !SameGroup(&compare[i].toObject(), newObj)) {
                if (!GiveObjectGroup(cx, &compare[i].toObject(), newObj))
                    return false;
            }
        }
    }

    return true;
}

// Similarly to CombineArrayElementTypes, if we are generating an array of
// plain objects with a consistent property layout, such as
// [{p:[1,2]},{p:[3,4]},{p:[5.5,6.5]}], where those plain objects in
// turn have arrays as their own properties, try to ensure that a consistent
// group is given to each array held by the same property of the plain objects.
bool
js::CombinePlainObjectPropertyTypes(ExclusiveContext* cx, JSObject* newObj,
                                    const Value* compare, size_t ncompare)
{
    if (!ncompare || !compare[0].isObject())
        return true;

    JSObject* oldObj = &compare[0].toObject();
    if (!SameGroup(oldObj, newObj))
        return true;

    if (newObj->is<PlainObject>()) {
        if (newObj->as<PlainObject>().lastProperty() != oldObj->as<PlainObject>().lastProperty())
            return true;

        for (size_t slot = 0; slot < newObj->as<PlainObject>().slotSpan(); slot++) {
            Value newValue = newObj->as<PlainObject>().getSlot(slot);
            Value oldValue = oldObj->as<PlainObject>().getSlot(slot);

            if (!newValue.isObject() || !oldValue.isObject())
                continue;

            JSObject* newInnerObj = &newValue.toObject();
            JSObject* oldInnerObj = &oldValue.toObject();

            if (SameGroup(oldInnerObj, newInnerObj))
                continue;

            if (!GiveObjectGroup(cx, newInnerObj, oldInnerObj))
                return false;

            if (SameGroup(oldInnerObj, newInnerObj))
                continue;

            if (!GiveObjectGroup(cx, oldInnerObj, newInnerObj))
                return false;

            if (SameGroup(oldInnerObj, newInnerObj)) {
                for (size_t i = 1; i < ncompare; i++) {
                    if (compare[i].isObject() && SameGroup(&compare[i].toObject(), newObj)) {
                        Value otherValue = compare[i].toObject().as<PlainObject>().getSlot(slot);
                        if (otherValue.isObject() && !SameGroup(&otherValue.toObject(), newInnerObj)) {
                            if (!GiveObjectGroup(cx, &otherValue.toObject(), newInnerObj))
                                return false;
                        }
                    }
                }
            }
        }
    } else if (newObj->is<UnboxedPlainObject>()) {
        const UnboxedLayout& layout = newObj->as<UnboxedPlainObject>().layout();
        const int32_t* traceList = layout.traceList();
        if (!traceList)
            return true;

        uint8_t* newData = newObj->as<UnboxedPlainObject>().data();
        uint8_t* oldData = oldObj->as<UnboxedPlainObject>().data();

        for (; *traceList != -1; traceList++) {}
        traceList++;
        for (; *traceList != -1; traceList++) {
            JSObject* newInnerObj = *reinterpret_cast<JSObject**>(newData + *traceList);
            JSObject* oldInnerObj = *reinterpret_cast<JSObject**>(oldData + *traceList);

            if (!newInnerObj || !oldInnerObj || SameGroup(oldInnerObj, newInnerObj))
                continue;

            if (!GiveObjectGroup(cx, newInnerObj, oldInnerObj))
                return false;

            if (SameGroup(oldInnerObj, newInnerObj))
                continue;

            if (!GiveObjectGroup(cx, oldInnerObj, newInnerObj))
                return false;

            if (SameGroup(oldInnerObj, newInnerObj)) {
                for (size_t i = 1; i < ncompare; i++) {
                    if (compare[i].isObject() && SameGroup(&compare[i].toObject(), newObj)) {
                        uint8_t* otherData = compare[i].toObject().as<UnboxedPlainObject>().data();
                        JSObject* otherInnerObj = *reinterpret_cast<JSObject**>(otherData + *traceList);
                        if (otherInnerObj && !SameGroup(otherInnerObj, newInnerObj)) {
                            if (!GiveObjectGroup(cx, otherInnerObj, newInnerObj))
                                return false;
                        }
                    }
                }
            }
        }
    }

    return true;
}

/////////////////////////////////////////////////////////////////////
// ObjectGroupCompartment PlainObjectTable
/////////////////////////////////////////////////////////////////////

struct ObjectGroupCompartment::PlainObjectKey
{
    jsid* properties;
    uint32_t nproperties;

    struct Lookup {
        IdValuePair* properties;
        uint32_t nproperties;

        Lookup(IdValuePair* properties, uint32_t nproperties)
          : properties(properties), nproperties(nproperties)
        {}
    };

    static inline HashNumber hash(const Lookup& lookup) {
        return (HashNumber) (JSID_BITS(lookup.properties[lookup.nproperties - 1].id) ^
                             lookup.nproperties);
    }

    static inline bool match(const PlainObjectKey& v, const Lookup& lookup) {
        if (lookup.nproperties != v.nproperties)
            return false;
        for (size_t i = 0; i < lookup.nproperties; i++) {
            if (lookup.properties[i].id != v.properties[i])
                return false;
        }
        return true;
    }
};

struct ObjectGroupCompartment::PlainObjectEntry
{
    ReadBarrieredObjectGroup group;
    ReadBarrieredShape shape;
    TypeSet::Type* types;
};

static bool
CanShareObjectGroup(IdValuePair* properties, size_t nproperties)
{
    // Don't reuse groups for objects containing indexed properties, which
    // might end up as dense elements.
    for (size_t i = 0; i < nproperties; i++) {
        uint32_t index;
        if (IdIsIndex(properties[i].id, &index))
            return false;
    }
    return true;
}

static bool
AddPlainObjectProperties(ExclusiveContext* cx, HandlePlainObject obj,
                         IdValuePair* properties, size_t nproperties)
{
    RootedId propid(cx);
    RootedValue value(cx);

    for (size_t i = 0; i < nproperties; i++) {
        propid = properties[i].id;
        value = properties[i].value;
        if (!NativeDefineProperty(cx, obj, propid, value, nullptr, nullptr, JSPROP_ENUMERATE))
            return false;
    }

    return true;
}

PlainObject*
js::NewPlainObjectWithProperties(ExclusiveContext* cx, IdValuePair* properties, size_t nproperties,
                                 NewObjectKind newKind)
{
    gc::AllocKind allocKind = gc::GetGCObjectKind(nproperties);
    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx, allocKind, newKind));
    if (!obj || !AddPlainObjectProperties(cx, obj, properties, nproperties))
        return nullptr;
    return obj;
}

/* static */ JSObject*
ObjectGroup::newPlainObject(ExclusiveContext* cx, IdValuePair* properties, size_t nproperties,
                            NewObjectKind newKind)
{
    // Watch for simple cases where we don't try to reuse plain object groups.
    if (newKind == SingletonObject || nproperties == 0 || nproperties >= PropertyTree::MAX_HEIGHT)
        return NewPlainObjectWithProperties(cx, properties, nproperties, newKind);

    ObjectGroupCompartment::PlainObjectTable*& table =
        cx->compartment()->objectGroups.plainObjectTable;

    if (!table) {
        table = cx->new_<ObjectGroupCompartment::PlainObjectTable>();
        if (!table || !table->init()) {
            ReportOutOfMemory(cx);
            js_delete(table);
            table = nullptr;
            return nullptr;
        }
    }

    ObjectGroupCompartment::PlainObjectKey::Lookup lookup(properties, nproperties);
    ObjectGroupCompartment::PlainObjectTable::Ptr p = table->lookup(lookup);

    if (!p) {
        if (!CanShareObjectGroup(properties, nproperties))
            return NewPlainObjectWithProperties(cx, properties, nproperties, newKind);

        RootedObject proto(cx);
        if (!GetBuiltinPrototype(cx, JSProto_Object, &proto))
            return nullptr;

        Rooted<TaggedProto> tagged(cx, TaggedProto(proto));
        RootedObjectGroup group(cx, ObjectGroupCompartment::makeGroup(cx, &PlainObject::class_,
                                                                      tagged));
        if (!group)
            return nullptr;

        gc::AllocKind allocKind = gc::GetGCObjectKind(nproperties);
        RootedPlainObject obj(cx, NewObjectWithGroup<PlainObject>(cx, group,
                                                                  allocKind, TenuredObject));
        if (!obj || !AddPlainObjectProperties(cx, obj, properties, nproperties))
            return nullptr;

        // Don't make entries with duplicate property names, which will show up
        // here as objects with fewer properties than we thought we were
        // adding to the object. In this case, reset the object's group to the
        // default (which will have unknown properties) so that the group we
        // just created will be collected by the GC.
        if (obj->slotSpan() != nproperties) {
            ObjectGroup* group = defaultNewGroup(cx, obj->getClass(), obj->getTaggedProto());
            if (!group)
                return nullptr;
            obj->setGroup(group);
            return obj;
        }

        // Keep track of the initial objects we create with this type.
        // If the initial ones have a consistent shape and property types, we
        // will try to use an unboxed layout for the group.
        PreliminaryObjectArrayWithTemplate* preliminaryObjects =
            cx->new_<PreliminaryObjectArrayWithTemplate>(obj->lastProperty());
        if (!preliminaryObjects)
            return nullptr;
        group->setPreliminaryObjects(preliminaryObjects);
        preliminaryObjects->registerNewObject(obj);

        ScopedJSFreePtr<jsid> ids(group->zone()->pod_calloc<jsid>(nproperties));
        if (!ids) {
            ReportOutOfMemory(cx);
            return nullptr;
        }

        ScopedJSFreePtr<TypeSet::Type> types(
            group->zone()->pod_calloc<TypeSet::Type>(nproperties));
        if (!types) {
            ReportOutOfMemory(cx);
            return nullptr;
        }

        for (size_t i = 0; i < nproperties; i++) {
            ids[i] = properties[i].id;
            types[i] = GetValueTypeForTable(obj->getSlot(i));
            AddTypePropertyId(cx, group, nullptr, IdToTypeId(ids[i]), types[i]);
        }

        ObjectGroupCompartment::PlainObjectKey key;
        key.properties = ids;
        key.nproperties = nproperties;
        MOZ_ASSERT(ObjectGroupCompartment::PlainObjectKey::match(key, lookup));

        ObjectGroupCompartment::PlainObjectEntry entry;
        entry.group.set(group);
        entry.shape.set(obj->lastProperty());
        entry.types = types;

        ObjectGroupCompartment::PlainObjectTable::AddPtr np = table->lookupForAdd(lookup);
        if (!table->add(np, key, entry))
            return nullptr;

        ids.forget();
        types.forget();

        return obj;
    }

    RootedObjectGroup group(cx, p->value().group);

    // Watch for existing groups which now use an unboxed layout.
    if (group->maybeUnboxedLayout()) {
        MOZ_ASSERT(group->unboxedLayout().properties().length() == nproperties);
        return UnboxedPlainObject::createWithProperties(cx, group, newKind, properties);
    }

    // Update property types according to the properties we are about to add.
    // Do this before we do anything which can GC, which might move or remove
    // this table entry.
    if (!group->unknownProperties()) {
        for (size_t i = 0; i < nproperties; i++) {
            TypeSet::Type type = p->value().types[i];
            TypeSet::Type ntype = GetValueTypeForTable(properties[i].value);
            if (ntype == type)
                continue;
            if (ntype.isPrimitive(JSVAL_TYPE_INT32) &&
                type.isPrimitive(JSVAL_TYPE_DOUBLE))
            {
                // The property types already reflect 'int32'.
            } else {
                if (ntype.isPrimitive(JSVAL_TYPE_DOUBLE) &&
                    type.isPrimitive(JSVAL_TYPE_INT32))
                {
                    // Include 'double' in the property types to avoid the update below later.
                    p->value().types[i] = TypeSet::DoubleType();
                }
                AddTypePropertyId(cx, group, nullptr, IdToTypeId(properties[i].id), ntype);
            }
        }
    }

    RootedShape shape(cx, p->value().shape);

    if (group->maybePreliminaryObjects())
        newKind = TenuredObject;

    gc::AllocKind allocKind = gc::GetGCObjectKind(nproperties);
    RootedPlainObject obj(cx, NewObjectWithGroup<PlainObject>(cx, group, allocKind,
                                                              newKind));

    if (!obj || !obj->setLastProperty(cx, shape))
        return nullptr;

    for (size_t i = 0; i < nproperties; i++)
        obj->setSlot(i, properties[i].value);

    if (group->maybePreliminaryObjects()) {
        group->maybePreliminaryObjects()->registerNewObject(obj);
        group->maybePreliminaryObjects()->maybeAnalyze(cx, group);
    }

    return obj;
}

/////////////////////////////////////////////////////////////////////
// ObjectGroupCompartment AllocationSiteTable
/////////////////////////////////////////////////////////////////////

struct ObjectGroupCompartment::AllocationSiteKey : public DefaultHasher<AllocationSiteKey>,
                                                   public JS::Traceable {
    ReadBarrieredScript script;

    uint32_t offset : 24;
    JSProtoKey kind : 8;

    ReadBarrieredObject proto;

    static const uint32_t OFFSET_LIMIT = (1 << 23);

    AllocationSiteKey(JSScript* script_, uint32_t offset_, JSProtoKey kind_, JSObject* proto_)
      : script(script_), offset(offset_), kind(kind_), proto(proto_)
    {
        MOZ_ASSERT(offset_ < OFFSET_LIMIT);
    }

    AllocationSiteKey(AllocationSiteKey&& key)
      : script(mozilla::Move(key.script)),
        offset(key.offset),
        kind(key.kind),
        proto(mozilla::Move(key.proto))
    { }

    AllocationSiteKey(const AllocationSiteKey& key)
      : script(key.script),
        offset(key.offset),
        kind(key.kind),
        proto(key.proto)
    { }

    static inline uint32_t hash(AllocationSiteKey key) {
        return uint32_t(size_t(key.script->offsetToPC(key.offset)) ^ key.kind ^
               MovableCellHasher<JSObject*>::hash(key.proto));
    }

    static inline bool match(const AllocationSiteKey& a, const AllocationSiteKey& b) {
        return DefaultHasher<JSScript*>::match(a.script, b.script) &&
               a.offset == b.offset &&
               a.kind == b.kind &&
               MovableCellHasher<JSObject*>::match(a.proto, b.proto);
    }

    static void trace(AllocationSiteKey* key, JSTracer* trc) {
        TraceRoot(trc, &key->script, "AllocationSiteKey script");
        TraceNullableRoot(trc, &key->proto, "AllocationSiteKey proto");
    }
};

/* static */ ObjectGroup*
ObjectGroup::allocationSiteGroup(JSContext* cx, JSScript* scriptArg, jsbytecode* pc,
                                 JSProtoKey kind, HandleObject protoArg /* = nullptr */)
{
    MOZ_ASSERT(!useSingletonForAllocationSite(scriptArg, pc, kind));
    MOZ_ASSERT_IF(protoArg, kind == JSProto_Array);

    uint32_t offset = scriptArg->pcToOffset(pc);

    if (offset >= ObjectGroupCompartment::AllocationSiteKey::OFFSET_LIMIT) {
        if (protoArg)
            return defaultNewGroup(cx, GetClassForProtoKey(kind), TaggedProto(protoArg));
        return defaultNewGroup(cx, kind);
    }

    ObjectGroupCompartment::AllocationSiteTable*& table =
        cx->compartment()->objectGroups.allocationSiteTable;

    if (!table) {
        table = cx->new_<ObjectGroupCompartment::AllocationSiteTable>();
        if (!table || !table->init()) {
            ReportOutOfMemory(cx);
            js_delete(table);
            table = nullptr;
            return nullptr;
        }
    }

    RootedScript script(cx, scriptArg);
    RootedObject proto(cx, protoArg);
    if (!proto && kind != JSProto_Null && !GetBuiltinPrototype(cx, kind, &proto))
        return nullptr;

    Rooted<ObjectGroupCompartment::AllocationSiteKey> key(cx,
        ObjectGroupCompartment::AllocationSiteKey(script, offset, kind, proto));

    ObjectGroupCompartment::AllocationSiteTable::AddPtr p = table->lookupForAdd(key);
    if (p)
        return p->value();

    AutoEnterAnalysis enter(cx);

    Rooted<TaggedProto> tagged(cx, TaggedProto(proto));
    ObjectGroup* res = ObjectGroupCompartment::makeGroup(cx, GetClassForProtoKey(kind), tagged,
                                                         OBJECT_FLAG_FROM_ALLOCATION_SITE);
    if (!res)
        return nullptr;

    if (JSOp(*pc) == JSOP_NEWOBJECT) {
        // Keep track of the preliminary objects with this group, so we can try
        // to use an unboxed layout for the object once some are allocated.
        Shape* shape = script->getObject(pc)->as<PlainObject>().lastProperty();
        if (!shape->isEmptyShape()) {
            PreliminaryObjectArrayWithTemplate* preliminaryObjects =
                cx->new_<PreliminaryObjectArrayWithTemplate>(shape);
            if (preliminaryObjects)
                res->setPreliminaryObjects(preliminaryObjects);
            else
                cx->recoverFromOutOfMemory();
        }
    }

    if (kind == JSProto_Array &&
        (JSOp(*pc) == JSOP_NEWARRAY || IsCallPC(pc)) &&
        cx->runtime()->options().unboxedArrays())
    {
        PreliminaryObjectArrayWithTemplate* preliminaryObjects =
            cx->new_<PreliminaryObjectArrayWithTemplate>(nullptr);
        if (preliminaryObjects)
            res->setPreliminaryObjects(preliminaryObjects);
        else
            cx->recoverFromOutOfMemory();
    }

    if (!table->add(p, key, res)) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    return res;
}

void
ObjectGroupCompartment::replaceAllocationSiteGroup(JSScript* script, jsbytecode* pc,
                                                   JSProtoKey kind, ObjectGroup* group)
{
    AllocationSiteKey key(script, script->pcToOffset(pc), kind, group->proto().toObjectOrNull());

    AllocationSiteTable::Ptr p = allocationSiteTable->lookup(key);
    MOZ_RELEASE_ASSERT(p);
    allocationSiteTable->remove(p);
    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!allocationSiteTable->putNew(key, group))
            oomUnsafe.crash("Inconsistent object table");
    }
}

/* static */ ObjectGroup*
ObjectGroup::callingAllocationSiteGroup(JSContext* cx, JSProtoKey key, HandleObject proto)
{
    MOZ_ASSERT_IF(proto, key == JSProto_Array);

    jsbytecode* pc;
    RootedScript script(cx, cx->currentScript(&pc));
    if (script)
        return allocationSiteGroup(cx, script, pc, key, proto);
    if (proto)
        return defaultNewGroup(cx, GetClassForProtoKey(key), TaggedProto(proto));
    return defaultNewGroup(cx, key);
}

/* static */ bool
ObjectGroup::setAllocationSiteObjectGroup(JSContext* cx,
                                          HandleScript script, jsbytecode* pc,
                                          HandleObject obj, bool singleton)
{
    JSProtoKey key = JSCLASS_CACHED_PROTO_KEY(obj->getClass());
    MOZ_ASSERT(key != JSProto_Null);
    MOZ_ASSERT(singleton == useSingletonForAllocationSite(script, pc, key));

    if (singleton) {
        MOZ_ASSERT(obj->isSingleton());

        /*
         * Inference does not account for types of run-once initializer
         * objects, as these may not be created until after the script
         * has been analyzed.
         */
        TypeScript::Monitor(cx, script, pc, ObjectValue(*obj));
    } else {
        ObjectGroup* group = allocationSiteGroup(cx, script, pc, key);
        if (!group)
            return false;
        obj->setGroup(group);
    }

    return true;
}

/* static */ ArrayObject*
ObjectGroup::getOrFixupCopyOnWriteObject(JSContext* cx, HandleScript script, jsbytecode* pc)
{
    // Make sure that the template object for script/pc has a type indicating
    // that the object and its copies have copy on write elements.
    RootedArrayObject obj(cx, &script->getObject(GET_UINT32_INDEX(pc))->as<ArrayObject>());
    MOZ_ASSERT(obj->denseElementsAreCopyOnWrite());

    if (obj->group()->fromAllocationSite()) {
        MOZ_ASSERT(obj->group()->hasAnyFlags(OBJECT_FLAG_COPY_ON_WRITE));
        return obj;
    }

    RootedObjectGroup group(cx, allocationSiteGroup(cx, script, pc, JSProto_Array));
    if (!group)
        return nullptr;

    group->addFlags(OBJECT_FLAG_COPY_ON_WRITE);

    // Update type information in the initializer object group.
    MOZ_ASSERT(obj->slotSpan() == 0);
    for (size_t i = 0; i < obj->getDenseInitializedLength(); i++) {
        const Value& v = obj->getDenseElement(i);
        AddTypePropertyId(cx, group, nullptr, JSID_VOID, v);
    }

    obj->setGroup(group);
    return obj;
}

/* static */ ArrayObject*
ObjectGroup::getCopyOnWriteObject(JSScript* script, jsbytecode* pc)
{
    // getOrFixupCopyOnWriteObject should already have been called for
    // script/pc, ensuring that the template object has a group with the
    // COPY_ON_WRITE flag. We don't assert this here, due to a corner case
    // where this property doesn't hold. See jsop_newarray_copyonwrite in
    // IonBuilder.
    ArrayObject* obj = &script->getObject(GET_UINT32_INDEX(pc))->as<ArrayObject>();
    MOZ_ASSERT(obj->denseElementsAreCopyOnWrite());

    return obj;
}

/* static */ bool
ObjectGroup::findAllocationSite(JSContext* cx, ObjectGroup* group,
                                JSScript** script, uint32_t* offset)
{
    *script = nullptr;
    *offset = 0;

    const ObjectGroupCompartment::AllocationSiteTable* table =
        cx->compartment()->objectGroups.allocationSiteTable;

    if (!table)
        return false;

    for (ObjectGroupCompartment::AllocationSiteTable::Range r = table->all();
         !r.empty();
         r.popFront())
    {
        if (group == r.front().value()) {
            *script = r.front().key().script;
            *offset = r.front().key().offset;
            return true;
        }
    }

    return false;
}

/////////////////////////////////////////////////////////////////////
// ObjectGroupCompartment
/////////////////////////////////////////////////////////////////////

ObjectGroupCompartment::ObjectGroupCompartment()
{
    PodZero(this);
}

ObjectGroupCompartment::~ObjectGroupCompartment()
{
    js_delete(defaultNewTable);
    js_delete(lazyTable);
    js_delete(arrayObjectTable);
    js_delete(plainObjectTable);
    js_delete(allocationSiteTable);
}

void
ObjectGroupCompartment::removeDefaultNewGroup(const Class* clasp, TaggedProto proto,
                                              JSObject* associated)
{
    NewTable::Ptr p = defaultNewTable->lookup(NewEntry::Lookup(clasp, proto, associated));
    MOZ_RELEASE_ASSERT(p);

    defaultNewTable->remove(p);
}

void
ObjectGroupCompartment::replaceDefaultNewGroup(const Class* clasp, TaggedProto proto,
                                               JSObject* associated, ObjectGroup* group)
{
    NewEntry::Lookup lookup(clasp, proto, associated);

    NewTable::Ptr p = defaultNewTable->lookup(lookup);
    MOZ_RELEASE_ASSERT(p);
    defaultNewTable->remove(p);
    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!defaultNewTable->putNew(lookup, NewEntry(group, associated)))
            oomUnsafe.crash("Inconsistent object table");
    }
}

/* static */
ObjectGroup*
ObjectGroupCompartment::makeGroup(ExclusiveContext* cx, const Class* clasp,
                                  Handle<TaggedProto> proto,
                                  ObjectGroupFlags initialFlags /* = 0 */)
{
    MOZ_ASSERT_IF(proto.isObject(), cx->isInsideCurrentCompartment(proto.toObject()));

    ObjectGroup* group = Allocate<ObjectGroup>(cx);
    if (!group)
        return nullptr;
    new(group) ObjectGroup(clasp, proto, cx->compartment(), initialFlags);

    return group;
}

void
ObjectGroupCompartment::addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                               size_t* allocationSiteTables,
                                               size_t* arrayObjectGroupTables,
                                               size_t* plainObjectGroupTables,
                                               size_t* compartmentTables)
{
    if (allocationSiteTable)
        *allocationSiteTables += allocationSiteTable->sizeOfIncludingThis(mallocSizeOf);

    if (arrayObjectTable)
        *arrayObjectGroupTables += arrayObjectTable->sizeOfIncludingThis(mallocSizeOf);

    if (plainObjectTable) {
        *plainObjectGroupTables += plainObjectTable->sizeOfIncludingThis(mallocSizeOf);

        for (PlainObjectTable::Enum e(*plainObjectTable);
             !e.empty();
             e.popFront())
        {
            const PlainObjectKey& key = e.front().key();
            const PlainObjectEntry& value = e.front().value();

            /* key.ids and values.types have the same length. */
            *plainObjectGroupTables += mallocSizeOf(key.properties) + mallocSizeOf(value.types);
        }
    }

    if (defaultNewTable)
        *compartmentTables += defaultNewTable->sizeOfIncludingThis(mallocSizeOf);

    if (lazyTable)
        *compartmentTables += lazyTable->sizeOfIncludingThis(mallocSizeOf);
}

void
ObjectGroupCompartment::clearTables()
{
    if (allocationSiteTable && allocationSiteTable->initialized())
        allocationSiteTable->clear();
    if (arrayObjectTable && arrayObjectTable->initialized())
        arrayObjectTable->clear();
    if (plainObjectTable && plainObjectTable->initialized()) {
        for (PlainObjectTable::Enum e(*plainObjectTable); !e.empty(); e.popFront()) {
            const PlainObjectKey& key = e.front().key();
            PlainObjectEntry& entry = e.front().value();
            js_free(key.properties);
            js_free(entry.types);
        }
        plainObjectTable->clear();
    }
    if (defaultNewTable && defaultNewTable->initialized())
        defaultNewTable->clear();
    if (lazyTable && lazyTable->initialized())
        lazyTable->clear();
}

void
ObjectGroupCompartment::sweep(FreeOp* fop)
{
    /*
     * Iterate through the array/object group tables and remove all entries
     * referencing collected data. These tables only hold weak references.
     */

    if (arrayObjectTable) {
        for (ArrayObjectTable::Enum e(*arrayObjectTable); !e.empty(); e.popFront()) {
            ArrayObjectKey key = e.front().key();
            MOZ_ASSERT(key.type.isUnknown() || !key.type.isSingleton());

            bool remove = false;
            if (!key.type.isUnknown() && key.type.isGroup()) {
                ObjectGroup* group = key.type.groupNoBarrier();
                if (IsAboutToBeFinalizedUnbarriered(&group))
                    remove = true;
                else
                    key.type = TypeSet::ObjectType(group);
            }
            if (IsAboutToBeFinalized(&e.front().value()))
                remove = true;

            if (remove)
                e.removeFront();
            else if (key != e.front().key())
                e.rekeyFront(key);
        }
    }

    if (plainObjectTable) {
        for (PlainObjectTable::Enum e(*plainObjectTable); !e.empty(); e.popFront()) {
            const PlainObjectKey& key = e.front().key();
            PlainObjectEntry& entry = e.front().value();

            bool remove = false;
            if (IsAboutToBeFinalized(&entry.group))
                remove = true;
            if (IsAboutToBeFinalized(&entry.shape))
                remove = true;
            for (unsigned i = 0; !remove && i < key.nproperties; i++) {
                if (gc::IsAboutToBeFinalizedUnbarriered(&key.properties[i]))
                    remove = true;

                MOZ_ASSERT(!entry.types[i].isSingleton());
                if (entry.types[i].isGroup()) {
                    ObjectGroup* group = entry.types[i].groupNoBarrier();
                    if (IsAboutToBeFinalizedUnbarriered(&group))
                        remove = true;
                    else if (group != entry.types[i].groupNoBarrier())
                        entry.types[i] = TypeSet::ObjectType(group);
                }
            }

            if (remove) {
                js_free(key.properties);
                js_free(entry.types);
                e.removeFront();
            }
        }
    }

    if (allocationSiteTable) {
        for (AllocationSiteTable::Enum e(*allocationSiteTable); !e.empty(); e.popFront()) {
            bool keyDying = IsAboutToBeFinalized(&e.front().mutableKey().script) ||
                            (e.front().key().proto && IsAboutToBeFinalized(&e.front().mutableKey().proto));
            bool valDying = IsAboutToBeFinalized(&e.front().value());
            if (keyDying || valDying)
                e.removeFront();
        }
    }

    sweepNewTable(defaultNewTable);
    sweepNewTable(lazyTable);
}

void
ObjectGroupCompartment::sweepNewTable(NewTable* table)
{
    if (table && table->initialized()) {
        for (NewTable::Enum e(*table); !e.empty(); e.popFront()) {
            NewEntry entry = e.front();
            if (IsAboutToBeFinalized(&entry.group) ||
                (entry.associated && IsAboutToBeFinalizedUnbarriered(&entry.associated)))
            {
                e.removeFront();
            } else {
                /* Any rekeying necessary is handled by fixupNewObjectGroupTable() below. */
                MOZ_ASSERT(entry.group.unbarrieredGet() == e.front().group.unbarrieredGet());
                MOZ_ASSERT(entry.associated == e.front().associated);
            }
        }
    }
}

void
ObjectGroupCompartment::fixupNewTableAfterMovingGC(NewTable* table)
{
    /*
     * Each entry's hash depends on the object's prototype and we can't tell
     * whether that has been moved or not in sweepNewObjectGroupTable().
     */
    if (table && table->initialized()) {
        for (NewTable::Enum e(*table); !e.empty(); e.popFront()) {
            NewEntry entry = e.front();
            bool needRekey = false;
            if (IsForwarded(entry.group.unbarrieredGet())) {
                entry.group.set(Forwarded(entry.group.unbarrieredGet()));
                needRekey = true;
            }
            TaggedProto proto = entry.group.unbarrieredGet()->proto();
            if (proto.isObject() && IsForwarded(proto.toObject())) {
                proto = TaggedProto(Forwarded(proto.toObject()));
                needRekey = true;
            }
            if (entry.associated && IsForwarded(entry.associated)) {
                entry.associated = Forwarded(entry.associated);
                needRekey = true;
            }
            if (needRekey) {
                const Class* clasp = entry.group.unbarrieredGet()->clasp();
                if (entry.associated && entry.associated->is<JSFunction>())
                    clasp = nullptr;
                NewEntry::Lookup lookup(clasp, proto, entry.associated);
                e.rekeyFront(lookup, entry);
            }
        }
    }
}

#ifdef JSGC_HASH_TABLE_CHECKS

void
ObjectGroupCompartment::checkNewTableAfterMovingGC(NewTable* table)
{
    /*
     * Assert that nothing points into the nursery or needs to be relocated, and
     * that the hash table entries are discoverable.
     */
    if (!table || !table->initialized())
        return;

    for (NewTable::Enum e(*table); !e.empty(); e.popFront()) {
        NewEntry entry = e.front();
        CheckGCThingAfterMovingGC(entry.group.unbarrieredGet());
        TaggedProto proto = entry.group.unbarrieredGet()->proto();
        if (proto.isObject())
            CheckGCThingAfterMovingGC(proto.toObject());
        CheckGCThingAfterMovingGC(entry.associated);

        const Class* clasp = entry.group.unbarrieredGet()->clasp();
        if (entry.associated && entry.associated->is<JSFunction>())
            clasp = nullptr;

        NewEntry::Lookup lookup(clasp, proto, entry.associated);
        NewTable::Ptr ptr = table->lookup(lookup);
        MOZ_RELEASE_ASSERT(ptr.found() && &*ptr == &e.front());
    }
}

#endif // JSGC_HASH_TABLE_CHECKS
