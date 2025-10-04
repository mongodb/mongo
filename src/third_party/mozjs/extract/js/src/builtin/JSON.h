/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_JSON_h
#define builtin_JSON_h

#include "mozilla/Range.h"

#include "NamespaceImports.h"

#include "js/RootingAPI.h"

namespace js {

class StringBuffer;

extern const JSClass JSONClass;

enum class StringifyBehavior {
  // Attempt an optimistic fast path if possible, bailing back to the slow path
  // if anything is encountered that could invalidate the fast path results per
  // spec. Default behavior for non-DEBUG builds.
  Normal,

  // Handle a subset of functionality when called by JS::ToJSONMaybeSafely.
  // Different restrictions than the fast path described for Normal. See the
  // Stringify() comment below for details.
  RestrictedSafe,

  // If the fast path fails, throw an exception instead of falling back to the
  // slow path. Useful for testing that something that should be handled by the
  // fast path actually is.
  FastOnly,

  // Do not attempt the fast path. Useful for timing comparisons.
  SlowOnly,

  // Attempt to run both the fast and slow paths and compare the results,
  // crashing on any discrepancy. For correctness testing only. Default behavior
  // when DEBUG is defined.
  Compare
};

/**
 * If stringifyBehavior is RestrictedSafe, Stringify will attempt to assert the
 * API requirements of JS::ToJSONMaybeSafely as it traverses the graph, and will
 * not try to invoke .toJSON on things as it goes.
 */
extern bool Stringify(JSContext* cx, js::MutableHandleValue vp,
                      JSObject* replacer, const Value& space, StringBuffer& sb,
                      StringifyBehavior stringifyBehavior);

template <typename CharT>
extern bool ParseJSONWithReviver(JSContext* cx,
                                 const mozilla::Range<const CharT> chars,
                                 HandleValue reviver, MutableHandleValue vp);

}  // namespace js

#endif /* builtin_JSON_h */
