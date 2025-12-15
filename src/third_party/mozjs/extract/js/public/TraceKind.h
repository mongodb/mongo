/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_TraceKind_h
#define js_TraceKind_h

#include "mozilla/UniquePtr.h"

#include "js/TypeDecls.h"

// Forward declarations of all the types a TraceKind can denote.
class JSLinearString;

namespace js {
class BaseScript;
class BaseShape;
class GetterSetter;
class PropMap;
class RegExpShared;
class Shape;
class Scope;
namespace gc {
class SmallBuffer;
}  // namespace gc
namespace jit {
class JitCode;
}  // namespace jit
}  // namespace js

namespace JS {

// When tracing a thing, the GC needs to know about the layout of the object it
// is looking at. There are a fixed number of different layouts that the GC
// knows about. The "trace kind" is a static map which tells which layout a GC
// thing has.
//
// Although this map is public, the details are completely hidden. Not all of
// the matching C++ types are exposed, and those that are, are opaque.
//
// See Value::gcKind() and JSTraceCallback in Tracer.h for more details.
enum class TraceKind {
  // These trace kinds have a publicly exposed, although opaque, C++ type.
  // Note: The order here is determined by our Value packing. Other users
  //       should sort alphabetically, for consistency.
  // Note: Nursery allocatable kinds go first. See js::gc::NurseryTraceKinds.
  Object = 0x00,
  BigInt = 0x01,
  String = 0x02,
  Symbol = 0x03,

  // Shape details are exposed through JS_TraceShapeCycleCollectorChildren.
  Shape = 0x04,

  BaseShape = 0x05,

  // The kind associated with a nullptr.
  Null = 0x06,

  // The following kinds do not have an exposed C++ idiom.
  JitCode,
  Script,
  Scope,
  RegExpShared,
  GetterSetter,
  PropMap,
  SmallBuffer
};

// GCCellPtr packs the trace kind into the low bits of the pointer for common
// kinds.
const static uintptr_t OutOfLineTraceKindMask = 0x07;
static_assert(uintptr_t(JS::TraceKind::Null) < OutOfLineTraceKindMask,
              "GCCellPtr requires an inline representation for nullptr");

// When this header is imported inside SpiderMonkey, the class definitions are
// available and we can query those definitions to find the correct kind
// directly from the class hierarchy.
template <typename T>
struct MapTypeToTraceKind {
  static const JS::TraceKind kind = T::TraceKind;
};

// When this header is used outside SpiderMonkey, the class definitions are not
// available, so the following table containing all public GC types is used.
//
// canBeGray: GC can mark things of this kind gray. The cycle collector
//            traverses gray GC things when looking for cycles.
// inCCGraph: Things of this kind are represented as nodes in the CC graph. This
//            also means they can be used as a keys in WeakMap.

// clang-format off
#define JS_FOR_EACH_TRACEKIND(D)                               \
  /* name         type                 canBeGray  inCCGraph */ \
  D(BaseShape,    js::BaseShape,       true,      false)       \
  D(JitCode,      js::jit::JitCode,    true,      false)       \
  D(Scope,        js::Scope,           true,      true)        \
  D(Object,       JSObject,            true,      true)        \
  D(Script,       js::BaseScript,      true,      true)        \
  D(Shape,        js::Shape,           true,      false)       \
  D(String,       JSString,            false,     false)       \
  D(Symbol,       JS::Symbol,          false,     false)       \
  D(BigInt,       JS::BigInt,          false,     false)       \
  D(RegExpShared, js::RegExpShared,    true,      true)        \
  D(GetterSetter, js::GetterSetter,    true,      true)        \
  D(PropMap,      js::PropMap,         false,     false)       \
  D(SmallBuffer,  js::gc::SmallBuffer, false,     false)
// clang-format on

// Returns true if the JS::TraceKind is represented as a node in cycle collector
// graph.
inline constexpr bool IsCCTraceKind(JS::TraceKind aKind) {
  switch (aKind) {
#define JS_EXPAND_DEF(name, _1, _2, inCCGraph) \
  case JS::TraceKind::name:                    \
    return inCCGraph;
    JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF
    default:
      return false;
  }
}

// Helper for SFINAE to ensure certain methods are only used on appropriate base
// types. This avoids common footguns such as `Cell::is<JSFunction>()` which
// match any type of JSObject.
template <typename T>
struct IsBaseTraceType : std::false_type {};

#define JS_EXPAND_DEF(_, type, _1, _2) \
  template <>                          \
  struct IsBaseTraceType<type> : std::true_type {};
JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF

template <typename T>
inline constexpr bool IsBaseTraceType_v = IsBaseTraceType<T>::value;

// Map from all public types to their trace kind.
#define JS_EXPAND_DEF(name, type, _, _1)                   \
  template <>                                              \
  struct MapTypeToTraceKind<type> {                        \
    static const JS::TraceKind kind = JS::TraceKind::name; \
  };
JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF

template <>
struct MapTypeToTraceKind<JSLinearString> {
  static const JS::TraceKind kind = JS::TraceKind::String;
};
template <>
struct MapTypeToTraceKind<JSFunction> {
  static const JS::TraceKind kind = JS::TraceKind::Object;
};
template <>
struct MapTypeToTraceKind<JSScript> {
  static const JS::TraceKind kind = JS::TraceKind::Script;
};

// RootKind is closely related to TraceKind. Whereas TraceKind's indices are
// laid out for convenient embedding as a pointer tag, the indicies of RootKind
// are designed for use as array keys via EnumeratedArray.
enum class RootKind : int8_t {
// These map 1:1 with trace kinds.
#define EXPAND_ROOT_KIND(name, _0, _1, _2) name,
  JS_FOR_EACH_TRACEKIND(EXPAND_ROOT_KIND)
#undef EXPAND_ROOT_KIND

