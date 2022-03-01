/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript iterators. */

#include "vm/Iteration.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"

#include <algorithm>
#include <new>

#include "jstypes.h"

#include "builtin/Array.h"
#include "builtin/SelfHostingDefines.h"
#include "ds/Sort.h"
#include "gc/FreeOp.h"
#include "gc/Marking.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/Proxy.h"
#include "util/DifferentialTesting.h"
#include "util/Poison.h"
#include "vm/BytecodeUtil.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/NativeObject.h"  // js::PlainObject
#include "vm/Shape.h"
#include "vm/TypedArrayObject.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/Compartment-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/PlainObject-inl.h"  // js::PlainObject::createWithTemplate
#include "vm/Stack-inl.h"
#include "vm/StringType-inl.h"

using namespace js;

using mozilla::ArrayEqual;
using mozilla::DebugOnly;
using mozilla::Maybe;
using mozilla::PodCopy;

using RootedPropertyIteratorObject = Rooted<PropertyIteratorObject*>;

static const gc::AllocKind ITERATOR_FINALIZE_KIND =
    gc::AllocKind::OBJECT2_BACKGROUND;

// Beware!  This function may have to trace incompletely-initialized
// |NativeIterator| allocations if the |IdToString| in that constructor recurs
// into this code.
void NativeIterator::trace(JSTracer* trc) {
  TraceNullableEdge(trc, &objectBeingIterated_, "objectBeingIterated_");
  TraceNullableEdge(trc, &iterObj_, "iterObj");

  // The limits below are correct at every instant of |NativeIterator|
  // initialization, with the end-pointer incremented as each new shape is
  // created, so they're safe to use here.
  std::for_each(shapesBegin(), shapesEnd(), [trc](GCPtrShape& shape) {
    TraceEdge(trc, &shape, "iterator_shape");
  });

  // But as properties must be created *before* shapes, |propertiesBegin()|
  // that depends on |shapesEnd()| having its final value can't safely be
  // used.  Until this is fully initialized, use |propertyCursor_| instead,
  // which points at the start of properties even in partially initialized
  // |NativeIterator|s.  (|propertiesEnd()| is safe at all times with respect
  // to the properly-chosen beginning.)
  //
  // Note that we must trace all properties (not just those not yet visited,
  // or just visited, due to |NativeIterator::previousPropertyWas|) for
  // |NativeIterator|s to be reusable.
  GCPtrLinearString* begin =
      MOZ_LIKELY(isInitialized()) ? propertiesBegin() : propertyCursor_;
  std::for_each(begin, propertiesEnd(), [trc](GCPtrLinearString& prop) {
    // Properties begin life non-null and never *become*
    // null.  (Deletion-suppression will shift trailing
    // properties over a deleted property in the properties
    // array, but it doesn't null them out.)
    TraceEdge(trc, &prop, "prop");
  });
}

using PropertyKeySet = GCHashSet<PropertyKey, DefaultHasher<PropertyKey>>;

template <bool CheckForDuplicates>
static inline bool Enumerate(JSContext* cx, HandleObject pobj, jsid id,
                             bool enumerable, unsigned flags,
                             MutableHandle<PropertyKeySet> visited,
                             MutableHandleIdVector props) {
  if (CheckForDuplicates) {
    // If we've already seen this, we definitely won't add it.
    PropertyKeySet::AddPtr p = visited.lookupForAdd(id);
    if (MOZ_UNLIKELY(!!p)) {
      return true;
    }

    // It's not necessary to add properties to the hash set at the end of
    // the prototype chain, but custom enumeration behaviors might return
    // duplicated properties, so always add in such cases.
    if (pobj->is<ProxyObject>() || pobj->staticPrototype() ||
        pobj->getClass()->getNewEnumerate()) {
      if (!visited.add(p, id)) {
        return false;
      }
    }
  }

  if (!enumerable && !(flags & JSITER_HIDDEN)) {
    return true;
  }

  // Symbol-keyed properties and nonenumerable properties are skipped unless
  // the caller specifically asks for them. A caller can also filter out
  // non-symbols by asking for JSITER_SYMBOLSONLY. PrivateName symbols are
  // skipped unless JSITER_PRIVATE is passed.
  if (id.isSymbol()) {
    if (!(flags & JSITER_SYMBOLS)) {
      return true;
    }
    if (!(flags & JSITER_PRIVATE) && id.isPrivateName()) {
      return true;
    }
  } else {
    if ((flags & JSITER_SYMBOLSONLY)) {
      return true;
    }
  }

  return props.append(id);
}

static bool EnumerateExtraProperties(JSContext* cx, HandleObject obj,
                                     unsigned flags,
                                     MutableHandle<PropertyKeySet> visited,
                                     MutableHandleIdVector props) {
  MOZ_ASSERT(obj->getClass()->getNewEnumerate());

  RootedIdVector properties(cx);
  bool enumerableOnly = !(flags & JSITER_HIDDEN);
  if (!obj->getClass()->getNewEnumerate()(cx, obj, &properties,
                                          enumerableOnly)) {
    return false;
  }

  RootedId id(cx);
  for (size_t n = 0; n < properties.length(); n++) {
    id = properties[n];

    // The enumerate hook does not indicate whether the properties
    // it returns are enumerable or not. Since we already passed
    // `enumerableOnly` to the hook to filter out non-enumerable
    // properties, it doesn't really matter what we pass here.
    bool enumerable = true;
    if (!Enumerate<true>(cx, obj, id, enumerable, flags, visited, props)) {
      return false;
    }
  }

  return true;
}

static bool SortComparatorIntegerIds(jsid a, jsid b, bool* lessOrEqualp) {
  uint32_t indexA, indexB;
  MOZ_ALWAYS_TRUE(IdIsIndex(a, &indexA));
  MOZ_ALWAYS_TRUE(IdIsIndex(b, &indexB));
  *lessOrEqualp = (indexA <= indexB);
  return true;
}

