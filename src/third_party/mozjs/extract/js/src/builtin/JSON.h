/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

extern JSObject*
InitJSONClass(JSContext* cx, HandleObject obj);

enum class StringifyBehavior {
    Normal,
    RestrictedSafe
};

/**
 * If maybeSafely is true, Stringify will attempt to assert the API requirements
 * of JS::ToJSONMaybeSafely as it traverses the graph, and will not try to
 * invoke .toJSON on things as it goes.
 */
extern bool
Stringify(JSContext* cx, js::MutableHandleValue vp, JSObject* replacer,
          const Value& space, StringBuffer& sb, StringifyBehavior stringifyBehavior);

template <typename CharT>
extern bool
ParseJSONWithReviver(JSContext* cx, const mozilla::Range<const CharT> chars,
                     HandleValue reviver, MutableHandleValue vp);

} // namespace js

#endif /* builtin_JSON_h */