  // These tagged pointers are special-cased for performance.
  Id,
  Value,

  // Everything else.
  Traceable,

  Limit
};

// Most RootKind correspond directly to a trace kind.
template <TraceKind traceKind>
struct MapTraceKindToRootKind {};
#define JS_EXPAND_DEF(name, _0, _1, _2)                  \
  template <>                                            \
  struct MapTraceKindToRootKind<JS::TraceKind::name> {   \
    static const JS::RootKind kind = JS::RootKind::name; \
  };
JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF)
#undef JS_EXPAND_DEF

// Specify the RootKind for all types. Value and jsid map to special cases;
// Cell pointer types we can derive directly from the TraceKind; everything else
// should go in the Traceable list and use GCPolicy<T>::trace for tracing.
template <typename T>
struct MapTypeToRootKind {
  static const JS::RootKind kind = JS::RootKind::Traceable;
};
template <typename T>
struct MapTypeToRootKind<T*> {
  static const JS::RootKind kind =
      JS::MapTraceKindToRootKind<JS::MapTypeToTraceKind<T>::kind>::kind;
};
template <>
struct MapTypeToRootKind<JS::Realm*> {
  // Not a pointer to a GC cell. Use GCPolicy.
  static const JS::RootKind kind = JS::RootKind::Traceable;
};
template <typename T>
struct MapTypeToRootKind<mozilla::UniquePtr<T>> {
  static const JS::RootKind kind = JS::MapTypeToRootKind<T>::kind;
};
template <>
struct MapTypeToRootKind<JS::Value> {
  static const JS::RootKind kind = JS::RootKind::Value;
};
template <>
struct MapTypeToRootKind<jsid> {
  static const JS::RootKind kind = JS::RootKind::Id;
};

// Fortunately, few places in the system need to deal with fully abstract
// cells. In those places that do, we generally want to move to a layout
// templated function as soon as possible. This template wraps the upcast
// for that dispatch.
//
// Given a call:
//
//    DispatchTraceKindTyped(f, thing, traceKind, ... args)
//
// Downcast the |void *thing| to the specific type designated by |traceKind|,
// and pass it to the functor |f| along with |... args|, forwarded. Pass the
// type designated by |traceKind| as the functor's template argument. The
// |thing| parameter is optional; without it, we simply pass through |... args|.
template <typename F, typename... Args>
auto DispatchTraceKindTyped(F f, JS::TraceKind traceKind, Args&&... args) {
  switch (traceKind) {
#define JS_EXPAND_DEF(name, type, _, _1) \
  case JS::TraceKind::name:              \
    return f.template operator()<type>(std::forward<Args>(args)...);
    JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF
    default:
      MOZ_CRASH("Invalid trace kind in DispatchTraceKindTyped.");
  }
}

// Given a GC thing specified by pointer and trace kind, calls the functor |f|
// with a template argument of the actual type of the pointer and returns the
// result.
template <typename F>
auto MapGCThingTyped(void* thing, JS::TraceKind traceKind, F&& f) {
  switch (traceKind) {
#define JS_EXPAND_DEF(name, type, _, _1) \
  case JS::TraceKind::name:              \
    return f(static_cast<type*>(thing));
    JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF
    default:
      MOZ_CRASH("Invalid trace kind in MapGCThingTyped.");
  }
}

// Given a GC thing specified by pointer and trace kind, calls the functor |f|
// with a template argument of the actual type of the pointer and ignores the
// result.
template <typename F>
void ApplyGCThingTyped(void* thing, JS::TraceKind traceKind, F&& f) {
  // This function doesn't do anything but is supplied for symmetry with other
  // MapGCThingTyped/ApplyGCThingTyped implementations that have to wrap the
  // functor to return a dummy value that is ignored.
  MapGCThingTyped(thing, traceKind, std::move(f));
}

}  // namespace JS

#endif  // js_TraceKind_h
