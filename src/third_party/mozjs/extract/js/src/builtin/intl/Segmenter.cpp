/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Intl.Segmenter implementation. */

#include "builtin/intl/Segmenter.h"

#include "mozilla/Assertions.h"
#include "mozilla/IntegerTypeTraits.h"
#include "mozilla/Range.h"
#include "mozilla/UniquePtr.h"

#if defined(MOZ_ICU4X)
#  include "mozilla/intl/ICU4XGeckoDataProvider.h"
#  include "ICU4XGraphemeClusterSegmenter.h"
#  include "ICU4XSentenceSegmenter.h"
#  include "ICU4XWordSegmenter.h"
#endif

#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "gc/AllocKind.h"
#include "gc/GCContext.h"
#include "js/CallArgs.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/StableStringChars.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "util/Unicode.h"
#include "vm/ArrayObject.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"
#include "vm/WellKnownAtom.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

const JSClassOps SegmenterObject::classOps_ = {
    nullptr,                    // addProperty
    nullptr,                    // delProperty
    nullptr,                    // enumerate
    nullptr,                    // newEnumerate
    nullptr,                    // resolve
    nullptr,                    // mayResolve
    SegmenterObject::finalize,  // finalize
    nullptr,                    // call
    nullptr,                    // construct
    nullptr,                    // trace
};

const JSClass SegmenterObject::class_ = {
    "Intl.Segmenter",
    JSCLASS_HAS_RESERVED_SLOTS(SegmenterObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Segmenter) |
        JSCLASS_FOREGROUND_FINALIZE,
    &SegmenterObject::classOps_,
    &SegmenterObject::classSpec_,
};

const JSClass& SegmenterObject::protoClass_ = PlainObject::class_;

static bool segmenter_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().Segmenter);
  return true;
}

static const JSFunctionSpec segmenter_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf", "Intl_Segmenter_supportedLocalesOf",
                      1, 0),
    JS_FS_END,
};

static const JSFunctionSpec segmenter_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_Segmenter_resolvedOptions", 0,
                      0),
    JS_SELF_HOSTED_FN("segment", "Intl_Segmenter_segment", 1, 0),
    JS_FN("toSource", segmenter_toSource, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec segmenter_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Intl.Segmenter", JSPROP_READONLY),
    JS_PS_END,
};

static bool Segmenter(JSContext* cx, unsigned argc, Value* vp);

const ClassSpec SegmenterObject::classSpec_ = {
    GenericCreateConstructor<Segmenter, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<SegmenterObject>,
    segmenter_static_methods,
    nullptr,
    segmenter_methods,
    segmenter_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

/**
 * Intl.Segmenter ([ locales [ , options ]])
 */
static bool Segmenter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Intl.Segmenter")) {
    return false;
  }

  // Steps 2-3 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Segmenter,
                                          &proto)) {
    return false;
  }

  Rooted<SegmenterObject*> segmenter(cx);
  segmenter = NewObjectWithClassProto<SegmenterObject>(cx, proto);
  if (!segmenter) {
    return false;
  }

  HandleValue locales = args.get(0);
  HandleValue options = args.get(1);

  // Steps 4-13.
  if (!intl::InitializeObject(cx, segmenter, cx->names().InitializeSegmenter,
                              locales, options)) {
    return false;
  }

  // Step 14.
  args.rval().setObject(*segmenter);
  return true;
}

const JSClassOps SegmentsObject::classOps_ = {
    nullptr,                   // addProperty
    nullptr,                   // delProperty
    nullptr,                   // enumerate
    nullptr,                   // newEnumerate
    nullptr,                   // resolve
    nullptr,                   // mayResolve
    SegmentsObject::finalize,  // finalize
    nullptr,                   // call
    nullptr,                   // construct
    nullptr,                   // trace
};