template <bool CheckForDuplicates>
static bool EnumerateNativeProperties(JSContext* cx, HandleNativeObject pobj,
                                      unsigned flags,
                                      MutableHandle<PropertyKeySet> visited,
                                      MutableHandleIdVector props) {
  bool enumerateSymbols;
  if (flags & JSITER_SYMBOLSONLY) {
    enumerateSymbols = true;
  } else {
    // Collect any dense elements from this object.
    size_t firstElemIndex = props.length();
    size_t initlen = pobj->getDenseInitializedLength();
    const Value* vp = pobj->getDenseElements();
    bool hasHoles = false;
    for (size_t i = 0; i < initlen; ++i, ++vp) {
      if (vp->isMagic(JS_ELEMENTS_HOLE)) {
        hasHoles = true;
      } else {
        // Dense arrays never get so large that i would not fit into an
        // integer id.
        if (!Enumerate<CheckForDuplicates>(cx, pobj, INT_TO_JSID(i),
                                           /* enumerable = */ true, flags,
                                           visited, props)) {
          return false;
        }
      }
    }

    // Collect any typed array or shared typed array elements from this
    // object.
    if (pobj->is<TypedArrayObject>()) {
      size_t len = pobj->as<TypedArrayObject>().length();

      // Fail early if the typed array is enormous, because this will be very
      // slow and will likely report OOM. This also means we don't need to
      // handle indices greater than JSID_INT_MAX in the loop below.
      static_assert(JSID_INT_MAX == INT32_MAX);
      if (len > INT32_MAX) {
        ReportOutOfMemory(cx);
        return false;
      }

      for (size_t i = 0; i < len; i++) {
        if (!Enumerate<CheckForDuplicates>(cx, pobj, INT_TO_JSID(i),
                                           /* enumerable = */ true, flags,
                                           visited, props)) {
          return false;
        }
      }
    }

    // Collect any sparse elements from this object.
    bool isIndexed = pobj->isIndexed();
    if (isIndexed) {
      // If the dense elements didn't have holes, we don't need to include
      // them in the sort.
      if (!hasHoles) {
        firstElemIndex = props.length();
      }

      for (ShapePropertyIter<NoGC> iter(pobj->shape()); !iter.done(); iter++) {
        jsid id = iter->key();
        uint32_t dummy;
        if (IdIsIndex(id, &dummy)) {
          if (!Enumerate<CheckForDuplicates>(cx, pobj, id, iter->enumerable(),
                                             flags, visited, props)) {
            return false;
          }
        }
      }

      MOZ_ASSERT(firstElemIndex <= props.length());

      jsid* ids = props.begin() + firstElemIndex;
      size_t n = props.length() - firstElemIndex;

      RootedIdVector tmp(cx);
      if (!tmp.resize(n)) {
        return false;
      }
      PodCopy(tmp.begin(), ids, n);

      if (!MergeSort(ids, n, tmp.begin(), SortComparatorIntegerIds)) {
        return false;
      }
    }

    size_t initialLength = props.length();

    /* Collect all unique property names from this object's shape. */
    bool symbolsFound = false;
    for (ShapePropertyIter<NoGC> iter(pobj->shape()); !iter.done(); iter++) {
      jsid id = iter->key();

      if (id.isSymbol()) {
        symbolsFound = true;
        continue;
      }

      uint32_t dummy;
      if (isIndexed && IdIsIndex(id, &dummy)) {
        continue;
      }

      if (!Enumerate<CheckForDuplicates>(cx, pobj, id, iter->enumerable(),
                                         flags, visited, props)) {
        return false;
      }
    }
    std::reverse(props.begin() + initialLength, props.end());

    enumerateSymbols = symbolsFound && (flags & JSITER_SYMBOLS);
  }

  if (enumerateSymbols) {
    // Do a second pass to collect symbols. ES6 draft rev 25 (2014 May 22)
    // 9.1.12 requires that all symbols appear after all strings in the
    // result.
    size_t initialLength = props.length();
    for (ShapePropertyIter<NoGC> iter(pobj->shape()); !iter.done(); iter++) {
      jsid id = iter->key();
      if (id.isSymbol()) {
        if (!Enumerate<CheckForDuplicates>(cx, pobj, id, iter->enumerable(),
                                           flags, visited, props)) {
          return false;
        }
      }
    }
    std::reverse(props.begin() + initialLength, props.end());
  }

  return true;
}

static bool EnumerateNativeProperties(JSContext* cx, HandleNativeObject pobj,
                                      unsigned flags,
                                      MutableHandle<PropertyKeySet> visited,
                                      MutableHandleIdVector props,
                                      bool checkForDuplicates) {
  if (checkForDuplicates) {
    return EnumerateNativeProperties<true>(cx, pobj, flags, visited, props);
  }
  return EnumerateNativeProperties<false>(cx, pobj, flags, visited, props);
}

template <bool CheckForDuplicates>
static bool EnumerateProxyProperties(JSContext* cx, HandleObject pobj,
                                     unsigned flags,
                                     MutableHandle<PropertyKeySet> visited,
                                     MutableHandleIdVector props) {
  MOZ_ASSERT(pobj->is<ProxyObject>());

  RootedIdVector proxyProps(cx);

  if (flags & JSITER_HIDDEN || flags & JSITER_SYMBOLS) {
    // This gets all property keys, both strings and symbols. The call to
    // Enumerate in the loop below will filter out unwanted keys, per the
    // flags.
    if (!Proxy::ownPropertyKeys(cx, pobj, &proxyProps)) {
      return false;
    }

    Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
    for (size_t n = 0, len = proxyProps.length(); n < len; n++) {
      bool enumerable = false;

      // We need to filter, if the caller just wants enumerable symbols.
      if (!(flags & JSITER_HIDDEN)) {
        if (!Proxy::getOwnPropertyDescriptor(cx, pobj, proxyProps[n], &desc)) {
          return false;
        }
        enumerable = desc.isSome() && desc->enumerable();
      }

      if (!Enumerate<CheckForDuplicates>(cx, pobj, proxyProps[n], enumerable,
                                         flags, visited, props)) {
        return false;
      }
    }

    return true;
  }

  // Returns enumerable property names (no symbols).
  if (!Proxy::getOwnEnumerablePropertyKeys(cx, pobj, &proxyProps)) {
    return false;
  }

  for (size_t n = 0, len = proxyProps.length(); n < len; n++) {
    if (!Enumerate<CheckForDuplicates>(cx, pobj, proxyProps[n], true, flags,
                                       visited, props)) {
      return false;
    }
  }

  return true;
}

#ifdef DEBUG

struct SortComparatorIds {
  JSContext* const cx;

  explicit SortComparatorIds(JSContext* cx) : cx(cx) {}

  bool operator()(jsid aArg, jsid bArg, bool* lessOrEqualp) {
    RootedId a(cx, aArg);
    RootedId b(cx, bArg);

    // Pick an arbitrary order on jsids that is as stable as possible
    // across executions.
    if (a == b) {
      *lessOrEqualp = true;
      return true;
    }

    size_t ta = JSID_BITS(a.get()) & JSID_TYPE_MASK;
    size_t tb = JSID_BITS(b.get()) & JSID_TYPE_MASK;
    if (ta != tb) {
      *lessOrEqualp = (ta <= tb);
      return true;
    }

    if (JSID_IS_INT(a)) {
      *lessOrEqualp = (JSID_TO_INT(a) <= JSID_TO_INT(b));
      return true;
    }

    RootedString astr(cx), bstr(cx);
    if (a.isSymbol()) {
      MOZ_ASSERT(b.isSymbol());
      JS::SymbolCode ca = a.toSymbol()->code();
      JS::SymbolCode cb = b.toSymbol()->code();
      if (ca != cb) {
        *lessOrEqualp = uint32_t(ca) <= uint32_t(cb);
        return true;
      }
      MOZ_ASSERT(ca == JS::SymbolCode::PrivateNameSymbol ||
                 ca == JS::SymbolCode::InSymbolRegistry ||
                 ca == JS::SymbolCode::UniqueSymbol);
      astr = a.toSymbol()->description();
      bstr = b.toSymbol()->description();
      if (!astr || !bstr) {
        *lessOrEqualp = !astr;
        return true;
      }

      // Fall through to string comparison on the descriptions. The sort
      // order is nondeterministic if two different unique symbols have
      // the same description.
    } else {
      astr = IdToString(cx, a);
      if (!astr) {
        return false;
      }
      bstr = IdToString(cx, b);
      if (!bstr) {
        return false;
      }
    }

    int32_t result;
    if (!CompareStrings(cx, astr, bstr, &result)) {
      return false;
    }

    *lessOrEqualp = (result <= 0);
    return true;
  }
};

#endif /* DEBUG */

