/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_HashTable_h
#define js_HashTable_h

#include "mozilla/HashTable.h"

#include "jstypes.h"

namespace js {

using HashNumber = mozilla::HashNumber;
static const uint32_t kHashNumberBits = mozilla::kHashNumberBits;

class JS_PUBLIC_API TempAllocPolicy;

template <class T>
using DefaultHasher = mozilla::DefaultHasher<T>;

template <typename Key>
using PointerHasher = mozilla::PointerHasher<Key>;

template <typename T, class HashPolicy = mozilla::DefaultHasher<T>,
          class AllocPolicy = TempAllocPolicy>
using HashSet = mozilla::HashSet<T, HashPolicy, AllocPolicy>;

template <typename Key, typename Value,
          class HashPolicy = mozilla::DefaultHasher<Key>,
          class AllocPolicy = TempAllocPolicy>
using HashMap = mozilla::HashMap<Key, Value, HashPolicy, AllocPolicy>;

}  // namespace js

#endif /* js_HashTable_h */