const JSClass SegmentsObject::class_ = {
    "Intl.Segments",
    JSCLASS_HAS_RESERVED_SLOTS(SegmentsObject::SLOT_COUNT) |
        JSCLASS_FOREGROUND_FINALIZE,
    &SegmentsObject::classOps_,
};

static const JSFunctionSpec segments_methods[] = {
    JS_SELF_HOSTED_FN("containing", "Intl_Segments_containing", 1, 0),
    JS_SELF_HOSTED_SYM_FN(iterator, "Intl_Segments_iterator", 0, 0),
    JS_FS_END,
};

bool GlobalObject::initSegmentsProto(JSContext* cx,
                                     Handle<GlobalObject*> global) {
  Rooted<JSObject*> proto(
      cx, GlobalObject::createBlankPrototype<PlainObject>(cx, global));
  if (!proto) {
    return false;
  }

  if (!JS_DefineFunctions(cx, proto, segments_methods)) {
    return false;
  }

  global->initBuiltinProto(ProtoKind::SegmentsProto, proto);
  return true;
}

const JSClassOps SegmentIteratorObject::classOps_ = {
    nullptr,                          // addProperty
    nullptr,                          // delProperty
    nullptr,                          // enumerate
    nullptr,                          // newEnumerate
    nullptr,                          // resolve
    nullptr,                          // mayResolve
    SegmentIteratorObject::finalize,  // finalize
    nullptr,                          // call
    nullptr,                          // construct
    nullptr,                          // trace
};

const JSClass SegmentIteratorObject::class_ = {
    "Intl.SegmentIterator",
    JSCLASS_HAS_RESERVED_SLOTS(SegmentIteratorObject::SLOT_COUNT) |
        JSCLASS_FOREGROUND_FINALIZE,
    &SegmentIteratorObject::classOps_,
};

static const JSFunctionSpec segment_iterator_methods[] = {
    JS_SELF_HOSTED_FN("next", "Intl_SegmentIterator_next", 0, 0),
    JS_FS_END,
};

static const JSPropertySpec segment_iterator_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Segmenter String Iterator", JSPROP_READONLY),
    JS_PS_END,
};