static bool Snapshot(JSContext* cx, HandleObject pobj_, unsigned flags,
                     MutableHandleIdVector props) {
  Rooted<PropertyKeySet> visited(cx, PropertyKeySet(cx));
  RootedObject pobj(cx, pobj_);

  // Don't check for duplicates if we're only interested in own properties.
  // This does the right thing for most objects: native objects don't have
  // duplicate property ids and we allow the [[OwnPropertyKeys]] proxy trap to
  // return duplicates.
  //
  // The only special case is when the object has a newEnumerate hook: it
  // can return duplicate properties and we have to filter them. This is
  // handled below.
  bool checkForDuplicates = !(flags & JSITER_OWNONLY);

  do {
    if (pobj->getClass()->getNewEnumerate()) {
      if (!EnumerateExtraProperties(cx, pobj, flags, &visited, props)) {
        return false;
      }

      if (pobj->is<NativeObject>()) {
        if (!EnumerateNativeProperties(cx, pobj.as<NativeObject>(), flags,
                                       &visited, props, true)) {
          return false;
        }
      }

    } else if (pobj->is<NativeObject>()) {
      // Give the object a chance to resolve all lazy properties
      if (JSEnumerateOp enumerate = pobj->getClass()->getEnumerate()) {
        if (!enumerate(cx, pobj.as<NativeObject>())) {
          return false;
        }
      }
      if (!EnumerateNativeProperties(cx, pobj.as<NativeObject>(), flags,
                                     &visited, props, checkForDuplicates)) {
        return false;
      }
    } else if (pobj->is<ProxyObject>()) {
      if (checkForDuplicates) {
        if (!EnumerateProxyProperties<true>(cx, pobj, flags, &visited, props)) {
          return false;
        }
      } else {
        if (!EnumerateProxyProperties<false>(cx, pobj, flags, &visited,
                                             props)) {
          return false;
        }
      }
    } else {
      MOZ_CRASH("non-native objects must have an enumerate op");
    }

    if (flags & JSITER_OWNONLY) {
      break;
    }

    if (!GetPrototype(cx, pobj, &pobj)) {
      return false;
    }

    // The [[Prototype]] chain might be cyclic.
    if (!CheckForInterrupt(cx)) {
      return false;
    }
  } while (pobj != nullptr);

#ifdef DEBUG
  if (js::SupportDifferentialTesting()) {
    /*
     * In some cases the enumeration order for an object depends on the
     * execution mode (interpreter vs. JIT), especially for native objects
     * with a class enumerate hook (where resolving a property changes the
     * resulting enumeration order). These aren't really bugs, but the
     * differences can change the generated output and confuse correctness
     * fuzzers, so we sort the ids if such a fuzzer is running.
     *
     * We don't do this in the general case because (a) doing so is slow,
     * and (b) it also breaks the web, which expects enumeration order to
     * follow the order in which properties are added, in certain cases.
     * Since ECMA does not specify an enumeration order for objects, both
     * behaviors are technically correct to do.
     */

    jsid* ids = props.begin();
    size_t n = props.length();

    RootedIdVector tmp(cx);
    if (!tmp.resize(n)) {
      return false;
    }
    PodCopy(tmp.begin(), ids, n);

    if (!MergeSort(ids, n, tmp.begin(), SortComparatorIds(cx))) {
      return false;
    }
  }
#endif

  return true;
}

JS_PUBLIC_API bool js::GetPropertyKeys(JSContext* cx, HandleObject obj,
                                       unsigned flags,
                                       MutableHandleIdVector props) {
  return Snapshot(cx, obj,
                  flags & (JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS |
                           JSITER_SYMBOLSONLY | JSITER_PRIVATE),
                  props);
}

static inline void RegisterEnumerator(ObjectRealm& realm, NativeIterator* ni) {
  // Register non-escaping native enumerators (for-in) with the current
  // context.
  ni->link(realm.enumerators);

  MOZ_ASSERT(!ni->isActive());
  ni->markActive();
}

static PropertyIteratorObject* NewPropertyIteratorObject(JSContext* cx) {
  const JSClass* clasp = &PropertyIteratorObject::class_;
  RootedShape shape(cx, SharedShape::getInitialShape(cx, clasp, cx->realm(),
                                                     TaggedProto(nullptr),
                                                     ITERATOR_FINALIZE_KIND));
  if (!shape) {
    return nullptr;
  }

  JSObject* obj;
  JS_TRY_VAR_OR_RETURN_NULL(
      cx, obj,
      NativeObject::create(cx, ITERATOR_FINALIZE_KIND,
                           GetInitialHeap(GenericObject, clasp), shape));

  PropertyIteratorObject* res = &obj->as<PropertyIteratorObject>();

  // CodeGenerator::visitIteratorStartO assumes the iterator object is not
  // inside the nursery when deciding whether a barrier is necessary.
  MOZ_ASSERT(!js::gc::IsInsideNursery(res));

  MOZ_ASSERT(res->numFixedSlots() == PropertyIteratorObject::NUM_FIXED_SLOTS);
  return res;
}

static inline size_t NumTrailingWords(size_t propertyCount, size_t shapeCount) {
  static_assert(sizeof(GCPtrLinearString) == sizeof(uintptr_t));
  static_assert(sizeof(GCPtrShape) == sizeof(uintptr_t));
  return propertyCount + shapeCount;
}

static inline size_t AllocationSize(size_t propertyCount, size_t shapeCount) {
  return sizeof(NativeIterator) +
         (NumTrailingWords(propertyCount, shapeCount) * sizeof(uintptr_t));
}

static PropertyIteratorObject* CreatePropertyIterator(
    JSContext* cx, Handle<JSObject*> objBeingIterated, HandleIdVector props,
    uint32_t numShapes, HashNumber shapesHash) {
  if (props.length() > NativeIterator::PropCountLimit) {
    ReportAllocationOverflow(cx);
    return nullptr;
  }

  Rooted<PropertyIteratorObject*> propIter(cx, NewPropertyIteratorObject(cx));
  if (!propIter) {
    return nullptr;
  }

  void* mem = cx->pod_malloc_with_extra<NativeIterator, uintptr_t>(
      NumTrailingWords(props.length(), numShapes));
  if (!mem) {
    return nullptr;
  }

  // This also registers |ni| with |propIter|.
  bool hadError = false;
  NativeIterator* ni = new (mem) NativeIterator(
      cx, propIter, objBeingIterated, props, numShapes, shapesHash, &hadError);
  if (hadError) {
    return nullptr;
  }

  ObjectRealm& realm = objBeingIterated ? ObjectRealm::get(objBeingIterated)
                                        : ObjectRealm::get(propIter);
  RegisterEnumerator(realm, ni);

  return propIter;
}

/**
 * Initialize a sentinel NativeIterator whose purpose is only to act as the
 * start/end of the circular linked list of NativeIterators in
 * ObjectRealm::enumerators.
 */
NativeIterator::NativeIterator() {
  // Do our best to enforce that nothing in |this| except the two fields set
  // below is ever observed.
  AlwaysPoison(static_cast<void*>(this), JS_NEW_NATIVE_ITERATOR_PATTERN,
               sizeof(*this), MemCheckKind::MakeUndefined);

  // These are the only two fields in sentinel NativeIterators that are
  // examined, in ObjectRealm::traceWeakNativeIterators. Everything else is
  // only examined *if* it's a NativeIterator being traced by a
  // PropertyIteratorObject that owns it, and nothing owns this iterator.
  prev_ = next_ = this;
}

NativeIterator* NativeIterator::allocateSentinel(JSContext* cx) {
  NativeIterator* ni = js_new<NativeIterator>();
  if (!ni) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  return ni;
}

static HashNumber HashIteratorShape(Shape* shape) {
  return DefaultHasher<Shape*>::hash(shape);
}

/**
 * Initialize a fresh NativeIterator.
 *
 * This definition is a bit tricky: some parts of initializing are fallible, so
 * as we initialize, we must carefully keep this in GC-safe state (see
 * NativeIterator::trace).
 */
