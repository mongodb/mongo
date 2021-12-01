/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_TraceKind_h
#define js_TraceKind_h

#include "mozilla/UniquePtr.h"

#include "js/TypeDecls.h"

// Forward declarations of all the types a TraceKind can denote.
namespace js {
class BaseShape;
class LazyScript;
class ObjectGroup;
class RegExpShared;
class Shape;
class Scope;
namespace jit {
class JitCode;
} // namespace jit
} // namespace js

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
enum class TraceKind
{
    // These trace kinds have a publicly exposed, although opaque, C++ type.
    // Note: The order here is determined by our Value packing. Other users
    //       should sort alphabetically, for consistency.
    Object = 0x00,
    String = 0x02,
    Symbol = 0x03,

    // 0x1 is not used for any GCThing Value tag, so we use it for Script.
    Script = 0x01,

    // Shape details are exposed through JS_TraceShapeCycleCollectorChildren.
    Shape = 0x04,

    // ObjectGroup details are exposed through JS_TraceObjectGroupCycleCollectorChildren.
    ObjectGroup = 0x05,

    // The kind associated with a nullptr.
    Null = 0x06,

    // The following kinds do not have an exposed C++ idiom.
    BaseShape = 0x0F,
    JitCode = 0x1F,
    LazyScript = 0x2F,
    Scope = 0x3F,
    RegExpShared = 0x4F
};
const static uintptr_t OutOfLineTraceKindMask = 0x07;

#define ASSERT_TRACE_KIND(tk) \
    static_assert((uintptr_t(tk) & OutOfLineTraceKindMask) == OutOfLineTraceKindMask, \
        "mask bits are set")
ASSERT_TRACE_KIND(JS::TraceKind::BaseShape);
ASSERT_TRACE_KIND(JS::TraceKind::JitCode);
ASSERT_TRACE_KIND(JS::TraceKind::LazyScript);
ASSERT_TRACE_KIND(JS::TraceKind::Scope);
ASSERT_TRACE_KIND(JS::TraceKind::RegExpShared);
#undef ASSERT_TRACE_KIND

// When this header is imported inside SpiderMonkey, the class definitions are
// available and we can query those definitions to find the correct kind
// directly from the class hierarchy.
template <typename T>
struct MapTypeToTraceKind {
    static const JS::TraceKind kind = T::TraceKind;
};

// When this header is used outside SpiderMonkey, the class definitions are not
// available, so the following table containing all public GC types is used.
#define JS_FOR_EACH_TRACEKIND(D) \
 /* PrettyName       TypeName           AddToCCKind */ \
    D(BaseShape,     js::BaseShape,     true) \
    D(JitCode,       js::jit::JitCode,  true) \
    D(LazyScript,    js::LazyScript,    true) \
    D(Scope,         js::Scope,         true) \
    D(Object,        JSObject,          true) \
    D(ObjectGroup,   js::ObjectGroup,   true) \
    D(Script,        JSScript,          true) \
    D(Shape,         js::Shape,         true) \
    D(String,        JSString,          false) \
    D(Symbol,        JS::Symbol,        false) \
    D(RegExpShared,  js::RegExpShared,  true)

// Map from all public types to their trace kind.
#define JS_EXPAND_DEF(name, type, _) \
    template <> struct MapTypeToTraceKind<type> { \
        static const JS::TraceKind kind = JS::TraceKind::name; \
    };
JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF

// RootKind is closely related to TraceKind. Whereas TraceKind's indices are
// laid out for convenient embedding as a pointer tag, the indicies of RootKind
// are designed for use as array keys via EnumeratedArray.
enum class RootKind : int8_t
{
    // These map 1:1 with trace kinds.
#define EXPAND_ROOT_KIND(name, _0, _1) \
    name,
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
template <TraceKind traceKind> struct MapTraceKindToRootKind {};
#define JS_EXPAND_DEF(name, _0, _1) \
    template <> struct MapTraceKindToRootKind<JS::TraceKind::name> { \
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
template <> struct MapTypeToRootKind<JS::Realm*> {
    // Not a pointer to a GC cell. Use GCPolicy.
    static const JS::RootKind kind = JS::RootKind::Traceable;
};
template <typename T>
struct MapTypeToRootKind<mozilla::UniquePtr<T>> {
    static const JS::RootKind kind = JS::MapTypeToRootKind<T>::kind;
};
template <> struct MapTypeToRootKind<JS::Value> {
    static const JS::RootKind kind = JS::RootKind::Value;
};
template <> struct MapTypeToRootKind<jsid> {
    static const JS::RootKind kind = JS::RootKind::Id;
};
template <> struct MapTypeToRootKind<JSFunction*> : public MapTypeToRootKind<JSObject*> {};

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

// VS2017+, GCC and Clang require an explicit template declaration in front of
// the specialization of operator() because it is a dependent template. VS2015,
// on the other hand, gets very confused if we have a |template| token there.
// The clang-cl front end defines _MSC_VER, but still requires the explicit
// template declaration, so we must test for __clang__ here as well.
#if (defined(_MSC_VER) && _MSC_VER < 1910) && !defined(__clang__)
# define JS_DEPENDENT_TEMPLATE_HINT
#else
# define JS_DEPENDENT_TEMPLATE_HINT template
#endif
template <typename F, typename... Args>
auto
DispatchTraceKindTyped(F f, JS::TraceKind traceKind, Args&&... args)
  -> decltype(f. JS_DEPENDENT_TEMPLATE_HINT operator()<JSObject>(mozilla::Forward<Args>(args)...))
{
    switch (traceKind) {
#define JS_EXPAND_DEF(name, type, _) \
      case JS::TraceKind::name: \
        return f. JS_DEPENDENT_TEMPLATE_HINT operator()<type>(mozilla::Forward<Args>(args)...);
      JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF
      default:
          MOZ_CRASH("Invalid trace kind in DispatchTraceKindTyped.");
    }
}
#undef JS_DEPENDENT_TEMPLATE_HINT

template <typename F, typename... Args>
auto
DispatchTraceKindTyped(F f, void* thing, JS::TraceKind traceKind, Args&&... args)
  -> decltype(f(static_cast<JSObject*>(nullptr), mozilla::Forward<Args>(args)...))
{
    switch (traceKind) {
#define JS_EXPAND_DEF(name, type, _) \
      case JS::TraceKind::name: \
          return f(static_cast<type*>(thing), mozilla::Forward<Args>(args)...);
      JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF
      default:
          MOZ_CRASH("Invalid trace kind in DispatchTraceKindTyped.");
    }
}

} // namespace JS

#endif // js_TraceKind_h
