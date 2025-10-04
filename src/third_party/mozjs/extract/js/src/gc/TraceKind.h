/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_TraceKind_h
#define gc_TraceKind_h

#include "js/TraceKind.h"

namespace js {
namespace gc {

// Map from all trace kinds to the base GC type.
template <JS::TraceKind kind>
struct MapTraceKindToType {};

#define DEFINE_TRACE_KIND_MAP(name, type, _, _1)   \
  template <>                                      \
  struct MapTraceKindToType<JS::TraceKind::name> { \
    using Type = type;                             \
  };
JS_FOR_EACH_TRACEKIND(DEFINE_TRACE_KIND_MAP);
#undef DEFINE_TRACE_KIND_MAP

// Map from a possibly-derived type to the base GC type.
template <typename T>
struct BaseGCType {
  using type =
      typename MapTraceKindToType<JS::MapTypeToTraceKind<T>::kind>::Type;
  static_assert(std::is_base_of_v<type, T>, "Failed to find base type");
};

template <typename T>
struct TraceKindCanBeGray {};
#define EXPAND_TRACEKIND_DEF(_, type, canBeGray, _1) \
  template <>                                        \
  struct TraceKindCanBeGray<type> {                  \
    static constexpr bool value = canBeGray;         \
  };
JS_FOR_EACH_TRACEKIND(EXPAND_TRACEKIND_DEF)
#undef EXPAND_TRACEKIND_DEF

struct TraceKindCanBeGrayFunctor {
  template <typename T>
  bool operator()() {
    return TraceKindCanBeGray<T>::value;
  }
};

static inline bool TraceKindCanBeMarkedGray(JS::TraceKind kind) {
  return DispatchTraceKindTyped(TraceKindCanBeGrayFunctor(), kind);
}

} /* namespace gc */
} /* namespace js */

#endif /* gc_TraceKind_h */