NativeIterator::NativeIterator(JSContext* cx,
                               Handle<PropertyIteratorObject*> propIter,
                               Handle<JSObject*> objBeingIterated,
                               HandleIdVector props, uint32_t numShapes,
                               HashNumber shapesHash, bool* hadError)
    : objectBeingIterated_(objBeingIterated),
      iterObj_(propIter),
      // NativeIterator initially acts (before full initialization) as if it
      // contains no shapes...
      shapesEnd_(shapesBegin()),
      // ...and no properties.
      propertyCursor_(
          reinterpret_cast<GCPtrLinearString*>(shapesBegin() + numShapes)),
      propertiesEnd_(propertyCursor_),
      shapesHash_(shapesHash),
      flagsAndCount_(
          initialFlagsAndCount(props.length()))  // note: no Flags::Initialized
{
  // If there are shapes, the object and all objects on its prototype chain must
  // be native objects. See CanCompareIterableObjectToCache.
  MOZ_ASSERT_IF(numShapes > 0,
                objBeingIterated && objBeingIterated->is<NativeObject>());

  MOZ_ASSERT(!*hadError);

  // NOTE: This must be done first thing: The caller can't free `this` on error
  //       because it has GCPtr fields whose barriers have already fired; the
  //       store buffer has pointers to them. Only the GC can free `this` (via
  //       PropertyIteratorObject::finalize).
  propIter->setNativeIterator(this);

  // The GC asserts on finalization that `this->allocationSize()` matches the
  // `nbytes` passed to `AddCellMemory`. So once these lines run, we must make
  // `this->allocationSize()` correct. That means infallibly initializing the
  // shapes. It's OK for the constructor to fail after that.
  size_t nbytes = AllocationSize(props.length(), numShapes);
  AddCellMemory(propIter, nbytes, MemoryUse::NativeIterator);

  if (numShapes > 0) {
    // Construct shapes into the shapes array.  Also recompute the shapesHash,
    // which incorporates Shape* addresses that could have changed during a GC
    // triggered in (among other places) |IdToString| above.
    JSObject* pobj = objBeingIterated;
#ifdef DEBUG
    uint32_t i = 0;
#endif
    HashNumber shapesHash = 0;
    do {
      MOZ_ASSERT(pobj->is<NativeObject>());
      Shape* shape = pobj->shape();
      new (shapesEnd_) GCPtrShape(shape);
      shapesEnd_++;
#ifdef DEBUG
      i++;
#endif

      shapesHash = mozilla::AddToHash(shapesHash, HashIteratorShape(shape));

      // The one caller of this method that passes |numShapes > 0|, does
      // so only if the entire chain consists of cacheable objects (that
      // necessarily have static prototypes).
      pobj = pobj->staticPrototype();
    } while (pobj);

    shapesHash_ = shapesHash;
    MOZ_ASSERT(i == numShapes);
  }
  MOZ_ASSERT(static_cast<void*>(shapesEnd_) == propertyCursor_);

  for (size_t i = 0, len = props.length(); i < len; i++) {
    JSLinearString* str = IdToString(cx, props[i]);
    if (!str) {
      *hadError = true;
      return;
    }
    new (propertiesEnd_) GCPtrLinearString(str);
    propertiesEnd_++;
  }

  markInitialized();

  MOZ_ASSERT(!*hadError);
}

inline size_t NativeIterator::allocationSize() const {
  size_t numShapes = shapesEnd() - shapesBegin();
  return AllocationSize(initialPropertyCount(), numShapes);
}

/* static */
bool IteratorHashPolicy::match(PropertyIteratorObject* obj,
                               const Lookup& lookup) {
  NativeIterator* ni = obj->getNativeIterator();
  if (ni->shapesHash() != lookup.shapesHash ||
      ni->shapeCount() != lookup.numShapes) {
    return false;
  }

  return ArrayEqual(reinterpret_cast<Shape**>(ni->shapesBegin()), lookup.shapes,
                    ni->shapeCount());
}

static inline bool CanCompareIterableObjectToCache(JSObject* obj) {
  if (obj->is<NativeObject>()) {
    return obj->as<NativeObject>().getDenseInitializedLength() == 0;
  }
  return false;
}

static MOZ_ALWAYS_INLINE PropertyIteratorObject* LookupInIteratorCache(
    JSContext* cx, JSObject* obj, uint32_t* numShapes) {
  MOZ_ASSERT(*numShapes == 0);

  Vector<Shape*, 8> shapes(cx);
  HashNumber shapesHash = 0;
  JSObject* pobj = obj;
  do {
    if (!CanCompareIterableObjectToCache(pobj)) {
      return nullptr;
    }

    MOZ_ASSERT(pobj->is<NativeObject>());
    Shape* shape = pobj->shape();
    shapesHash = mozilla::AddToHash(shapesHash, HashIteratorShape(shape));

    if (MOZ_UNLIKELY(!shapes.append(shape))) {
      cx->recoverFromOutOfMemory();
      return nullptr;
    }

    pobj = pobj->staticPrototype();
  } while (pobj);

  MOZ_ASSERT(!shapes.empty());
  *numShapes = shapes.length();

  IteratorHashPolicy::Lookup lookup(shapes.begin(), shapes.length(),
                                    shapesHash);
  auto p = ObjectRealm::get(obj).iteratorCache.lookup(lookup);
  if (!p) {
    return nullptr;
  }

  PropertyIteratorObject* iterobj = *p;
  MOZ_ASSERT(iterobj->compartment() == cx->compartment());

  NativeIterator* ni = iterobj->getNativeIterator();
  if (!ni->isReusable()) {
    return nullptr;
  }

  return iterobj;
}

static bool CanStoreInIteratorCache(JSObject* obj) {
  do {
    MOZ_ASSERT(obj->as<NativeObject>().getDenseInitializedLength() == 0);

    // Typed arrays have indexed properties not captured by the Shape guard.
    // Enumerate hooks may add extra properties.
    const JSClass* clasp = obj->getClass();
    if (MOZ_UNLIKELY(IsTypedArrayClass(clasp))) {
      return false;
    }
    if (MOZ_UNLIKELY(clasp->getNewEnumerate() || clasp->getEnumerate())) {
      return false;
    }

    obj = obj->staticPrototype();
  } while (obj);

  return true;
}

