/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_TraceKind_h
#define js_TraceKind_h

#include "js/TypeDecls.h"

// Forward declarations of all the types a TraceKind can denote.
namespace js {
class BaseShape;
class LazyScript;
class ObjectGroup;
class Shape;
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
    String = 0x01,
    Symbol = 0x02,
    Script = 0x03,

    // Shape details are exposed through JS_TraceShapeCycleCollectorChildren.
    Shape = 0x04,

    // ObjectGroup details are exposed through JS_TraceObjectGroupCycleCollectorChildren.
    ObjectGroup = 0x05,

    // The kind associated with a nullptr.
    Null = 0x06,

    // The following kinds do not have an exposed C++ idiom.
    BaseShape = 0x0F,
    JitCode = 0x1F,
    LazyScript = 0x2F
};
const static uintptr_t OutOfLineTraceKindMask = 0x07;
static_assert(uintptr_t(JS::TraceKind::BaseShape) & OutOfLineTraceKindMask, "mask bits are set");
static_assert(uintptr_t(JS::TraceKind::JitCode) & OutOfLineTraceKindMask, "mask bits are set");
static_assert(uintptr_t(JS::TraceKind::LazyScript) & OutOfLineTraceKindMask, "mask bits are set");

#define JS_FOR_EACH_TRACEKIND(D) \
 /* PrettyName       TypeName           AddToCCKind */ \
    D(BaseShape,     js::BaseShape,     true) \
    D(JitCode,       js::jit::JitCode,  true) \
    D(LazyScript,    js::LazyScript,    true) \
    D(Object,        JSObject,          true) \
    D(ObjectGroup,   js::ObjectGroup,   true) \
    D(Script,        JSScript,          true) \
    D(Shape,         js::Shape,         true) \
    D(String,        JSString,          false) \
    D(Symbol,        JS::Symbol,        false)

// Map from base trace type to the trace kind.
template <typename T> struct MapTypeToTraceKind {};
#define JS_EXPAND_DEF(name, type, _) \
    template <> struct MapTypeToTraceKind<type> { \
        static const JS::TraceKind kind = JS::TraceKind::name; \
    };
JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF

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

// GCC and Clang require an explicit template declaration in front of the
// specialization of operator() because it is a dependent template. MSVC, on
// the other hand, gets very confused if we have a |template| token there.
// The clang-cl front end defines _MSC_VER, but still requires the explicit
// template declaration, so we must test for __clang__ here as well.
#if defined(_MSC_VER) && !defined(__clang__)
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