bool GlobalObject::initSegmentIteratorProto(JSContext* cx,
                                            Handle<GlobalObject*> global) {
  Rooted<JSObject*> iteratorProto(
      cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
  if (!iteratorProto) {
    return false;
  }

  Rooted<JSObject*> proto(
      cx, GlobalObject::createBlankPrototypeInheriting<PlainObject>(
              cx, iteratorProto));
  if (!proto) {
    return false;
  }

  if (!JS_DefineFunctions(cx, proto, segment_iterator_methods)) {
    return false;
  }

  if (!JS_DefineProperties(cx, proto, segment_iterator_properties)) {
    return false;
  }

  global->initBuiltinProto(ProtoKind::SegmentIteratorProto, proto);
  return true;
}

struct Boundaries {
  // Start index of this segmentation boundary.
  int32_t startIndex = 0;

  // End index of this segmentation boundary.
  int32_t endIndex = 0;

  // |true| if the segment is word-like. (Only used for word segmentation.)
  bool isWordLike = false;
};

/**
 * Find the segmentation boundary for the string character whose position is
 * |index|. The end position of the last segment boundary is |previousIndex|.
 */
template <class T>
static Boundaries FindBoundaryFrom(const T& iter, int32_t previousIndex,
                                   int32_t index) {
  MOZ_ASSERT(previousIndex <= index,
             "previous index must not exceed the search index");

  int32_t previous = previousIndex;
  while (true) {
    // Find the next possible break index.
    int32_t next = iter.next();

    // If |next| is larger than the search index, we've found our segment end
    // index.
    if (next > index) {
      return {previous, next, iter.isWordLike()};
    }

    // Otherwise store |next| as the start index of the next segment,
    previous = next;
  }
}

// TODO: Consider switching to the ICU4X C++ headers when the C++ headers
// are in better shape: https://github.com/rust-diplomat/diplomat/issues/280

template <typename Interface>
class SegmenterBreakIteratorType {
  typename Interface::BreakIterator* impl_;

 public:
  explicit SegmenterBreakIteratorType(void* impl)
      : impl_(static_cast<typename Interface::BreakIterator*>(impl)) {
    MOZ_ASSERT(impl);
  }

  int32_t next() const { return Interface::next(impl_); }

  bool isWordLike() const { return Interface::isWordLike(impl_); }
};

#if defined(MOZ_ICU4X)
// Each SegmenterBreakIterator interface contains the following definitions:
//
// - BreakIterator: Type of the ICU4X break iterator.
// - Segmenter: Type of the ICU4X segmenter.
// - Char: Character type, either `JS::Latin1Char` or `char16_t`.
// - create: Static method to create a new instance of `BreakIterator`.
// - destroy: Static method to destroy an instance of `BreakIterator`.
// - next: Static method to fetch the next break iteration index.
// - isWordLike: Static method to determine if the current segment is word-like.
//
//
// Each Segmenter interface contains the following definitions:
//
// - Segmenter: Type of the ICU4X segmenter.
// - BreakIteratorLatin1: SegmenterBreakIterator interface to Latin1 strings.
// - BreakIteratorTwoByte: SegmenterBreakIterator interface to TwoByte strings.
// - create: Static method to create a new instance of `Segmenter`.
// - destroy: Static method to destroy an instance of `Segmenter`.

struct GraphemeClusterSegmenterBreakIteratorLatin1 {
  using BreakIterator = capi::ICU4XGraphemeClusterBreakIteratorLatin1;
  using Segmenter = capi::ICU4XGraphemeClusterSegmenter;
  using Char = JS::Latin1Char;

  static constexpr auto& create =
      capi::ICU4XGraphemeClusterSegmenter_segment_latin1;
  static constexpr auto& destroy =
      capi::ICU4XGraphemeClusterBreakIteratorLatin1_destroy;
  static constexpr auto& next =
      capi::ICU4XGraphemeClusterBreakIteratorLatin1_next;

  static bool isWordLike(const BreakIterator*) { return false; }
};

struct GraphemeClusterSegmenterBreakIteratorTwoByte {
  using BreakIterator = capi::ICU4XGraphemeClusterBreakIteratorUtf16;
  using Segmenter = capi::ICU4XGraphemeClusterSegmenter;
  using Char = char16_t;

  static constexpr auto& create =
      capi::ICU4XGraphemeClusterSegmenter_segment_utf16;
  static constexpr auto& destroy =
      capi::ICU4XGraphemeClusterBreakIteratorUtf16_destroy;
  static constexpr auto& next =
      capi::ICU4XGraphemeClusterBreakIteratorUtf16_next;

  static bool isWordLike(const BreakIterator*) { return false; }
};

struct GraphemeClusterSegmenter {
  using Segmenter = capi::ICU4XGraphemeClusterSegmenter;
  using BreakIteratorLatin1 =
      SegmenterBreakIteratorType<GraphemeClusterSegmenterBreakIteratorLatin1>;
  using BreakIteratorTwoByte =
      SegmenterBreakIteratorType<GraphemeClusterSegmenterBreakIteratorTwoByte>;

  static constexpr auto& create = capi::ICU4XGraphemeClusterSegmenter_create;
  static constexpr auto& destroy = capi::ICU4XGraphemeClusterSegmenter_destroy;
};

struct WordSegmenterBreakIteratorLatin1 {
  using BreakIterator = capi::ICU4XWordBreakIteratorLatin1;
  using Segmenter = capi::ICU4XWordSegmenter;
  using Char = JS::Latin1Char;

  static constexpr auto& create = capi::ICU4XWordSegmenter_segment_latin1;
  static constexpr auto& destroy = capi::ICU4XWordBreakIteratorLatin1_destroy;
  static constexpr auto& next = capi::ICU4XWordBreakIteratorLatin1_next;
  static constexpr auto& isWordLike =
      capi::ICU4XWordBreakIteratorLatin1_is_word_like;
};

struct WordSegmenterBreakIteratorTwoByte {
  using BreakIterator = capi::ICU4XWordBreakIteratorUtf16;
  using Segmenter = capi::ICU4XWordSegmenter;
  using Char = char16_t;

  static constexpr auto& create = capi::ICU4XWordSegmenter_segment_utf16;
  static constexpr auto& destroy = capi::ICU4XWordBreakIteratorUtf16_destroy;
  static constexpr auto& next = capi::ICU4XWordBreakIteratorUtf16_next;
  static constexpr auto& isWordLike =
      capi::ICU4XWordBreakIteratorUtf16_is_word_like;
};

struct WordSegmenter {
  using Segmenter = capi::ICU4XWordSegmenter;
  using BreakIteratorLatin1 =
      SegmenterBreakIteratorType<WordSegmenterBreakIteratorLatin1>;
  using BreakIteratorTwoByte =
      SegmenterBreakIteratorType<WordSegmenterBreakIteratorTwoByte>;

  static constexpr auto& create = capi::ICU4XWordSegmenter_create_auto;
  static constexpr auto& destroy = capi::ICU4XWordSegmenter_destroy;
};

struct SentenceSegmenterBreakIteratorLatin1 {
  using BreakIterator = capi::ICU4XSentenceBreakIteratorLatin1;
  using Segmenter = capi::ICU4XSentenceSegmenter;
  using Char = JS::Latin1Char;

  static constexpr auto& create = capi::ICU4XSentenceSegmenter_segment_latin1;
  static constexpr auto& destroy =
      capi::ICU4XSentenceBreakIteratorLatin1_destroy;
  static constexpr auto& next = capi::ICU4XSentenceBreakIteratorLatin1_next;

  static bool isWordLike(const BreakIterator*) { return false; }
};

struct SentenceSegmenterBreakIteratorTwoByte {
  using BreakIterator = capi::ICU4XSentenceBreakIteratorUtf16;
  using Segmenter = capi::ICU4XSentenceSegmenter;
  using Char = char16_t;

  static constexpr auto& create = capi::ICU4XSentenceSegmenter_segment_utf16;
  static constexpr auto& destroy =
      capi::ICU4XSentenceBreakIteratorUtf16_destroy;
  static constexpr auto& next = capi::ICU4XSentenceBreakIteratorUtf16_next;

  static bool isWordLike(const BreakIterator*) { return false; }
};

struct SentenceSegmenter {
  using Segmenter = capi::ICU4XSentenceSegmenter;
  using BreakIteratorLatin1 =
      SegmenterBreakIteratorType<SentenceSegmenterBreakIteratorLatin1>;
  using BreakIteratorTwoByte =
      SegmenterBreakIteratorType<SentenceSegmenterBreakIteratorTwoByte>;

  static constexpr auto& create = capi::ICU4XSentenceSegmenter_create;
  static constexpr auto& destroy = capi::ICU4XSentenceSegmenter_destroy;
};
#endif

/**
 * Create a new ICU4X segmenter instance.
 */
template <typename Interface>
static typename Interface::Segmenter* CreateSegmenter(JSContext* cx) {
  auto result = Interface::create(mozilla::intl::GetDataProvider());
  if (!result.is_ok) {
    intl::ReportInternalError(cx);
    return nullptr;
  }
  return result.ok;
}

static bool EnsureInternalsResolved(JSContext* cx,
                                    Handle<SegmenterObject*> segmenter) {
  if (segmenter->getLocale()) {
    return true;
  }

  Rooted<JS::Value> value(cx);

  Rooted<JSObject*> internals(cx, intl::GetInternalsObject(cx, segmenter));
  if (!internals) {
    return false;
  }

  if (!GetProperty(cx, internals, internals, cx->names().locale, &value)) {
    return false;
  }
  Rooted<JSString*> locale(cx, value.toString());

  if (!GetProperty(cx, internals, internals, cx->names().granularity, &value)) {
    return false;
  }

  SegmenterGranularity granularity;
  {
    JSLinearString* linear = value.toString()->ensureLinear(cx);
    if (!linear) {
      return false;
    }

    if (StringEqualsLiteral(linear, "grapheme")) {
      granularity = SegmenterGranularity::Grapheme;
    } else if (StringEqualsLiteral(linear, "word")) {
      granularity = SegmenterGranularity::Word;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(linear, "sentence"));
      granularity = SegmenterGranularity::Sentence;
    }
  }

#if defined(MOZ_ICU4X)
  switch (granularity) {
    case SegmenterGranularity::Grapheme: {
      auto* seg = CreateSegmenter<GraphemeClusterSegmenter>(cx);
      if (!seg) {
        return false;
      }
      segmenter->setSegmenter(seg);
      break;
    }
    case SegmenterGranularity::Word: {
      auto* seg = CreateSegmenter<WordSegmenter>(cx);
      if (!seg) {
        return false;
      }
      segmenter->setSegmenter(seg);
      break;
    }
    case SegmenterGranularity::Sentence: {
      auto* seg = CreateSegmenter<SentenceSegmenter>(cx);
      if (!seg) {
        return false;
      }
      segmenter->setSegmenter(seg);
      break;
    }
  }
#endif

  segmenter->setLocale(locale);
  segmenter->setGranularity(granularity);

  return true;
}

