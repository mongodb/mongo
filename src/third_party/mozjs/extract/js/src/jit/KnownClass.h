/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_KnownClass_h
#define jit_KnownClass_h

#include "jspubtd.h"

namespace js {
namespace jit {

class MDefinition;

// Users of this enum often can't handle Proxy and Wrapper classes,
// as well as non-Function callables.
enum class KnownClass {
  PlainObject,
  Array,
  Function,
  RegExp,
  ArrayIterator,
  StringIterator,
  RegExpStringIterator,
  None
};

KnownClass GetObjectKnownClass(const MDefinition* def);
const JSClass* GetObjectKnownJSClass(const MDefinition* def);

}  // namespace jit
}  // namespace js

#endif  // jit_KnownClass_h
