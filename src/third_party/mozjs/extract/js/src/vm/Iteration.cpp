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

#include "jsapi.h"
#include "jstypes.h"

#include "builtin/Array.h"
#include "builtin/SelfHostingDefines.h"
#include "ds/Sort.h"
#include "gc/GC.h"
#include "gc/GCContext.h"
#include "js/ForOfIterator.h"         // JS::ForOfIterator
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "util/DifferentialTesting.h"
#include "util/Poison.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"  // js::PlainObject
#include "vm/Shape.h"
#include "vm/StringType.h"
#include "vm/TypedArrayObject.h"

#ifdef ENABLE_RECORD_TUPLE
#  include "builtin/RecordObject.h"
#  include "builtin/TupleObject.h"
#endif

#include "vm/NativeObject-inl.h"
#include "vm/PlainObject-inl.h"  // js::PlainObject::createWithTemplate

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
  std::for_each(shapesBegin(), shapesEnd(), [trc](GCPtr<Shape*>& shape) {
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
  GCPtr<JSLinearString*>* begin =
      MOZ_LIKELY(isInitialized()) ? propertiesBegin() : propertyCursor_;
  std::for_each(begin, propertiesEnd(), [trc](GCPtr<JSLinearString*>& prop) {
    // Properties begin life non-null and never *become*
    // null.  (Deletion-suppression will shift trailing
    // properties over a deleted property in the properties
    // array, but it doesn't null them out.)
    TraceEdge(trc, &prop, "prop");
  });
}

using PropertyKeySet = GCHashSet<PropertyKey, DefaultHasher<PropertyKey>>;

class PropertyEnumerator {
  RootedObject obj_;
  MutableHandleIdVector props_;
  PropertyIndexVector* indices_;

  uint32_t flags_;
  Rooted<PropertyKeySet> visited_;

  bool enumeratingProtoChain_ = false;

  enum class IndicesState {
    // Every property that has been enumerated so far can be represented as a
    // PropertyIndex, but we are not currently producing a list of indices. If
    // the state is Valid when we are done enumerating, then the resulting
    // iterator can be marked as NativeIteratorIndices::AvailableOnRequest.
    Valid,

    // Every property that has been enumerated so far can be represented as a
    // PropertyIndex, and |indices_| points to a PropertyIndexVector containing
    // those indices. This is used when we want to create a NativeIterator with
    // valid indices.
    Allocating,

    // It is not possible to represent every property of the object being
    // enumerated as a PropertyIndex. For example, enumerated properties on the
    // prototype chain are unsupported. We can transition to this state from
    // either of the other two.
    Unsupported
  };
  IndicesState indicesState_;

 public:
  PropertyEnumerator(JSContext* cx, JSObject* obj, uint32_t flags,
                     MutableHandleIdVector props,
                     PropertyIndexVector* indices = nullptr)
      : obj_(cx, obj),
        props_(props),
        indices_(indices),
        flags_(flags),
        visited_(cx, PropertyKeySet(cx)),
        indicesState_(indices ? IndicesState::Allocating
                              : IndicesState::Valid) {}

  bool snapshot(JSContext* cx);

  void markIndicesUnsupported() { indicesState_ = IndicesState::Unsupported; }
  bool supportsIndices() const {
    return indicesState_ != IndicesState::Unsupported;
  }
  bool allocatingIndices() const {
    return indicesState_ == IndicesState::Allocating;
  }

 private:
  template <bool CheckForDuplicates>
  bool enumerate(JSContext* cx, jsid id, bool enumerable,
                 PropertyIndex index = PropertyIndex::Invalid());

  bool enumerateExtraProperties(JSContext* cx);

  template <bool CheckForDuplicates>
  bool enumerateNativeProperties(JSContext* cx);

  bool enumerateNativeProperties(JSContext* cx, bool checkForDuplicates) {
    if (checkForDuplicates) {
      return enumerateNativeProperties<true>(cx);
    }
    return enumerateNativeProperties<false>(cx);
  }

  template <bool CheckForDuplicates>
  bool enumerateProxyProperties(JSContext* cx);

  void reversePropsAndIndicesAfter(size_t initialLength) {
    // We iterate through prop maps in descending order of property creation,
    // but we need our return value to be in ascending order. If we are tracking
    // property indices, make sure to keep them in sync.
    MOZ_ASSERT(props_.begin() + initialLength <= props_.end());
    MOZ_ASSERT_IF(allocatingIndices(), props_.length() == indices_->length());

    std::reverse(props_.begin() + initialLength, props_.end());
    if (allocatingIndices()) {
      std::reverse(indices_->begin() + initialLength, indices_->end());
    }
  }
};

template <bool CheckForDuplicates>
bool PropertyEnumerator::enumerate(JSContext* cx, jsid id, bool enumerable,
                                   PropertyIndex index) {
  if (CheckForDuplicates) {
    // If we've already seen this, we definitely won't add it.
    PropertyKeySet::AddPtr p = visited_.lookupForAdd(id);
    if (MOZ_UNLIKELY(!!p)) {
      return true;
    }

    // It's not necessary to add properties to the hash set at the end of
    // the prototype chain, but custom enumeration behaviors might return
    // duplicated properties, so always add in such cases.
    if (obj_->is<ProxyObject>() || obj_->staticPrototype() ||
        obj_->getClass()->getNewEnumerate()) {
      if (!visited_.add(p, id)) {
        return false;
      }
    }
  }

  if (!enumerable && !(flags_ & JSITER_HIDDEN)) {
    return true;
  }

  // Symbol-keyed properties and nonenumerable properties are skipped unless
  // the caller specifically asks for them. A caller can also filter out
  // non-symbols by asking for JSITER_SYMBOLSONLY. PrivateName symbols are
  // skipped unless JSITER_PRIVATE is passed.
  if (id.isSymbol()) {
    if (!(flags_ & JSITER_SYMBOLS)) {
      return true;
    }
    if (!(flags_ & JSITER_PRIVATE) && id.isPrivateName()) {
      return true;
    }
  } else {
    if ((flags_ & JSITER_SYMBOLSONLY)) {
      return true;
    }
  }

  MOZ_ASSERT_IF(allocatingIndices(), indices_->length() == props_.length());
  if (!props_.append(id)) {
    return false;
  }

  if (!supportsIndices()) {
    return true;
  }
  if (index.kind() == PropertyIndex::Kind::Invalid || enumeratingProtoChain_) {
    markIndicesUnsupported();
    return true;
  }

  if (allocatingIndices() && !indices_->append(index)) {
    return false;
  }

  return true;
}

bool PropertyEnumerator::enumerateExtraProperties(JSContext* cx) {
  MOZ_ASSERT(obj_->getClass()->getNewEnumerate());

  RootedIdVector properties(cx);
  bool enumerableOnly = !(flags_ & JSITER_HIDDEN);
  if (!obj_->getClass()->getNewEnumerate()(cx, obj_, &properties,
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
    if (!enumerate<true>(cx, id, enumerable)) {
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
bool PropertyEnumerator::enumerateNativeProperties(JSContext* cx) {
  Handle<NativeObject*> pobj = obj_.as<NativeObject>();

  // We don't need to iterate over the shape's properties if we're only
  // interested in enumerable properties and the object is known to have no
  // enumerable properties.
  //
  // Don't optimize if CheckForDuplicates is true, because non-enumerable
  // properties still have to participate in duplicate-property checking.
  const bool iterShapeProperties = CheckForDuplicates ||
                                   (flags_ & JSITER_HIDDEN) ||
                                   pobj->hasEnumerableProperty();

  bool enumerateSymbols;
  if (flags_ & JSITER_SYMBOLSONLY) {
    if (!iterShapeProperties) {
      return true;
    }
    enumerateSymbols = true;
  } else {
    // Collect any dense elements from this object.
    size_t firstElemIndex = props_.length();
    size_t initlen = pobj->getDenseInitializedLength();
    const Value* elements = pobj->getDenseElements();
    bool hasHoles = false;
    for (uint32_t i = 0; i < initlen; ++i) {
      if (elements[i].isMagic(JS_ELEMENTS_HOLE)) {
        hasHoles = true;
      } else {
        // Dense arrays never get so large that i would not fit into an
        // integer id.
        if (!enumerate<CheckForDuplicates>(cx, PropertyKey::Int(i),
                                           /* enumerable = */ true,
                                           PropertyIndex::ForElement(i))) {
          return false;
        }
      }
    }

    // Collect any typed array or shared typed array elements from this
    // object.
    if (pobj->is<TypedArrayObject>()) {
      size_t len = pobj->as<TypedArrayObject>().length().valueOr(0);

      // Fail early if the typed array is enormous, because this will be very
      // slow and will likely report OOM. This also means we don't need to
      // handle indices greater than PropertyKey::IntMax in the loop below.
      static_assert(PropertyKey::IntMax == INT32_MAX);
      if (len > INT32_MAX) {
        ReportOutOfMemory(cx);
        return false;
      }

      for (uint32_t i = 0; i < len; i++) {
        if (!enumerate<CheckForDuplicates>(cx, PropertyKey::Int(i),
                                           /* enumerable = */ true)) {
          return false;
        }
      }
    }
#ifdef ENABLE_RECORD_TUPLE
    else {
      Rooted<RecordType*> rec(cx);
      if (RecordObject::maybeUnbox(pobj, &rec)) {
        Rooted<ArrayObject*> keys(cx, rec->keys());

        for (size_t i = 0; i < keys->length(); i++) {
          JSAtom* key = &keys->getDenseElement(i).toString()->asAtom();
          PropertyKey id = AtomToId(key);
          if (!enumerate<CheckForDuplicates>(cx, id,
                                             /* enumerable = */ true)) {
            return false;
          }
        }

        return true;
      } else {
        mozilla::Maybe<TupleType&> tup = TupleObject::maybeUnbox(pobj);
        if (tup) {
          uint32_t len = tup->length();

          for (size_t i = 0; i < len; i++) {
            // We expect tuple indices not to get so large that `i` won't
            // fit into an `int32_t`.
            MOZ_ASSERT(PropertyKey::fitsInInt(i));
            PropertyKey id = PropertyKey::Int(i);
            if (!enumerate<CheckForDuplicates>(cx, id,
                                               /* enumerable = */ true)) {
              return false;
            }
          }

          return true;
        }
      }
    }
#endif

    // The code below enumerates shape properties (including sparse elements) so
    // if we can ignore those we're done.
    if (!iterShapeProperties) {
      return true;
    }

    // Collect any sparse elements from this object.
    bool isIndexed = pobj->isIndexed();
    if (isIndexed) {
      // If the dense elements didn't have holes, we don't need to include
      // them in the sort.
      if (!hasHoles) {
        firstElemIndex = props_.length();
      }

      for (ShapePropertyIter<NoGC> iter(pobj->shape()); !iter.done(); iter++) {
        jsid id = iter->key();
        uint32_t dummy;
        if (IdIsIndex(id, &dummy)) {
          if (!enumerate<CheckForDuplicates>(cx, id, iter->enumerable())) {
            return false;
          }
        }
      }

      MOZ_ASSERT(firstElemIndex <= props_.length());

      jsid* ids = props_.begin() + firstElemIndex;
      size_t n = props_.length() - firstElemIndex;

      RootedIdVector tmp(cx);
      if (!tmp.resize(n)) {
        return false;
      }
      PodCopy(tmp.begin(), ids, n);

      if (!MergeSort(ids, n, tmp.begin(), SortComparatorIntegerIds)) {
        return false;
      }
    }

    size_t initialLength = props_.length();

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

      PropertyIndex index = iter->isDataProperty()
                                ? PropertyIndex::ForSlot(pobj, iter->slot())
                                : PropertyIndex::Invalid();
      if (!enumerate<CheckForDuplicates>(cx, id, iter->enumerable(), index)) {
        return false;
      }
    }
    reversePropsAndIndicesAfter(initialLength);

    enumerateSymbols = symbolsFound && (flags_ & JSITER_SYMBOLS);
  }

  if (enumerateSymbols) {
    MOZ_ASSERT(iterShapeProperties);
    MOZ_ASSERT(!allocatingIndices());

    // Do a second pass to collect symbols. The spec requires that all symbols
    // appear after all strings in [[OwnPropertyKeys]] for ordinary objects:
    // https://tc39.es/ecma262/#sec-ordinaryownpropertykeys
    size_t initialLength = props_.length();
    for (ShapePropertyIter<NoGC> iter(pobj->shape()); !iter.done(); iter++) {
      jsid id = iter->key();
      if (id.isSymbol()) {
        if (!enumerate<CheckForDuplicates>(cx, id, iter->enumerable())) {
          return false;
        }
      }
    }
    reversePropsAndIndicesAfter(initialLength);
  }

  return true;
}

template <bool CheckForDuplicates>
bool PropertyEnumerator::enumerateProxyProperties(JSContext* cx) {
  MOZ_ASSERT(obj_->is<ProxyObject>());

  RootedIdVector proxyProps(cx);

  if (flags_ & JSITER_HIDDEN || flags_ & JSITER_SYMBOLS) {
    // This gets all property keys, both strings and symbols. The call to
    // enumerate in the loop below will filter out unwanted keys, per the
    // flags.
    if (!Proxy::ownPropertyKeys(cx, obj_, &proxyProps)) {
      return false;
    }

    Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
    for (size_t n = 0, len = proxyProps.length(); n < len; n++) {
      bool enumerable = false;

      // We need to filter, if the caller just wants enumerable symbols.
      if (!(flags_ & JSITER_HIDDEN)) {
        if (!Proxy::getOwnPropertyDescriptor(cx, obj_, proxyProps[n], &desc)) {
          return false;
        }
        enumerable = desc.isSome() && desc->enumerable();
      }

      if (!enumerate<CheckForDuplicates>(cx, proxyProps[n], enumerable)) {
        return false;
      }
    }

    return true;
  }

  // Returns enumerable property names (no symbols).
  if (!Proxy::getOwnEnumerablePropertyKeys(cx, obj_, &proxyProps)) {
    return false;
  }

  for (size_t n = 0, len = proxyProps.length(); n < len; n++) {
    if (!enumerate<CheckForDuplicates>(cx, proxyProps[n], true)) {
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

    enum class KeyType { Void, Int, String, Symbol };

    auto keyType = [](PropertyKey key) {
      if (key.isString()) {
        return KeyType::String;
      }
      if (key.isInt()) {
        return KeyType::Int;
      }
      if (key.isSymbol()) {
        return KeyType::Symbol;
      }
      MOZ_ASSERT(key.isVoid());
      return KeyType::Void;
    };

    if (keyType(a) != keyType(b)) {
      *lessOrEqualp = (keyType(a) <= keyType(b));
      return true;
    }

    if (a.isInt()) {
      *lessOrEqualp = (a.toInt() <= b.toInt());
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

static void AssertNoEnumerableProperties(NativeObject* obj) {
#ifdef DEBUG
  // Verify the object has no enumerable properties if the HasEnumerable
  // ObjectFlag is not set.

  MOZ_ASSERT(!obj->hasEnumerableProperty());

  static constexpr size_t MaxPropsToCheck = 5;

  size_t count = 0;
  for (ShapePropertyIter<NoGC> iter(obj->shape()); !iter.done(); iter++) {
    MOZ_ASSERT(!iter->enumerable());
    if (++count > MaxPropsToCheck) {
      break;
    }
  }
#endif  // DEBUG
}

static bool ProtoMayHaveEnumerableProperties(JSObject* obj) {
  if (!obj->is<NativeObject>()) {
    return true;
  }

  JSObject* proto = obj->as<NativeObject>().staticPrototype();
  while (proto) {
    if (!proto->is<NativeObject>()) {
      return true;
    }
    NativeObject* nproto = &proto->as<NativeObject>();
    if (nproto->hasEnumerableProperty() ||
        nproto->getDenseInitializedLength() > 0 ||
        ClassCanHaveExtraEnumeratedProperties(nproto->getClass())) {
      return true;
    }
    AssertNoEnumerableProperties(nproto);
    proto = nproto->staticPrototype();
  }

  return false;
}

bool PropertyEnumerator::snapshot(JSContext* cx) {
  // If we're only interested in enumerable properties and the proto chain has
  // no enumerable properties (the common case), we can optimize this to ignore
  // the proto chain. This also lets us take advantage of the no-duplicate-check
  // optimization below.
  if (!(flags_ & JSITER_HIDDEN) && !(flags_ & JSITER_OWNONLY) &&
      !ProtoMayHaveEnumerableProperties(obj_)) {
    flags_ |= JSITER_OWNONLY;
  }

  // Don't check for duplicates if we're only interested in own properties.
  // This does the right thing for most objects: native objects don't have
  // duplicate property ids and we allow the [[OwnPropertyKeys]] proxy trap to
  // return duplicates.
  //
  // The only special case is when the object has a newEnumerate hook: it
  // can return duplicate properties and we have to filter them. This is
  // handled below.
  bool checkForDuplicates = !(flags_ & JSITER_OWNONLY);

  do {
    if (obj_->getClass()->getNewEnumerate()) {
      markIndicesUnsupported();

      if (!enumerateExtraProperties(cx)) {
        return false;
      }

      if (obj_->is<NativeObject>()) {
        if (!enumerateNativeProperties(cx, /*checkForDuplicates*/ true)) {
          return false;
        }
      }

    } else if (obj_->is<NativeObject>()) {
      // Give the object a chance to resolve all lazy properties
      if (JSEnumerateOp enumerateOp = obj_->getClass()->getEnumerate()) {
        markIndicesUnsupported();
        if (!enumerateOp(cx, obj_.as<NativeObject>())) {
          return false;
        }
      }
      if (!enumerateNativeProperties(cx, checkForDuplicates)) {
        return false;
      }
    } else if (obj_->is<ProxyObject>()) {
      markIndicesUnsupported();
      if (checkForDuplicates) {
        if (!enumerateProxyProperties<true>(cx)) {
          return false;
        }
      } else {
        if (!enumerateProxyProperties<false>(cx)) {
          return false;
        }
      }
    } else {
      MOZ_CRASH("non-native objects must have an enumerate op");
    }

    if (flags_ & JSITER_OWNONLY) {
      break;
    }

    if (!GetPrototype(cx, obj_, &obj_)) {
      return false;
    }
    enumeratingProtoChain_ = true;

    // The [[Prototype]] chain might be cyclic.
    if (!CheckForInterrupt(cx)) {
      return false;
    }
  } while (obj_ != nullptr);

#ifdef DEBUG
  if (js::SupportDifferentialTesting() && !supportsIndices()) {
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

    jsid* ids = props_.begin();
    size_t n = props_.length();

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
  uint32_t validFlags =
      flags & (JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS |
               JSITER_SYMBOLSONLY | JSITER_PRIVATE);

  PropertyEnumerator enumerator(cx, obj, validFlags, props);
  return enumerator.snapshot(cx);
}

static inline void RegisterEnumerator(JSContext* cx, NativeIterator* ni) {
  MOZ_ASSERT(ni->objectBeingIterated());

  // Register non-escaping native enumerators (for-in) with the current
  // context.
  ni->link(cx->compartment()->enumeratorsAddr());

  MOZ_ASSERT(!ni->isActive());
  ni->markActive();
}

static PropertyIteratorObject* NewPropertyIteratorObject(JSContext* cx) {
  const JSClass* clasp = &PropertyIteratorObject::class_;
  Rooted<SharedShape*> shape(
      cx,
      SharedShape::getInitialShape(cx, clasp, cx->realm(), TaggedProto(nullptr),
                                   ITERATOR_FINALIZE_KIND));
  if (!shape) {
    return nullptr;
  }

  auto* res = NativeObject::create<PropertyIteratorObject>(
      cx, ITERATOR_FINALIZE_KIND, GetInitialHeap(GenericObject, clasp), shape);
  if (!res) {
    return nullptr;
  }

  // CodeGenerator::visitIteratorStartO assumes the iterator object is not
  // inside the nursery when deciding whether a barrier is necessary.
  MOZ_ASSERT(!js::gc::IsInsideNursery(res));
  return res;
}

static inline size_t NumTrailingBytes(size_t propertyCount, size_t shapeCount,
                                      bool hasIndices) {
  static_assert(alignof(GCPtr<JSLinearString*>) <= alignof(NativeIterator));
  static_assert(alignof(GCPtr<Shape*>) <= alignof(GCPtr<JSLinearString*>));
  static_assert(alignof(PropertyIndex) <= alignof(GCPtr<Shape*>));
  size_t result = propertyCount * sizeof(GCPtr<JSLinearString*>) +
                  shapeCount * sizeof(GCPtr<Shape*>);
  if (hasIndices) {
    result += propertyCount * sizeof(PropertyIndex);
  }
  return result;
}

static inline size_t AllocationSize(size_t propertyCount, size_t shapeCount,
                                    bool hasIndices) {
  return sizeof(NativeIterator) +
         NumTrailingBytes(propertyCount, shapeCount, hasIndices);
}

static PropertyIteratorObject* CreatePropertyIterator(
    JSContext* cx, Handle<JSObject*> objBeingIterated, HandleIdVector props,
    bool supportsIndices, PropertyIndexVector* indices,
    uint32_t cacheableProtoChainLength) {
  MOZ_ASSERT_IF(indices, supportsIndices);
  if (props.length() >= NativeIterator::PropCountLimit) {
    ReportAllocationOverflow(cx);
    return nullptr;
  }

  bool hasIndices = !!indices;

  // If the iterator is cacheable, we store the shape of each object
  // along the proto chain in the iterator. If the iterator is not
  // cacheable, but has indices, then we store one shape (the shape of
  // the object being iterated.)
  uint32_t numShapes = cacheableProtoChainLength;
  if (numShapes == 0 && hasIndices) {
    numShapes = 1;
  }

  Rooted<PropertyIteratorObject*> propIter(cx, NewPropertyIteratorObject(cx));
  if (!propIter) {
    return nullptr;
  }

  void* mem = cx->pod_malloc_with_extra<NativeIterator, uint8_t>(
      NumTrailingBytes(props.length(), numShapes, hasIndices));
  if (!mem) {
    return nullptr;
  }

  // This also registers |ni| with |propIter|.
  bool hadError = false;
  new (mem) NativeIterator(cx, propIter, objBeingIterated, props,
                           supportsIndices, indices, numShapes, &hadError);
  if (hadError) {
    return nullptr;
  }

  return propIter;
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
                               HandleIdVector props, bool supportsIndices,
                               PropertyIndexVector* indices, uint32_t numShapes,
                               bool* hadError)
    : objectBeingIterated_(objBeingIterated),
      iterObj_(propIter),
      // NativeIterator initially acts (before full initialization) as if it
      // contains no shapes...
      shapesEnd_(shapesBegin()),
      // ...and no properties.
      propertyCursor_(
          reinterpret_cast<GCPtr<JSLinearString*>*>(shapesBegin() + numShapes)),
      propertiesEnd_(propertyCursor_),
      shapesHash_(0),
      flagsAndCount_(
          initialFlagsAndCount(props.length()))  // note: no Flags::Initialized
{
  // If there are shapes, the object and all objects on its prototype chain must
  // be native objects. See CanCompareIterableObjectToCache.
  MOZ_ASSERT_IF(numShapes > 0,
                objBeingIterated && objBeingIterated->is<NativeObject>());

  MOZ_ASSERT(!*hadError);

  bool hasActualIndices = !!indices;
  MOZ_ASSERT_IF(hasActualIndices, indices->length() == props.length());

  // NOTE: This must be done first thing: The caller can't free `this` on error
  //       because it has GCPtr fields whose barriers have already fired; the
  //       store buffer has pointers to them. Only the GC can free `this` (via
  //       PropertyIteratorObject::finalize).
  propIter->initNativeIterator(this);

  // The GC asserts on finalization that `this->allocationSize()` matches the
  // `nbytes` passed to `AddCellMemory`. So once these lines run, we must make
  // `this->allocationSize()` correct. That means infallibly initializing the
  // shapes, and ensuring that indicesState_.allocated() is true if we've
  // allocated space for indices. It's OK for the constructor to fail after
  // that.
  size_t nbytes = AllocationSize(props.length(), numShapes, hasActualIndices);
  AddCellMemory(propIter, nbytes, MemoryUse::NativeIterator);
  if (supportsIndices) {
    if (hasActualIndices) {
      // If the string allocation fails, indicesAllocated() must be true
      // so that this->allocationSize() is correct. Set it to Disabled. It will
      // be updated below.
      setIndicesState(NativeIteratorIndices::Disabled);
    } else {
      // This object supports indices (ie it only has own enumerable
      // properties), but we didn't allocate them because we haven't seen a
      // consumer yet. We mark the iterator so that potential consumers know to
      // request a fresh iterator with indices.
      setIndicesState(NativeIteratorIndices::AvailableOnRequest);
    }
  }

  if (numShapes > 0) {
    // Construct shapes into the shapes array.  Also compute the shapesHash,
    // which incorporates Shape* addresses that could have changed during a GC
    // triggered in (among other places) |IdToString| above.
    JSObject* pobj = objBeingIterated;
    HashNumber shapesHash = 0;
    for (uint32_t i = 0; i < numShapes; i++) {
      MOZ_ASSERT(pobj->is<NativeObject>());
      Shape* shape = pobj->shape();
      new (shapesEnd_) GCPtr<Shape*>(shape);
      shapesEnd_++;
      shapesHash = mozilla::AddToHash(shapesHash, HashIteratorShape(shape));
      pobj = pobj->staticPrototype();
    }
    shapesHash_ = shapesHash;

    // There are two cases in which we need to store shapes. If this
    // iterator is cacheable, we store the shapes for the entire proto
    // chain so we can check that the cached iterator is still valid
    // (see MacroAssembler::maybeLoadIteratorFromShape). If this iterator
    // has indices, then even if it isn't cacheable we need to store the
    // shape of the iterated object itself (see IteratorHasIndicesAndBranch).
    // In the former case, assert that we're storing the entire proto chain.
    MOZ_ASSERT_IF(numShapes > 1, pobj == nullptr);
  }
  MOZ_ASSERT(static_cast<void*>(shapesEnd_) == propertyCursor_);

  // Allocate any strings in the nursery until the first minor GC. After this
  // point they will end up getting tenured anyway because they are reachable
  // from |propIter| which will be tenured.
  AutoSelectGCHeap gcHeap(cx);

  size_t numProps = props.length();
  for (size_t i = 0; i < numProps; i++) {
    JSLinearString* str = IdToString(cx, props[i], gcHeap);
    if (!str) {
      *hadError = true;
      return;
    }
    new (propertiesEnd_) GCPtr<JSLinearString*>(str);
    propertiesEnd_++;
  }

  if (hasActualIndices) {
    PropertyIndex* cursor = indicesBegin();
    for (size_t i = 0; i < numProps; i++) {
      *cursor++ = (*indices)[i];
    }
    MOZ_ASSERT(uintptr_t(cursor) == uintptr_t(this) + nbytes);
    setIndicesState(NativeIteratorIndices::Valid);
  }

  markInitialized();

  MOZ_ASSERT(!*hadError);
}

inline size_t NativeIterator::allocationSize() const {
  size_t numShapes = shapesEnd() - shapesBegin();

  return AllocationSize(initialPropertyCount(), numShapes, indicesAllocated());
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

static bool CanStoreInIteratorCache(JSObject* obj) {
  do {
    MOZ_ASSERT(obj->as<NativeObject>().getDenseInitializedLength() == 0);

    // Typed arrays have indexed properties not captured by the Shape guard.
    // Enumerate hooks may add extra properties.
    if (MOZ_UNLIKELY(ClassCanHaveExtraEnumeratedProperties(obj->getClass()))) {
      return false;
    }

    obj = obj->staticPrototype();
  } while (obj);

  return true;
}

static MOZ_ALWAYS_INLINE PropertyIteratorObject* LookupInShapeIteratorCache(
    JSContext* cx, JSObject* obj, uint32_t* cacheableProtoChainLength) {
  if (!obj->shape()->cache().isIterator() ||
      !CanCompareIterableObjectToCache(obj)) {
    return nullptr;
  }
  PropertyIteratorObject* iterobj = obj->shape()->cache().toIterator();
  NativeIterator* ni = iterobj->getNativeIterator();
  MOZ_ASSERT(*ni->shapesBegin() == obj->shape());
  if (!ni->isReusable()) {
    return nullptr;
  }

  // Verify shapes of proto chain.
  JSObject* pobj = obj;
  for (GCPtr<Shape*>* s = ni->shapesBegin() + 1; s != ni->shapesEnd(); s++) {
    Shape* shape = *s;
    pobj = pobj->staticPrototype();
    if (pobj->shape() != shape) {
      return nullptr;
    }
    if (!CanCompareIterableObjectToCache(pobj)) {
      return nullptr;
    }
  }
  MOZ_ASSERT(CanStoreInIteratorCache(obj));
  *cacheableProtoChainLength = ni->shapeCount();
  return iterobj;
}

static MOZ_ALWAYS_INLINE PropertyIteratorObject* LookupInIteratorCache(
    JSContext* cx, JSObject* obj, uint32_t* cacheableProtoChainLength) {
  MOZ_ASSERT(*cacheableProtoChainLength == 0);

  if (PropertyIteratorObject* shapeCached =
          LookupInShapeIteratorCache(cx, obj, cacheableProtoChainLength)) {
    return shapeCached;
  }

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
  *cacheableProtoChainLength = shapes.length();

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

[[nodiscard]] static bool StoreInIteratorCache(
    JSContext* cx, JSObject* obj, PropertyIteratorObject* iterobj) {
  MOZ_ASSERT(CanStoreInIteratorCache(obj));

  NativeIterator* ni = iterobj->getNativeIterator();
  MOZ_ASSERT(ni->shapeCount() > 0);

  obj->shape()->maybeCacheIterator(cx, iterobj);

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

  uint32_t flags = 0;
  PropertyEnumerator enumerator(cx, obj, flags, props);
  return enumerator.snapshot(cx);
}

#ifdef DEBUG
static bool IndicesAreValid(NativeObject* obj, NativeIterator* ni) {
  MOZ_ASSERT(ni->hasValidIndices());
  size_t numDenseElements = obj->getDenseInitializedLength();
  size_t numFixedSlots = obj->numFixedSlots();
  const Value* elements = obj->getDenseElements();

  GCPtr<JSLinearString*>* keys = ni->propertiesBegin();
  PropertyIndex* indices = ni->indicesBegin();

  for (uint32_t i = 0; i < ni->numKeys(); i++) {
    PropertyIndex index = indices[i];
    switch (index.kind()) {
      case PropertyIndex::Kind::Element:
        // Verify that the dense element exists and is not a hole.
        if (index.index() >= numDenseElements ||
            elements[index.index()].isMagic(JS_ELEMENTS_HOLE)) {
          return false;
        }
        break;
      case PropertyIndex::Kind::FixedSlot: {
        // Verify that the slot exists and is an enumerable data property with
        // the expected key.
        Maybe<PropertyInfo> prop =
            obj->lookupPure(AtomToId(&keys[i]->asAtom()));
        if (!prop.isSome() || !prop->hasSlot() || !prop->enumerable() ||
            !prop->isDataProperty() || prop->slot() != index.index()) {
          return false;
        }
        break;
      }
      case PropertyIndex::Kind::DynamicSlot: {
        // Verify that the slot exists and is an enumerable data property with
        // the expected key.
        Maybe<PropertyInfo> prop =
            obj->lookupPure(AtomToId(&keys[i]->asAtom()));
        if (!prop.isSome() || !prop->hasSlot() || !prop->enumerable() ||
            !prop->isDataProperty() ||
            prop->slot() - numFixedSlots != index.index()) {
          return false;
        }
        break;
      }
      case PropertyIndex::Kind::Invalid:
        return false;
    }
  }
  return true;
}
#endif

template <bool WantIndices>
static PropertyIteratorObject* GetIteratorImpl(JSContext* cx,
                                               HandleObject obj) {
  MOZ_ASSERT(!obj->is<PropertyIteratorObject>());
  MOZ_ASSERT(cx->compartment() == obj->compartment(),
             "We may end up allocating shapes in the wrong zone!");

  uint32_t cacheableProtoChainLength = 0;
  if (PropertyIteratorObject* iterobj =
          LookupInIteratorCache(cx, obj, &cacheableProtoChainLength)) {
    NativeIterator* ni = iterobj->getNativeIterator();
    bool recreateWithIndices = WantIndices && ni->indicesAvailableOnRequest();
    if (!recreateWithIndices) {
      MOZ_ASSERT_IF(WantIndices && ni->hasValidIndices(),
                    IndicesAreValid(&obj->as<NativeObject>(), ni));
      ni->initObjectBeingIterated(*obj);
      RegisterEnumerator(cx, ni);
      return iterobj;
    }
  }

  if (cacheableProtoChainLength > 0 && !CanStoreInIteratorCache(obj)) {
    cacheableProtoChainLength = 0;
  }

  RootedIdVector keys(cx);
  PropertyIndexVector indices(cx);
  bool supportsIndices = false;

  if (MOZ_UNLIKELY(obj->is<ProxyObject>())) {
    if (!Proxy::enumerate(cx, obj, &keys)) {
      return nullptr;
    }
  } else {
    uint32_t flags = 0;
    PropertyEnumerator enumerator(cx, obj, flags, &keys, &indices);
    if (!enumerator.snapshot(cx)) {
      return nullptr;
    }
    supportsIndices = enumerator.supportsIndices();
    MOZ_ASSERT_IF(WantIndices && supportsIndices,
                  keys.length() == indices.length());
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

  PropertyIndexVector* indicesPtr =
      WantIndices && supportsIndices ? &indices : nullptr;
  PropertyIteratorObject* iterobj = CreatePropertyIterator(
      cx, obj, keys, supportsIndices, indicesPtr, cacheableProtoChainLength);
  if (!iterobj) {
    return nullptr;
  }
  RegisterEnumerator(cx, iterobj->getNativeIterator());

  cx->check(iterobj);
  MOZ_ASSERT_IF(
      WantIndices && supportsIndices,
      IndicesAreValid(&obj->as<NativeObject>(), iterobj->getNativeIterator()));

#ifdef DEBUG
  if (obj->is<NativeObject>()) {
    if (PrototypeMayHaveIndexedProperties(&obj->as<NativeObject>())) {
      iterobj->getNativeIterator()->setMaybeHasIndexedPropertiesFromProto();
    }
  }
#endif

  // Cache the iterator object.
  if (cacheableProtoChainLength > 0) {
    if (!StoreInIteratorCache(cx, obj, iterobj)) {
      return nullptr;
    }
  }

  return iterobj;
}

PropertyIteratorObject* js::GetIterator(JSContext* cx, HandleObject obj) {
  return GetIteratorImpl<false>(cx, obj);
}

PropertyIteratorObject* js::GetIteratorWithIndices(JSContext* cx,
                                                   HandleObject obj) {
  return GetIteratorImpl<true>(cx, obj);
}

PropertyIteratorObject* js::LookupInIteratorCache(JSContext* cx,
                                                  HandleObject obj) {
  uint32_t dummy = 0;
  return LookupInIteratorCache(cx, obj, &dummy);
}

PropertyIteratorObject* js::LookupInShapeIteratorCache(JSContext* cx,
                                                       HandleObject obj) {
  uint32_t dummy = 0;
  return LookupInShapeIteratorCache(cx, obj, &dummy);
}

// ES 2017 draft 7.4.7.
PlainObject* js::CreateIterResultObject(JSContext* cx, HandleValue value,
                                        bool done) {
  // Step 1 (implicit).

  // Step 2.
  Rooted<PlainObject*> templateObject(
      cx, GlobalObject::getOrCreateIterResultTemplateObject(cx));
  if (!templateObject) {
    return nullptr;
  }

  PlainObject* resultObj = PlainObject::createWithTemplate(cx, templateObject);
  if (!resultObj) {
    return nullptr;
  }

  // Step 3.
  resultObj->setSlot(GlobalObject::IterResultObjectValueSlot, value);

  // Step 4.
  resultObj->setSlot(GlobalObject::IterResultObjectDoneSlot,
                     done ? TrueHandleValue : FalseHandleValue);

  // Step 5.
  return resultObj;
}

PlainObject* GlobalObject::getOrCreateIterResultTemplateObject(JSContext* cx) {
  HeapPtr<PlainObject*>& obj = cx->global()->data().iterResultTemplate;
  if (obj) {
    return obj;
  }

  PlainObject* templateObj =
      createIterResultTemplateObject(cx, WithObjectPrototype::Yes);
  obj.init(templateObj);
  return obj;
}

/* static */
PlainObject* GlobalObject::getOrCreateIterResultWithoutPrototypeTemplateObject(
    JSContext* cx) {
  HeapPtr<PlainObject*>& obj =
      cx->global()->data().iterResultWithoutPrototypeTemplate;
  if (obj) {
    return obj;
  }

  PlainObject* templateObj =
      createIterResultTemplateObject(cx, WithObjectPrototype::No);
  obj.init(templateObj);
  return obj;
}

/* static */
PlainObject* GlobalObject::createIterResultTemplateObject(
    JSContext* cx, WithObjectPrototype withProto) {
  // Create template plain object
  Rooted<PlainObject*> templateObject(
      cx, withProto == WithObjectPrototype::Yes
              ? NewPlainObject(cx, TenuredObject)
              : NewPlainObjectWithProto(cx, nullptr));
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
  MOZ_ASSERT(iter->slot() == GlobalObject::IterResultObjectDoneSlot &&
             iter->key() == NameToId(cx->names().done));
  iter++;
  MOZ_ASSERT(iter->slot() == GlobalObject::IterResultObjectValueSlot &&
             iter->key() == NameToId(cx->names().value));
#endif

  return templateObject;
}

/*** Iterator objects *******************************************************/

size_t PropertyIteratorObject::sizeOfMisc(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(getNativeIterator());
}

void PropertyIteratorObject::trace(JSTracer* trc, JSObject* obj) {
  if (NativeIterator* ni =
          obj->as<PropertyIteratorObject>().getNativeIterator()) {
    ni->trace(trc);
  }
}

void PropertyIteratorObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  if (NativeIterator* ni =
          obj->as<PropertyIteratorObject>().getNativeIterator()) {
    gcx->free_(obj, ni, ni->allocationSize(), MemoryUse::NativeIterator);
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
    nullptr,   // construct
    trace,     // trace
};

const JSClass PropertyIteratorObject::class_ = {
    "Iterator",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount) | JSCLASS_BACKGROUND_FINALIZE,
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

// static
PropertyIteratorObject* GlobalObject::getOrCreateEmptyIterator(JSContext* cx) {
  if (!cx->global()->data().emptyIterator) {
    RootedIdVector props(cx);  // Empty
    PropertyIteratorObject* iter =
        CreatePropertyIterator(cx, nullptr, props, false, nullptr, 0);
    if (!iter) {
      return nullptr;
    }
    iter->getNativeIterator()->markEmptyIteratorSingleton();
    cx->global()->data().emptyIterator.init(iter);
  }
  return cx->global()->data().emptyIterator;
}

PropertyIteratorObject* js::ValueToIterator(JSContext* cx, HandleValue vp) {
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
    return GlobalObject::getOrCreateEmptyIterator(cx);
  } else {
    obj = ToObject(cx, vp);
    if (!obj) {
      return nullptr;
    }
  }

  return GetIterator(cx, obj);
}

void js::CloseIterator(JSObject* obj) {
  if (!obj->is<PropertyIteratorObject>()) {
    return;
  }

  // Remove iterator from the active list, which is a stack. The shared iterator
  // used for for-in with null/undefined is immutable and unlinked.

  NativeIterator* ni = obj->as<PropertyIteratorObject>().getNativeIterator();
  if (ni->isEmptyIteratorSingleton()) {
    return;
  }

  ni->unlink();

  MOZ_ASSERT(ni->isActive());
  ni->markInactive();

  ni->clearObjectBeingIterated();

  // Reset the enumerator; it may still be in the cached iterators for
  // this thread and can be reused.
  ni->resetPropertyCursorForReuse();
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
    if (ni->isEmptyIteratorSingleton()) {
      return;
    }
    ni->unlink();
  }
}

static bool SuppressDeletedProperty(JSContext* cx, NativeIterator* ni,
                                    HandleObject obj,
                                    Handle<JSLinearString*> str) {
  if (ni->objectBeingIterated() != obj) {
    return true;
  }

  ni->disableIndices();

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
    GCPtr<JSLinearString*>* const cursor = ni->nextProperty();
    GCPtr<JSLinearString*>* const end = ni->propertiesEnd();
    for (GCPtr<JSLinearString*>* idp = cursor; idp < end; ++idp) {
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
        for (GCPtr<JSLinearString*>* p = idp; p + 1 != end; p++) {
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
  NativeIteratorListIter iter(obj->compartment()->enumeratorsAddr());
  while (!iter.done()) {
    NativeIterator* ni = iter.next();
    if (!SuppressDeletedProperty(cx, ni, obj, str)) {
      return false;
    }
  }

  return true;
}

bool js::SuppressDeletedProperty(JSContext* cx, HandleObject obj, jsid id) {
  if (MOZ_LIKELY(!obj->compartment()->objectMaybeInIteration(obj))) {
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
  if (MOZ_LIKELY(!obj->compartment()->objectMaybeInIteration(obj))) {
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

  NativeIteratorListIter iter(obj->compartment()->enumeratorsAddr());
  while (!iter.done()) {
    NativeIterator* ni = iter.next();
    if (ni->objectBeingIterated() == obj &&
        !ni->maybeHasIndexedPropertiesFromProto()) {
      for (GCPtr<JSLinearString*>* idp = ni->nextProperty();
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
  }
}
#endif

static const JSFunctionSpec iterator_methods[] = {
    JS_SELF_HOSTED_SYM_FN(iterator, "IteratorIdentity", 0, 0),
    JS_FS_END,
};

static const JSFunctionSpec iterator_static_methods[] = {
    JS_SELF_HOSTED_FN("from", "IteratorFrom", 1, 0),
    JS_FS_END,
};

// These methods are only attached to Iterator.prototype when the
// Iterator Helpers feature is enabled.
static const JSFunctionSpec iterator_methods_with_helpers[] = {
    JS_SELF_HOSTED_FN("map", "IteratorMap", 1, 0),
    JS_SELF_HOSTED_FN("filter", "IteratorFilter", 1, 0),
    JS_SELF_HOSTED_FN("take", "IteratorTake", 1, 0),
    JS_SELF_HOSTED_FN("drop", "IteratorDrop", 1, 0),
    JS_SELF_HOSTED_FN("flatMap", "IteratorFlatMap", 1, 0),
    JS_SELF_HOSTED_FN("reduce", "IteratorReduce", 1, 0),
    JS_SELF_HOSTED_FN("toArray", "IteratorToArray", 0, 0),
    JS_SELF_HOSTED_FN("forEach", "IteratorForEach", 1, 0),
    JS_SELF_HOSTED_FN("some", "IteratorSome", 1, 0),
    JS_SELF_HOSTED_FN("every", "IteratorEvery", 1, 0),
    JS_SELF_HOSTED_FN("find", "IteratorFind", 1, 0),
    JS_SELF_HOSTED_SYM_FN(iterator, "IteratorIdentity", 0, 0),
    JS_FS_END,
};

// https://tc39.es/proposal-iterator-helpers/#sec-SetterThatIgnoresPrototypeProperties
static bool SetterThatIgnoresPrototypeProperties(JSContext* cx,
                                                 Handle<Value> thisv,
                                                 Handle<PropertyKey> prop,
                                                 Handle<Value> value) {
  // Step 1.
  Rooted<JSObject*> thisObj(cx,
                            RequireObject(cx, JSMSG_OBJECT_REQUIRED, thisv));
  if (!thisObj) {
    return false;
  }

  // Step 2.
  Rooted<JSObject*> home(
      cx, GlobalObject::getOrCreateIteratorPrototype(cx, cx->global()));
  if (!home) {
    return false;
  }
  if (thisObj == home) {
    UniqueChars propName =
        IdToPrintableUTF8(cx, prop, IdToPrintableBehavior::IdIsPropertyKey);
    if (!propName) {
      return false;
    }

    // Step 2.b.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_READ_ONLY,
                              propName.get());
    return false;
  }

  // Step 3.
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!GetOwnPropertyDescriptor(cx, thisObj, prop, &desc)) {
    return false;
  }

  // Step 4.
  if (desc.isNothing()) {
    // Step 4.a.
    return DefineDataProperty(cx, thisObj, prop, value, JSPROP_ENUMERATE);
  }

  // Step 5.
  return SetProperty(cx, thisObj, prop, value);
}

// https://tc39.es/proposal-iterator-helpers/#sec-get-iteratorprototype-@@tostringtag
static bool toStringTagGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  args.rval().setString(cx->names().Iterator);
  return true;
}

// https://tc39.es/proposal-iterator-helpers/#sec-set-iteratorprototype-@@tostringtag
static bool toStringTagSetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<PropertyKey> prop(
      cx, PropertyKey::Symbol(cx->wellKnownSymbols().toStringTag));
  if (!SetterThatIgnoresPrototypeProperties(cx, args.thisv(), prop,
                                            args.get(0))) {
    return false;
  }

  // Step 2.
  args.rval().setUndefined();
  return true;
}

// https://tc39.es/proposal-iterator-helpers/#sec-get-iteratorprototype-constructor
static bool constructorGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<JSObject*> constructor(
      cx, GlobalObject::getOrCreateConstructor(cx, JSProto_Iterator));
  if (!constructor) {
    return false;
  }
  args.rval().setObject(*constructor);
  return true;
}

// https://tc39.es/proposal-iterator-helpers/#sec-set-iteratorprototype-constructor
static bool constructorSetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<PropertyKey> prop(cx, NameToId(cx->names().constructor));
  if (!SetterThatIgnoresPrototypeProperties(cx, args.thisv(), prop,
                                            args.get(0))) {
    return false;
  }

  // Step 2.
  args.rval().setUndefined();
  return true;
}

static const JSPropertySpec iterator_properties[] = {
    // NOTE: Contrary to most other @@toStringTag properties, this property
    // has a special setter (and a getter).
    JS_SYM_GETSET(toStringTag, toStringTagGetter, toStringTagSetter, 0),
    JS_PS_END,
};

/* static */
bool GlobalObject::initIteratorProto(JSContext* cx,
                                     Handle<GlobalObject*> global) {
  if (global->hasBuiltinProto(ProtoKind::IteratorProto)) {
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
  global->initBuiltinProto(ProtoKind::IteratorProto, proto);

  if (!DefinePropertiesAndFunctions(cx, proto, nullptr, iterator_methods)) {
    // In this case, we leave a partially initialized object in the
    // slot. There's no obvious way to do better, since this object may already
    // be in the prototype chain of %GeneratorPrototype%.
    return false;
  }

  return true;
}

/* static */
template <GlobalObject::ProtoKind Kind, const JSClass* ProtoClass,
          const JSFunctionSpec* Methods, const bool needsFuseProperty>
bool GlobalObject::initObjectIteratorProto(JSContext* cx,
                                           Handle<GlobalObject*> global,
                                           Handle<JSAtom*> tag) {
  if (global->hasBuiltinProto(Kind)) {
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

  if constexpr (needsFuseProperty) {
    if (!JSObject::setHasFuseProperty(cx, proto)) {
      return false;
    }
  }

  global->initBuiltinProto(Kind, proto);
  return true;
}

/* static */
NativeObject* GlobalObject::getOrCreateArrayIteratorPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  return MaybeNativeObject(getOrCreateBuiltinProto(
      cx, global, ProtoKind::ArrayIteratorProto,
      cx->names().Array_Iterator_.toHandle(),
      initObjectIteratorProto<
          ProtoKind::ArrayIteratorProto, &ArrayIteratorPrototypeClass,
          array_iterator_methods, /* hasFuseProperty= */ true>));
}

/* static */
JSObject* GlobalObject::getOrCreateStringIteratorPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  return getOrCreateBuiltinProto(
      cx, global, ProtoKind::StringIteratorProto,
      cx->names().String_Iterator_.toHandle(),
      initObjectIteratorProto<ProtoKind::StringIteratorProto,
                              &StringIteratorPrototypeClass,
                              string_iterator_methods>);
}

/* static */
JSObject* GlobalObject::getOrCreateRegExpStringIteratorPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  return getOrCreateBuiltinProto(
      cx, global, ProtoKind::RegExpStringIteratorProto,
      cx->names().RegExp_String_Iterator_.toHandle(),
      initObjectIteratorProto<ProtoKind::RegExpStringIteratorProto,
                              &RegExpStringIteratorPrototypeClass,
                              regexp_string_iterator_methods>);
}

// Iterator Helper Proposal 2.1.3.1 Iterator()
// https://tc39.es/proposal-iterator-helpers/#sec-iterator as of revision
// ed6e15a
static bool IteratorConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Iterator")) {
    return false;
  }
  // Throw TypeError if NewTarget is the active function object, preventing the
  // Iterator constructor from being used directly.
  if (args.callee() == args.newTarget().toObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BOGUS_CONSTRUCTOR, "Iterator");
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
    iterator_properties,
    IteratorObject::finishInit,
};

const JSClass IteratorObject::class_ = {
    "Iterator",
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

/* static */ bool IteratorObject::finishInit(JSContext* cx, HandleObject ctor,
                                             HandleObject proto) {
  Rooted<PropertyKey> id(cx, NameToId(cx->names().constructor));
  return JS_DefinePropertyById(cx, proto, id, constructorGetter,
                               constructorSetter, 0);
}

// Set up WrapForValidIteratorObject class and its prototype.
static const JSFunctionSpec wrap_for_valid_iterator_methods[] = {
    JS_SELF_HOSTED_FN("next", "WrapForValidIteratorNext", 0, 0),
    JS_SELF_HOSTED_FN("return", "WrapForValidIteratorReturn", 0, 0),
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
  return MaybeNativeObject(getOrCreateBuiltinProto(
      cx, global, ProtoKind::WrapForValidIteratorProto,
      Handle<JSAtom*>(nullptr),
      initObjectIteratorProto<ProtoKind::WrapForValidIteratorProto,
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
    JS_SELF_HOSTED_FN("next", "IteratorHelperNext", 0, 0),
    JS_SELF_HOSTED_FN("return", "IteratorHelperReturn", 0, 0),
    JS_FS_END,
};

static const JSClass IteratorHelperPrototypeClass = {"Iterator Helper", 0};

const JSClass IteratorHelperObject::class_ = {
    "Iterator Helper",
    JSCLASS_HAS_RESERVED_SLOTS(IteratorHelperObject::SlotCount),
};

/* static */
NativeObject* GlobalObject::getOrCreateIteratorHelperPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  return MaybeNativeObject(getOrCreateBuiltinProto(
      cx, global, ProtoKind::IteratorHelperProto,
      cx->names().Iterator_Helper_.toHandle(),
      initObjectIteratorProto<ProtoKind::IteratorHelperProto,
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

bool js::IterableToArray(JSContext* cx, HandleValue iterable,
                         MutableHandle<ArrayObject*> array) {
  JS::ForOfIterator iterator(cx);
  if (!iterator.init(iterable, JS::ForOfIterator::ThrowOnNonIterable)) {
    return false;
  }

  array.set(NewDenseEmptyArray(cx));
  if (!array) {
    return false;
  }

  RootedValue nextValue(cx);
  while (true) {
    bool done;
    if (!iterator.next(&nextValue, &done)) {
      return false;
    }
    if (done) {
      break;
    }

    if (!NewbornArrayPush(cx, array, nextValue)) {
      return false;
    }
  }
  return true;
}