[[nodiscard]] static bool StoreInIteratorCache(
    JSContext* cx, JSObject* obj, PropertyIteratorObject* iterobj) {
  MOZ_ASSERT(CanStoreInIteratorCache(obj));

  NativeIterator* ni = iterobj->getNativeIterator();
  MOZ_ASSERT(ni->shapeCount() > 0);

  IteratorHashPolicy::Lookup lookup(
      reinterpret_cast<Shape**>(ni->shapesBegin()), ni->shapeCount(),
      ni->shapesHash());

  ObjectRealm::IteratorCache& cache = ObjectRealm::get(obj).iteratorCache;
  bool ok;
  auto p = cache.lookupForAdd(lookup);
  if (MOZ_LIKELY(!p)) {
    ok = cache.add(p, iterobj);
  } else {
    // If we weren't able to use an existing cached iterator, just
    // replace it.
    cache.remove(p);
    ok = cache.relookupOrAdd(p, lookup, iterobj);
  }
  if (!ok) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

bool js::EnumerateProperties(JSContext* cx, HandleObject obj,
                             MutableHandleIdVector props) {
  MOZ_ASSERT(props.empty());

  if (MOZ_UNLIKELY(obj->is<ProxyObject>())) {
    return Proxy::enumerate(cx, obj, props);
  }

  return Snapshot(cx, obj, 0, props);
}

#ifdef DEBUG
static bool PrototypeMayHaveIndexedProperties(NativeObject* nobj) {
  JSObject* proto = nobj->staticPrototype();
  if (!proto) {
    return false;
  }

  if (proto->is<NativeObject>() &&
      proto->as<NativeObject>().getDenseInitializedLength() > 0) {
    return true;
  }

  return ObjectMayHaveExtraIndexedProperties(proto);
}
#endif

static JSObject* GetIterator(JSContext* cx, HandleObject obj) {
  MOZ_ASSERT(!obj->is<PropertyIteratorObject>());
  MOZ_ASSERT(cx->compartment() == obj->compartment(),
             "We may end up allocating shapes in the wrong zone!");

  uint32_t numShapes = 0;
  if (PropertyIteratorObject* iterobj =
          LookupInIteratorCache(cx, obj, &numShapes)) {
    NativeIterator* ni = iterobj->getNativeIterator();
    ni->changeObjectBeingIterated(*obj);
    RegisterEnumerator(ObjectRealm::get(obj), ni);
    return iterobj;
  }

  if (numShapes > 0 && !CanStoreInIteratorCache(obj)) {
    numShapes = 0;
  }

  RootedIdVector keys(cx);
  if (!EnumerateProperties(cx, obj, &keys)) {
    return nullptr;
  }

  // If the object has dense elements, mark the dense elements as
  // maybe-in-iteration.
  //
  // The iterator is a snapshot so if indexed properties are added after this
  // point we don't need to do anything. However, the object might have sparse
  // elements now that can be densified later. To account for this, we set the
  // maybe-in-iteration flag also in NativeObject::maybeDensifySparseElements.
  //
  // In debug builds, AssertDenseElementsNotIterated is used to check the flag
  // is set correctly.
  if (obj->is<NativeObject>() &&
      obj->as<NativeObject>().getDenseInitializedLength() > 0) {
    obj->as<NativeObject>().markDenseElementsMaybeInIteration();
  }

  PropertyIteratorObject* iterobj =
      CreatePropertyIterator(cx, obj, keys, numShapes, 0);
  if (!iterobj) {
    return nullptr;
  }

  cx->check(iterobj);

#ifdef DEBUG
  if (obj->is<NativeObject>()) {
    if (PrototypeMayHaveIndexedProperties(&obj->as<NativeObject>())) {
      iterobj->getNativeIterator()->setMaybeHasIndexedPropertiesFromProto();
    }
  }
#endif

  // Cache the iterator object.
  if (numShapes > 0) {
    if (!StoreInIteratorCache(cx, obj, iterobj)) {
      return nullptr;
    }
  }

  return iterobj;
}

PropertyIteratorObject* js::LookupInIteratorCache(JSContext* cx,
                                                  HandleObject obj) {
  uint32_t numShapes = 0;
  return LookupInIteratorCache(cx, obj, &numShapes);
}

// ES 2017 draft 7.4.7.
PlainObject* js::CreateIterResultObject(JSContext* cx, HandleValue value,
                                        bool done) {
  // Step 1 (implicit).

  // Step 2.
  Rooted<PlainObject*> templateObject(
      cx, cx->realm()->getOrCreateIterResultTemplateObject(cx));
  if (!templateObject) {
    return nullptr;
  }

  PlainObject* resultObj;
  JS_TRY_VAR_OR_RETURN_NULL(
      cx, resultObj, PlainObject::createWithTemplate(cx, templateObject));

  // Step 3.
  resultObj->setSlot(Realm::IterResultObjectValueSlot, value);

  // Step 4.
  resultObj->setSlot(Realm::IterResultObjectDoneSlot,
                     done ? TrueHandleValue : FalseHandleValue);

  // Step 5.
  return resultObj;
}

PlainObject* Realm::getOrCreateIterResultTemplateObject(JSContext* cx) {
  MOZ_ASSERT(cx->realm() == this);

  if (iterResultTemplate_) {
    return iterResultTemplate_;
  }

  PlainObject* templateObj =
      createIterResultTemplateObject(cx, WithObjectPrototype::Yes);
  iterResultTemplate_.set(templateObj);
  return iterResultTemplate_;
}

PlainObject* Realm::getOrCreateIterResultWithoutPrototypeTemplateObject(
    JSContext* cx) {
  MOZ_ASSERT(cx->realm() == this);

  if (iterResultWithoutPrototypeTemplate_) {
    return iterResultWithoutPrototypeTemplate_;
  }

  PlainObject* templateObj =
      createIterResultTemplateObject(cx, WithObjectPrototype::No);
  iterResultWithoutPrototypeTemplate_.set(templateObj);
  return iterResultWithoutPrototypeTemplate_;
}

PlainObject* Realm::createIterResultTemplateObject(
    JSContext* cx, WithObjectPrototype withProto) {
  // Create template plain object
  Rooted<PlainObject*> templateObject(
      cx, withProto == WithObjectPrototype::Yes
              ? NewTenuredBuiltinClassInstance<PlainObject>(cx)
              : NewObjectWithGivenProto<PlainObject>(cx, nullptr));
  if (!templateObject) {
    return nullptr;
  }

  // Set dummy `value` property
  if (!NativeDefineDataProperty(cx, templateObject, cx->names().value,
                                UndefinedHandleValue, JSPROP_ENUMERATE)) {
    return nullptr;
  }

  // Set dummy `done` property
  if (!NativeDefineDataProperty(cx, templateObject, cx->names().done,
                                TrueHandleValue, JSPROP_ENUMERATE)) {
    return nullptr;
  }

#ifdef DEBUG
  // Make sure that the properties are in the right slots.
  ShapePropertyIter<NoGC> iter(templateObject->shape());
  MOZ_ASSERT(iter->slot() == Realm::IterResultObjectDoneSlot &&
             iter->key() == NameToId(cx->names().done));
  iter++;
  MOZ_ASSERT(iter->slot() == Realm::IterResultObjectValueSlot &&
             iter->key() == NameToId(cx->names().value));
#endif

  return templateObject;
}

/*** Iterator objects *******************************************************/

size_t PropertyIteratorObject::sizeOfMisc(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(getPrivate());
}

void PropertyIteratorObject::trace(JSTracer* trc, JSObject* obj) {
  if (NativeIterator* ni =
          obj->as<PropertyIteratorObject>().getNativeIterator()) {
    ni->trace(trc);
  }
}

void PropertyIteratorObject::finalize(JSFreeOp* fop, JSObject* obj) {
  if (NativeIterator* ni =
          obj->as<PropertyIteratorObject>().getNativeIterator()) {
    fop->free_(obj, ni, ni->allocationSize(), MemoryUse::NativeIterator);
  }
}

const JSClassOps PropertyIteratorObject::classOps_ = {
    nullptr,   // addProperty
    nullptr,   // delProperty
    nullptr,   // enumerate
    nullptr,   // newEnumerate
    nullptr,   // resolve
    nullptr,   // mayResolve
    finalize,  // finalize
    nullptr,   // call
    nullptr,   // hasInstance
    nullptr,   // construct
    trace,     // trace
};

const JSClass PropertyIteratorObject::class_ = {
    "Iterator", JSCLASS_HAS_PRIVATE | JSCLASS_BACKGROUND_FINALIZE,
    &PropertyIteratorObject::classOps_};

static const JSClass ArrayIteratorPrototypeClass = {"Array Iterator", 0};

enum {
  ArrayIteratorSlotIteratedObject,
  ArrayIteratorSlotNextIndex,
  ArrayIteratorSlotItemKind,
  ArrayIteratorSlotCount
};

const JSClass ArrayIteratorObject::class_ = {
    "Array Iterator", JSCLASS_HAS_RESERVED_SLOTS(ArrayIteratorSlotCount)};

ArrayIteratorObject* js::NewArrayIteratorTemplate(JSContext* cx) {
  RootedObject proto(
      cx, GlobalObject::getOrCreateArrayIteratorPrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  return NewTenuredObjectWithGivenProto<ArrayIteratorObject>(cx, proto);
}

ArrayIteratorObject* js::NewArrayIterator(JSContext* cx) {
  RootedObject proto(
      cx, GlobalObject::getOrCreateArrayIteratorPrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  return NewObjectWithGivenProto<ArrayIteratorObject>(cx, proto);
}

static const JSFunctionSpec array_iterator_methods[] = {
    JS_SELF_HOSTED_FN("next", "ArrayIteratorNext", 0, 0), JS_FS_END};

static const JSClass StringIteratorPrototypeClass = {"String Iterator", 0};

enum {
  StringIteratorSlotIteratedObject,
  StringIteratorSlotNextIndex,
  StringIteratorSlotCount
};

const JSClass StringIteratorObject::class_ = {
    "String Iterator", JSCLASS_HAS_RESERVED_SLOTS(StringIteratorSlotCount)};

static const JSFunctionSpec string_iterator_methods[] = {
    JS_SELF_HOSTED_FN("next", "StringIteratorNext", 0, 0), JS_FS_END};

StringIteratorObject* js::NewStringIteratorTemplate(JSContext* cx) {
  RootedObject proto(
      cx, GlobalObject::getOrCreateStringIteratorPrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  return NewTenuredObjectWithGivenProto<StringIteratorObject>(cx, proto);
}

StringIteratorObject* js::NewStringIterator(JSContext* cx) {
  RootedObject proto(
      cx, GlobalObject::getOrCreateStringIteratorPrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  return NewObjectWithGivenProto<StringIteratorObject>(cx, proto);
}

static const JSClass RegExpStringIteratorPrototypeClass = {
    "RegExp String Iterator", 0};

enum {
  // The regular expression used for iteration. May hold the original RegExp
  // object when it is reused instead of a new RegExp object.
  RegExpStringIteratorSlotRegExp,

  // The String value being iterated upon.
  RegExpStringIteratorSlotString,

  // The source string of the original RegExp object. Used to validate we can
  // reuse the original RegExp object for matching.
  RegExpStringIteratorSlotSource,

  // The flags of the original RegExp object.
  RegExpStringIteratorSlotFlags,

  // When non-negative, this slot holds the current lastIndex position when
  // reusing the original RegExp object for matching. When set to |-1|, the
  // iterator has finished. When set to any other negative value, the
  // iterator is not yet exhausted and we're not on the fast path and we're
  // not reusing the input RegExp object.
  RegExpStringIteratorSlotLastIndex,

  RegExpStringIteratorSlotCount
};

static_assert(RegExpStringIteratorSlotRegExp ==
                  REGEXP_STRING_ITERATOR_REGEXP_SLOT,
              "RegExpStringIteratorSlotRegExp must match self-hosting define "
              "for regexp slot.");
static_assert(RegExpStringIteratorSlotString ==
                  REGEXP_STRING_ITERATOR_STRING_SLOT,
              "RegExpStringIteratorSlotString must match self-hosting define "
              "for string slot.");
static_assert(RegExpStringIteratorSlotSource ==
                  REGEXP_STRING_ITERATOR_SOURCE_SLOT,
              "RegExpStringIteratorSlotString must match self-hosting define "
              "for source slot.");
static_assert(RegExpStringIteratorSlotFlags ==
                  REGEXP_STRING_ITERATOR_FLAGS_SLOT,
              "RegExpStringIteratorSlotFlags must match self-hosting define "
              "for flags slot.");
static_assert(RegExpStringIteratorSlotLastIndex ==
                  REGEXP_STRING_ITERATOR_LASTINDEX_SLOT,
              "RegExpStringIteratorSlotLastIndex must match self-hosting "
              "define for lastIndex slot.");

const JSClass RegExpStringIteratorObject::class_ = {
    "RegExp String Iterator",
    JSCLASS_HAS_RESERVED_SLOTS(RegExpStringIteratorSlotCount)};

static const JSFunctionSpec regexp_string_iterator_methods[] = {
    JS_SELF_HOSTED_FN("next", "RegExpStringIteratorNext", 0, 0),

    JS_FS_END};

RegExpStringIteratorObject* js::NewRegExpStringIteratorTemplate(JSContext* cx) {
  RootedObject proto(cx, GlobalObject::getOrCreateRegExpStringIteratorPrototype(
                             cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  return NewTenuredObjectWithGivenProto<RegExpStringIteratorObject>(cx, proto);
}

RegExpStringIteratorObject* js::NewRegExpStringIterator(JSContext* cx) {
  RootedObject proto(cx, GlobalObject::getOrCreateRegExpStringIteratorPrototype(
                             cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  return NewObjectWithGivenProto<RegExpStringIteratorObject>(cx, proto);
}

JSObject* js::ValueToIterator(JSContext* cx, HandleValue vp) {
  RootedObject obj(cx);
  if (vp.isObject()) {
    /* Common case. */
    obj = &vp.toObject();
  } else if (vp.isNullOrUndefined()) {
    /*
     * Enumerating over null and undefined gives an empty enumerator, so
     * that |for (var p in <null or undefined>) <loop>;| never executes
     * <loop>, per ES5 12.6.4.
     */
    RootedIdVector props(cx);  // Empty
    return CreatePropertyIterator(cx, nullptr, props, 0, 0);
  } else {
    obj = ToObject(cx, vp);
    if (!obj) {
      return nullptr;
    }
  }

  return GetIterator(cx, obj);
}

void js::CloseIterator(JSObject* obj) {
  if (obj->is<PropertyIteratorObject>()) {
    /* Remove enumerators from the active list, which is a stack. */
    NativeIterator* ni = obj->as<PropertyIteratorObject>().getNativeIterator();

    ni->unlink();

    MOZ_ASSERT(ni->isActive());
    ni->markInactive();

    // Reset the enumerator; it may still be in the cached iterators for
    // this thread and can be reused.
    ni->resetPropertyCursorForReuse();
  }
}

bool js::IteratorCloseForException(JSContext* cx, HandleObject obj) {
  MOZ_ASSERT(cx->isExceptionPending());

  bool isClosingGenerator = cx->isClosingGenerator();
  JS::AutoSaveExceptionState savedExc(cx);

  // Implements IteratorClose (ES 7.4.6) for exception unwinding. See
  // also the bytecode generated by BytecodeEmitter::emitIteratorClose.

  // Step 3.
  //
  // Get the "return" method.
  RootedValue returnMethod(cx);
  if (!GetProperty(cx, obj, obj, cx->names().return_, &returnMethod)) {
    return false;
  }

  // Step 4.
  //
  // Do nothing if "return" is null or undefined. Throw a TypeError if the
  // method is not IsCallable.
  if (returnMethod.isNullOrUndefined()) {
    return true;
  }
  if (!IsCallable(returnMethod)) {
    return ReportIsNotFunction(cx, returnMethod);
  }

  // Step 5, 6, 8.
  //
  // Call "return" if it is not null or undefined.
  RootedValue rval(cx);
  bool ok = Call(cx, returnMethod, obj, &rval);
  if (isClosingGenerator) {
    // Closing an iterator is implemented as an exception, but in spec
    // terms it is a Completion value with [[Type]] return. In this case
    // we *do* care if the call threw and if it returned an object.
    if (!ok) {
      return false;
    }
    if (!rval.isObject()) {
      return ThrowCheckIsObject(cx, CheckIsObjectKind::IteratorReturn);
    }
  } else {
    // We don't care if the call threw or that it returned an Object, as
    // Step 6 says if IteratorClose is being called during a throw, the
    // original throw has primacy.
    savedExc.restore();
  }

  return true;
}

void js::UnwindIteratorForUncatchableException(JSObject* obj) {
  if (obj->is<PropertyIteratorObject>()) {
    NativeIterator* ni = obj->as<PropertyIteratorObject>().getNativeIterator();
    ni->unlink();
  }
}

static bool SuppressDeletedProperty(JSContext* cx, NativeIterator* ni,
                                    HandleObject obj,
                                    Handle<JSLinearString*> str) {
  if (ni->objectBeingIterated() != obj) {
    return true;
  }

  // Optimization for the following common case:
  //
  //    for (var p in o) {
  //        delete o[p];
  //    }
  //
  // Note that usually both strings will be atoms so we only check for pointer
  // equality here.
  if (ni->previousPropertyWas(str)) {
    return true;
  }

  while (true) {
    bool restart = false;

    // Check whether id is still to come.
    GCPtrLinearString* const cursor = ni->nextProperty();
    GCPtrLinearString* const end = ni->propertiesEnd();
    for (GCPtrLinearString* idp = cursor; idp < end; ++idp) {
      // Common case: both strings are atoms.
      if ((*idp)->isAtom() && str->isAtom()) {
        if (*idp != str) {
          continue;
        }
      } else {
        if (!EqualStrings(*idp, str)) {
          continue;
        }
      }

      // Check whether another property along the prototype chain became
      // visible as a result of this deletion.
      RootedObject proto(cx);
      if (!GetPrototype(cx, obj, &proto)) {
        return false;
      }
      if (proto) {
        RootedId id(cx);
        RootedValue idv(cx, StringValue(*idp));
        if (!PrimitiveValueToId<CanGC>(cx, idv, &id)) {
          return false;
        }

        Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
        RootedObject holder(cx);
        if (!GetPropertyDescriptor(cx, proto, id, &desc, &holder)) {
          return false;
        }

        if (desc.isSome() && desc->enumerable()) {
          continue;
        }
      }

      // If GetPropertyDescriptor above removed a property from ni, start
      // over.
      if (end != ni->propertiesEnd() || cursor != ni->nextProperty()) {
        restart = true;
        break;
      }

      // No property along the prototype chain stepped in to take the
      // property's place, so go ahead and delete id from the list.
      // If it is the next property to be enumerated, just skip it.
      if (idp == cursor) {
        ni->incCursor();
      } else {
        for (GCPtrLinearString* p = idp; p + 1 != end; p++) {
          *p = *(p + 1);
        }

        ni->trimLastProperty();
      }

      ni->markHasUnvisitedPropertyDeletion();
      return true;
    }

    if (!restart) {
      return true;
    }
  }
}

/*
 * Suppress enumeration of deleted properties. This function must be called
 * when a property is deleted and there might be active enumerators.
 *
 * We maintain a list of active non-escaping for-in enumerators. To suppress
 * a property, we check whether each active enumerator contains the (obj, id)
 * pair and has not yet enumerated |id|. If so, and |id| is the next property,
 * we simply advance the cursor. Otherwise, we delete |id| from the list.
 *
 * We do not suppress enumeration of a property deleted along an object's
 * prototype chain. Only direct deletions on the object are handled.
 */
static bool SuppressDeletedPropertyHelper(JSContext* cx, HandleObject obj,
                                          Handle<JSLinearString*> str) {
  NativeIterator* enumeratorList = ObjectRealm::get(obj).enumerators;
  NativeIterator* ni = enumeratorList->next();

  while (ni != enumeratorList) {
    if (!SuppressDeletedProperty(cx, ni, obj, str)) {
      return false;
    }
    ni = ni->next();
  }

  return true;
}

bool js::SuppressDeletedProperty(JSContext* cx, HandleObject obj, jsid id) {
  if (MOZ_LIKELY(!ObjectRealm::get(obj).objectMaybeInIteration(obj))) {
    return true;
  }

  if (id.isSymbol()) {
    return true;
  }

  Rooted<JSLinearString*> str(cx, IdToString(cx, id));
  if (!str) {
    return false;
  }
  return SuppressDeletedPropertyHelper(cx, obj, str);
}

bool js::SuppressDeletedElement(JSContext* cx, HandleObject obj,
                                uint32_t index) {
  if (MOZ_LIKELY(!ObjectRealm::get(obj).objectMaybeInIteration(obj))) {
    return true;
  }

  RootedId id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }

  Rooted<JSLinearString*> str(cx, IdToString(cx, id));
  if (!str) {
    return false;
  }
  return SuppressDeletedPropertyHelper(cx, obj, str);
}

#ifdef DEBUG
void js::AssertDenseElementsNotIterated(NativeObject* obj) {
  // Search for active iterators for |obj| and assert they don't contain any
  // property keys that are dense elements. This is used to check correctness
  // of the MAYBE_IN_ITERATION flag on ObjectElements.
  //
  // Ignore iterators that may contain indexed properties from objects on the
  // prototype chain, as that can result in false positives. See bug 1656744.

  // Limit the number of properties we check to avoid slowing down debug builds
  // too much.
  static constexpr uint32_t MaxPropsToCheck = 10;
  uint32_t propsChecked = 0;

  NativeIterator* enumeratorList = ObjectRealm::get(obj).enumerators;
  NativeIterator* ni = enumeratorList->next();

  while (ni != enumeratorList) {
    if (ni->objectBeingIterated() == obj &&
        !ni->maybeHasIndexedPropertiesFromProto()) {
      for (GCPtrLinearString* idp = ni->nextProperty();
           idp < ni->propertiesEnd(); ++idp) {
        uint32_t index;
        if (idp->get()->isIndex(&index)) {
          MOZ_ASSERT(!obj->containsDenseElement(index));
        }
        if (++propsChecked > MaxPropsToCheck) {
          return;
        }
      }
    }
    ni = ni->next();
  }
}
#endif

static const JSFunctionSpec iterator_methods[] = {
    JS_SELF_HOSTED_SYM_FN(iterator, "IteratorIdentity", 0, 0), JS_FS_END};

static const JSFunctionSpec iterator_static_methods[] = {
    JS_SELF_HOSTED_FN("from", "IteratorFrom", 1, 0), JS_FS_END};

// These methods are only attached to Iterator.prototype when the
// Iterator Helpers feature is enabled.
static const JSFunctionSpec iterator_methods_with_helpers[] = {
    JS_SELF_HOSTED_FN("map", "IteratorMap", 1, 0),
    JS_SELF_HOSTED_FN("filter", "IteratorFilter", 1, 0),
    JS_SELF_HOSTED_FN("take", "IteratorTake", 1, 0),
    JS_SELF_HOSTED_FN("drop", "IteratorDrop", 1, 0),
    JS_SELF_HOSTED_FN("asIndexedPairs", "IteratorAsIndexedPairs", 0, 0),
    JS_SELF_HOSTED_FN("flatMap", "IteratorFlatMap", 1, 0),
    JS_SELF_HOSTED_FN("reduce", "IteratorReduce", 1, 0),
    JS_SELF_HOSTED_FN("toArray", "IteratorToArray", 0, 0),
    JS_SELF_HOSTED_FN("forEach", "IteratorForEach", 1, 0),
    JS_SELF_HOSTED_FN("some", "IteratorSome", 1, 0),
    JS_SELF_HOSTED_FN("every", "IteratorEvery", 1, 0),
    JS_SELF_HOSTED_FN("find", "IteratorFind", 1, 0),
    JS_SELF_HOSTED_SYM_FN(iterator, "IteratorIdentity", 0, 0),
    JS_FS_END};

/* static */
bool GlobalObject::initIteratorProto(JSContext* cx,
                                     Handle<GlobalObject*> global) {
  if (global->getReservedSlot(ITERATOR_PROTO).isObject()) {
    return true;
  }

  RootedObject proto(
      cx, GlobalObject::createBlankPrototype<PlainObject>(cx, global));
  if (!proto) {
    return false;
  }

  // %IteratorPrototype%.map.[[Prototype]] is %Generator% and
  // %Generator%.prototype.[[Prototype]] is %IteratorPrototype%.
  // Populate the slot early, to prevent runaway mutual recursion.
  global->setReservedSlot(ITERATOR_PROTO, ObjectValue(*proto));

  if (!DefinePropertiesAndFunctions(cx, proto, nullptr, iterator_methods)) {
    // In this case, we leave a partially initialized object in the
    // slot. There's no obvious way to do better, since this object may already
    // be in the prototype chain of %GeneratorPrototype%.
    return false;
  }

  return true;
}

/* static */
template <unsigned Slot, const JSClass* ProtoClass,
          const JSFunctionSpec* Methods>
bool GlobalObject::initObjectIteratorProto(JSContext* cx,
                                           Handle<GlobalObject*> global,
                                           HandleAtom tag) {
  if (global->getReservedSlot(Slot).isObject()) {
    return true;
  }

  RootedObject iteratorProto(
      cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
  if (!iteratorProto) {
    return false;
  }

  RootedObject proto(cx, GlobalObject::createBlankPrototypeInheriting(
                             cx, ProtoClass, iteratorProto));
  if (!proto || !DefinePropertiesAndFunctions(cx, proto, nullptr, Methods) ||
      (tag && !DefineToStringTag(cx, proto, tag))) {
    return false;
  }

  global->setReservedSlot(Slot, ObjectValue(*proto));
  return true;
}

/* static */
NativeObject* GlobalObject::getOrCreateArrayIteratorPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  return MaybeNativeObject(getOrCreateObject(
      cx, global, ARRAY_ITERATOR_PROTO, cx->names().ArrayIterator.toHandle(),
      initObjectIteratorProto<ARRAY_ITERATOR_PROTO,
                              &ArrayIteratorPrototypeClass,
                              array_iterator_methods>));
}

/* static */
JSObject* GlobalObject::getOrCreateStringIteratorPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  return getOrCreateObject(
      cx, global, STRING_ITERATOR_PROTO, cx->names().StringIterator.toHandle(),
      initObjectIteratorProto<STRING_ITERATOR_PROTO,
                              &StringIteratorPrototypeClass,
                              string_iterator_methods>);
}

/* static */
JSObject* GlobalObject::getOrCreateRegExpStringIteratorPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  return getOrCreateObject(
      cx, global, REGEXP_STRING_ITERATOR_PROTO,
      cx->names().RegExpStringIterator.toHandle(),
      initObjectIteratorProto<REGEXP_STRING_ITERATOR_PROTO,
                              &RegExpStringIteratorPrototypeClass,
                              regexp_string_iterator_methods>);
}

// Iterator Helper Proposal 2.1.3.1 Iterator()
// https://tc39.es/proposal-iterator-helpers/#sec-iterator as of revision
// ed6e15a
static bool IteratorConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, js_Iterator_str)) {
    return false;
  }
  // Throw TypeError if NewTarget is the active function object, preventing the
  // Iterator constructor from being used directly.
  if (args.callee() == args.newTarget().toObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BOGUS_CONSTRUCTOR, js_Iterator_str);
    return false;
  }

  // Step 2.
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Iterator, &proto)) {
    return false;
  }

  JSObject* obj = NewObjectWithClassProto<IteratorObject>(cx, proto);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static const ClassSpec IteratorObjectClassSpec = {
    GenericCreateConstructor<IteratorConstructor, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<IteratorObject>,
    iterator_static_methods,
    nullptr,
    iterator_methods_with_helpers,
    nullptr,
    nullptr,
};

const JSClass IteratorObject::class_ = {
    js_Iterator_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Iterator),
    JS_NULL_CLASS_OPS,
    &IteratorObjectClassSpec,
};