/**
 * Destroy an ICU4X segmenter instance.
 */
template <typename Interface>
static void DestroySegmenter(void* seg) {
  auto* segmenter = static_cast<typename Interface::Segmenter*>(seg);
  Interface::destroy(segmenter);
}

void SegmenterObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());

  auto& segmenter = obj->as<SegmenterObject>();
  if (void* seg = segmenter.getSegmenter()) {
#if defined(MOZ_ICU4X)
    switch (segmenter.getGranularity()) {
      case SegmenterGranularity::Grapheme: {
        DestroySegmenter<GraphemeClusterSegmenter>(seg);
        break;
      }
      case SegmenterGranularity::Word: {
        DestroySegmenter<WordSegmenter>(seg);
        break;
      }
      case SegmenterGranularity::Sentence: {
        DestroySegmenter<SentenceSegmenter>(seg);
        break;
      }
    }
#else
    MOZ_CRASH("ICU4X disabled");
#endif
  }
}

/**
 * Destroy an ICU4X break iterator instance.
 */
template <typename Interface>
static void DestroyBreakIterator(void* brk) {
  auto* breakIterator = static_cast<typename Interface::BreakIterator*>(brk);
  Interface::destroy(breakIterator);
}

/**
 * Destroy the ICU4X break iterator attached to |segments|.
 */
