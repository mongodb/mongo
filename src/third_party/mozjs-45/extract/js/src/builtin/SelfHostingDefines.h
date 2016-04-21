/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Specialized .h file to be used by both JS and C++ code.

#ifndef builtin_SelfHostingDefines_h
#define builtin_SelfHostingDefines_h

// Utility macros.
#define TO_INT32(x) ((x) | 0)
#define TO_UINT32(x) ((x) >>> 0)
#define IS_UINT32(x) ((x) >>> 0 === (x))
#define MAX_NUMERIC_INDEX 0x1fffffffffffff // == Math.pow(2, 53) - 1

// Unforgeable versions of ARRAY.push(ELEMENT) and ARRAY.slice.
#define ARRAY_PUSH(ARRAY, ELEMENT) \
  callFunction(std_Array_push, ARRAY, ELEMENT);
#define ARRAY_SLICE(ARRAY, ELEMENT) \
  callFunction(std_Array_slice, ARRAY, ELEMENT);

// Property descriptor attributes.
#define ATTR_ENUMERABLE         0x01
#define ATTR_CONFIGURABLE       0x02
#define ATTR_WRITABLE           0x04

#define ATTR_NONENUMERABLE      0x08
#define ATTR_NONCONFIGURABLE    0x10
#define ATTR_NONWRITABLE        0x20

// The extended slot in which the self-hosted name for self-hosted builtins is
// stored.
#define LAZY_FUNCTION_NAME_SLOT 0

// Stores the private WeakMap slot used for WeakSets
#define WEAKSET_MAP_SLOT 0

#define ITERATOR_SLOT_TARGET 0
// Used for collection iterators.
#define ITERATOR_SLOT_RANGE 1
// Used for list, i.e. Array and String, iterators.
#define ITERATOR_SLOT_NEXT_INDEX 1
#define ITERATOR_SLOT_ITEM_KIND 2
// Used for ListIterator.
#define ITERATOR_SLOT_NEXT_METHOD 2

#define ITEM_KIND_KEY 0
#define ITEM_KIND_VALUE 1
#define ITEM_KIND_KEY_AND_VALUE 2

// NB: keep these in sync with the copy in jsfriendapi.h.
#define JSITER_OWNONLY    0x8   /* iterate over obj's own properties only */
#define JSITER_HIDDEN     0x10  /* also enumerate non-enumerable properties */
#define JSITER_SYMBOLS    0x20  /* also include symbol property keys */
#define JSITER_SYMBOLSONLY 0x40 /* exclude string property keys */

#endif