const JSClass IteratorObject::protoClass_ = {
    "Iterator.prototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Iterator),
    JS_NULL_CLASS_OPS,
    &IteratorObjectClassSpec,
};

// Set up WrapForValidIteratorObject class and its prototype.
static const JSFunctionSpec wrap_for_valid_iterator_methods[] = {
    JS_SELF_HOSTED_FN("next", "WrapForValidIteratorNext", 1, 0),
    JS_SELF_HOSTED_FN("return", "WrapForValidIteratorReturn", 1, 0),
    JS_SELF_HOSTED_FN("throw", "WrapForValidIteratorThrow", 1, 0),
    JS_FS_END,
};

static const JSClass WrapForValidIteratorPrototypeClass = {
    "Wrap For Valid Iterator", 0};

const JSClass WrapForValidIteratorObject::class_ = {
    "Wrap For Valid Iterator",
    JSCLASS_HAS_RESERVED_SLOTS(WrapForValidIteratorObject::SlotCount),
};

/* static */
NativeObject* GlobalObject::getOrCreateWrapForValidIteratorPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  return MaybeNativeObject(getOrCreateObject(
      cx, global, WRAP_FOR_VALID_ITERATOR_PROTO, HandleAtom(nullptr),
      initObjectIteratorProto<WRAP_FOR_VALID_ITERATOR_PROTO,
                              &WrapForValidIteratorPrototypeClass,
                              wrap_for_valid_iterator_methods>));
}