template <typename T>
static void DestroyBreakIterator(const T* segments) {
#if defined(MOZ_ICU4X)
  void* brk = segments->getBreakIterator();
  MOZ_ASSERT(brk);

  bool isLatin1 = segments->hasLatin1StringChars();

  switch (segments->getGranularity()) {
    case SegmenterGranularity::Grapheme: {
      if (isLatin1) {
        DestroyBreakIterator<GraphemeClusterSegmenterBreakIteratorLatin1>(brk);
      } else {
        DestroyBreakIterator<GraphemeClusterSegmenterBreakIteratorTwoByte>(brk);
      }
      break;
    }
    case SegmenterGranularity::Word: {
      if (isLatin1) {
        DestroyBreakIterator<WordSegmenterBreakIteratorLatin1>(brk);
      } else {
        DestroyBreakIterator<WordSegmenterBreakIteratorTwoByte>(brk);
      }
      break;
    }
    case SegmenterGranularity::Sentence: {
      if (isLatin1) {
        DestroyBreakIterator<SentenceSegmenterBreakIteratorLatin1>(brk);
      } else {
        DestroyBreakIterator<SentenceSegmenterBreakIteratorTwoByte>(brk);
      }
      break;
    }
  }
#else
  MOZ_CRASH("ICU4X disabled");
#endif
}

void SegmentsObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());

  auto* segments = &obj->as<SegmentsObject>();

  if (auto chars = segments->getStringChars()) {
    size_t length = segments->getString()->length();
    if (chars.has<JS::Latin1Char>()) {
      intl::RemoveICUCellMemory(gcx, segments, length * sizeof(JS::Latin1Char));
      js_free(chars.data<JS::Latin1Char>());
    } else {
      intl::RemoveICUCellMemory(gcx, segments, length * sizeof(char16_t));
      js_free(chars.data<char16_t>());
    }
  }

  if (segments->getBreakIterator()) {
    DestroyBreakIterator(segments);
  }
}

void SegmentIteratorObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());

  auto* iterator = &obj->as<SegmentIteratorObject>();

  if (auto chars = iterator->getStringChars()) {
    size_t length = iterator->getString()->length();
    if (chars.has<JS::Latin1Char>()) {
      intl::RemoveICUCellMemory(gcx, iterator, length * sizeof(JS::Latin1Char));
      js_free(chars.data<JS::Latin1Char>());
    } else {
      intl::RemoveICUCellMemory(gcx, iterator, length * sizeof(char16_t));
      js_free(chars.data<char16_t>());
    }
  }

  if (iterator->getBreakIterator()) {
    DestroyBreakIterator(iterator);
  }
}

template <typename Iterator, typename T>
static Boundaries FindBoundaryFrom(Handle<T*> segments, int32_t index) {
  MOZ_ASSERT(0 <= index && uint32_t(index) < segments->getString()->length());

  Iterator iter(segments->getBreakIterator());
  return FindBoundaryFrom(iter, segments->getIndex(), index);
}

template <typename T>
static Boundaries GraphemeBoundaries(Handle<T*> segments, int32_t index) {
#if defined(MOZ_ICU4X)
  if (segments->hasLatin1StringChars()) {
    return FindBoundaryFrom<GraphemeClusterSegmenter::BreakIteratorLatin1>(
        segments, index);
  }
  return FindBoundaryFrom<GraphemeClusterSegmenter::BreakIteratorTwoByte>(
      segments, index);
#else
  MOZ_CRASH("ICU4X disabled");
#endif
}

template <typename T>
static Boundaries WordBoundaries(Handle<T*> segments, int32_t index) {
#if defined(MOZ_ICU4X)
  if (segments->hasLatin1StringChars()) {
    return FindBoundaryFrom<WordSegmenter::BreakIteratorLatin1>(segments,
                                                                index);
  }
  return FindBoundaryFrom<WordSegmenter::BreakIteratorTwoByte>(segments, index);
#else
  MOZ_CRASH("ICU4X disabled");
#endif
}

template <typename T>
static Boundaries SentenceBoundaries(Handle<T*> segments, int32_t index) {
#if defined(MOZ_ICU4X)
  if (segments->hasLatin1StringChars()) {
    return FindBoundaryFrom<SentenceSegmenter::BreakIteratorLatin1>(segments,
                                                                    index);
  }
  return FindBoundaryFrom<SentenceSegmenter::BreakIteratorTwoByte>(segments,
                                                                   index);
#else
  MOZ_CRASH("ICU4X disabled");
#endif
}

/**
 * Ensure the string characters have been copied into |segments| in preparation
 * for passing the string characters to ICU4X.
 */