WrapForValidIteratorObject* js::NewWrapForValidIterator(JSContext* cx) {
  RootedObject proto(cx, GlobalObject::getOrCreateWrapForValidIteratorPrototype(
                             cx, cx->global()));
  if (!proto) {
    return nullptr;
  }
  return NewObjectWithGivenProto<WrapForValidIteratorObject>(cx, proto);
}

// Common iterator object returned by Iterator Helper methods.
static const JSFunctionSpec iterator_helper_methods[] = {
    JS_SELF_HOSTED_FN("next", "IteratorHelperNext", 1, 0),
    JS_SELF_HOSTED_FN("return", "IteratorHelperReturn", 1, 0),
    JS_SELF_HOSTED_FN("throw", "IteratorHelperThrow", 1, 0), JS_FS_END};

static const JSClass IteratorHelperPrototypeClass = {"Iterator Helper", 0};

const JSClass IteratorHelperObject::class_ = {
    "Iterator Helper",
    JSCLASS_HAS_RESERVED_SLOTS(IteratorHelperObject::SlotCount),
};

/* static */
NativeObject* GlobalObject::getOrCreateIteratorHelperPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  return MaybeNativeObject(
      getOrCreateObject(cx, global, ITERATOR_HELPER_PROTO, HandleAtom(nullptr),
                        initObjectIteratorProto<ITERATOR_HELPER_PROTO,
                                                &IteratorHelperPrototypeClass,
                                                iterator_helper_methods>));
}

IteratorHelperObject* js::NewIteratorHelper(JSContext* cx) {
  RootedObject proto(
      cx, GlobalObject::getOrCreateIteratorHelperPrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }
  return NewObjectWithGivenProto<IteratorHelperObject>(cx, proto);
}