template <typename T>
static bool EnsureStringChars(JSContext* cx, Handle<T*> segments) {
  if (segments->hasStringChars()) {
    return true;
  }

  Rooted<JSLinearString*> string(cx, segments->getString()->ensureLinear(cx));
  if (!string) {
    return false;
  }

  size_t length = string->length();

  JS::AutoCheckCannotGC nogc;
  if (string->hasLatin1Chars()) {
    auto chars = DuplicateString(cx, string->latin1Chars(nogc), length);
    if (!chars) {
      return false;
    }
    segments->setStringChars(SegmentsStringChars{chars.release()});

    intl::AddICUCellMemory(segments, length * sizeof(JS::Latin1Char));
  } else {
    auto chars = DuplicateString(cx, string->twoByteChars(nogc), length);
    if (!chars) {
      return false;
    }
    segments->setStringChars(SegmentsStringChars{chars.release()});

    intl::AddICUCellMemory(segments, length * sizeof(char16_t));
  }
  return true;
}

/**
 * Create a new ICU4X break iterator instance.
 */
template <typename Interface, typename T>
static auto* CreateBreakIterator(Handle<T*> segments) {
  void* segmenter = segments->getSegmenter()->getSegmenter();
  MOZ_ASSERT(segmenter);

  auto chars = segments->getStringChars();
  MOZ_ASSERT(chars);

  size_t length = segments->getString()->length();

  using Unsigned = typename mozilla::UnsignedStdintTypeForSize<sizeof(
      typename Interface::Char)>::Type;

  auto* seg = static_cast<const typename Interface::Segmenter*>(segmenter);
  auto* ch = chars.template data<typename Interface::Char>();
  auto* chUnsigned = reinterpret_cast<Unsigned*>(ch);
  return Interface::create(seg, chUnsigned, length);
}

/**
 * Ensure |segments| has a break iterator whose current segment index is at most
 * |index|.
 */
template <typename T>
static bool EnsureBreakIterator(JSContext* cx, Handle<T*> segments,
                                int32_t index) {
  if (segments->getBreakIterator()) {
    // Reuse the break iterator if its current segment index is at most |index|.
    if (index >= segments->getIndex()) {
      return true;
    }

    // Reverse iteration not supported. Destroy the previous break iterator and
    // start from fresh.
    DestroyBreakIterator(segments.get());

    // Reset internal state.
    segments->setBreakIterator(nullptr);
    segments->setIndex(0);
  }

  // Ensure the string characters can be passed to ICU4X.
  if (!EnsureStringChars(cx, segments)) {
    return false;
  }

#if defined(MOZ_ICU4X)
  bool isLatin1 = segments->hasLatin1StringChars();

  // Create a new break iterator based on the granularity and character type.
  void* brk;
  switch (segments->getGranularity()) {
    case SegmenterGranularity::Grapheme: {
      if (isLatin1) {
        brk = CreateBreakIterator<GraphemeClusterSegmenterBreakIteratorLatin1>(
            segments);
      } else {
        brk = CreateBreakIterator<GraphemeClusterSegmenterBreakIteratorTwoByte>(
            segments);
      }
      break;
    }
    case SegmenterGranularity::Word: {
      if (isLatin1) {
        brk = CreateBreakIterator<WordSegmenterBreakIteratorLatin1>(segments);
      } else {
        brk = CreateBreakIterator<WordSegmenterBreakIteratorTwoByte>(segments);
      }
      break;
    }
    case SegmenterGranularity::Sentence: {
      if (isLatin1) {
        brk =
            CreateBreakIterator<SentenceSegmenterBreakIteratorLatin1>(segments);
      } else {
        brk = CreateBreakIterator<SentenceSegmenterBreakIteratorTwoByte>(
            segments);
      }
      break;
    }
  }

  MOZ_RELEASE_ASSERT(brk);
  segments->setBreakIterator(brk);

  MOZ_ASSERT(segments->getIndex() == 0, "index is initially zero");

  return true;
#else
  MOZ_CRASH("ICU4X disabled");
#endif
}

/**
 * Create the boundaries result array for self-hosted code.
 */
static ArrayObject* CreateBoundaries(JSContext* cx, Boundaries boundaries,
                                     SegmenterGranularity granularity) {
  auto [startIndex, endIndex, isWordLike] = boundaries;

  auto* result = NewDenseFullyAllocatedArray(cx, 3);
  if (!result) {
    return nullptr;
  }
  result->setDenseInitializedLength(3);
  result->initDenseElement(0, Int32Value(startIndex));
  result->initDenseElement(1, Int32Value(endIndex));
  if (granularity == SegmenterGranularity::Word) {
    result->initDenseElement(2, BooleanValue(isWordLike));
  } else {
    result->initDenseElement(2, UndefinedValue());
  }
  return result;
}

template <typename T>
static ArrayObject* FindSegmentBoundaries(JSContext* cx, Handle<T*> segments,
                                          int32_t index) {
  // Ensure break iteration can start at |index|.
  if (!EnsureBreakIterator(cx, segments, index)) {
    return nullptr;
  }

  // Find the actual segment boundaries.
  Boundaries boundaries{};
  switch (segments->getGranularity()) {
    case SegmenterGranularity::Grapheme: {
      boundaries = GraphemeBoundaries(segments, index);
      break;
    }
    case SegmenterGranularity::Word: {
      boundaries = WordBoundaries(segments, index);
      break;
    }
    case SegmenterGranularity::Sentence: {
      boundaries = SentenceBoundaries(segments, index);
      break;
    }
  }

  // Remember the end index of the current boundary segment.
  segments->setIndex(boundaries.endIndex);

  return CreateBoundaries(cx, boundaries, segments->getGranularity());
}

bool js::intl_CreateSegmentsObject(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  Rooted<SegmenterObject*> segmenter(cx,
                                     &args[0].toObject().as<SegmenterObject>());
  Rooted<JSString*> string(cx, args[1].toString());

  // Ensure the internal properties are resolved.
  if (!EnsureInternalsResolved(cx, segmenter)) {
    return false;
  }

  Rooted<JSObject*> proto(
      cx, GlobalObject::getOrCreateSegmentsPrototype(cx, cx->global()));
  if (!proto) {
    return false;
  }

  auto* segments = NewObjectWithGivenProto<SegmentsObject>(cx, proto);
  if (!segments) {
    return false;
  }

  segments->setSegmenter(segmenter);
  segments->setGranularity(segmenter->getGranularity());
  segments->setString(string);
  segments->setIndex(0);

  args.rval().setObject(*segments);
  return true;
}

bool js::intl_CreateSegmentIterator(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  Rooted<SegmentsObject*> segments(cx,
                                   &args[0].toObject().as<SegmentsObject>());

  Rooted<JSObject*> proto(
      cx, GlobalObject::getOrCreateSegmentIteratorPrototype(cx, cx->global()));
  if (!proto) {
    return false;
  }

  auto* iterator = NewObjectWithGivenProto<SegmentIteratorObject>(cx, proto);
  if (!iterator) {
    return false;
  }

  iterator->setSegmenter(segments->getSegmenter());
  iterator->setGranularity(segments->getGranularity());
  iterator->setString(segments->getString());
  iterator->setIndex(0);

  args.rval().setObject(*iterator);
  return true;
}

bool js::intl_FindSegmentBoundaries(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  Rooted<SegmentsObject*> segments(cx,
                                   &args[0].toObject().as<SegmentsObject>());

  int32_t index = args[1].toInt32();
  MOZ_ASSERT(index >= 0);
  MOZ_ASSERT(uint32_t(index) < segments->getString()->length());

  auto* result = FindSegmentBoundaries(
      cx, static_cast<Handle<SegmentsObject*>>(segments), index);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

bool js::intl_FindNextSegmentBoundaries(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  Rooted<SegmentIteratorObject*> iterator(
      cx, &args[0].toObject().as<SegmentIteratorObject>());

  int32_t index = iterator->getIndex();
  MOZ_ASSERT(index >= 0);
  MOZ_ASSERT(uint32_t(index) < iterator->getString()->length());

  auto* result = FindSegmentBoundaries(
      cx, static_cast<Handle<SegmentIteratorObject*>>(iterator), index);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}
