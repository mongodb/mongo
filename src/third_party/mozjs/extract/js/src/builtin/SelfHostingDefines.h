/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Specialized .h file to be used by both JS and C++ code.

#ifndef builtin_SelfHostingDefines_h
#define builtin_SelfHostingDefines_h

// Utility macros.
#define TO_INT32(x) ((x) | 0)
#define TO_UINT32(x) ((x) >>> 0)
#define IS_UINT32(x) ((x) >>> 0 == = (x))
#define MAX_UINT32 0xffffffff
#define MAX_NUMERIC_INDEX 0x1fffffffffffff  // == Math.pow(2, 53) - 1

// Unforgeable version of Function.prototype.apply.
#define FUN_APPLY(FUN, RECEIVER, ARGS) \
  callFunction(std_Function_apply, FUN, RECEIVER, ARGS)

// NB: keep this in sync with the copy in vm/ArgumentsObject.h.
#define MAX_ARGS_LENGTH (500 * 1000)

// NB: keep this in sync with JS::MaxStringLength in js/public/String.h.
#define MAX_STRING_LENGTH ((1 << 30) - 2)

// Spread non-empty argument list of up to 15 elements.
#define SPREAD(v, n) SPREAD_##n(v)
#define SPREAD_1(v) v[0]
#define SPREAD_2(v) SPREAD_1(v), v[1]
#define SPREAD_3(v) SPREAD_2(v), v[2]
#define SPREAD_4(v) SPREAD_3(v), v[3]
#define SPREAD_5(v) SPREAD_4(v), v[4]
#define SPREAD_6(v) SPREAD_5(v), v[5]
#define SPREAD_7(v) SPREAD_6(v), v[6]
#define SPREAD_8(v) SPREAD_7(v), v[7]
#define SPREAD_9(v) SPREAD_8(v), v[8]
#define SPREAD_10(v) SPREAD_9(v), v[9]
#define SPREAD_11(v) SPREAD_10(v), v[10]
#define SPREAD_12(v) SPREAD_11(v), v[11]
#define SPREAD_13(v) SPREAD_12(v), v[12]
#define SPREAD_14(v) SPREAD_13(v), v[13]
#define SPREAD_15(v) SPREAD_14(v), v[14]

// Property descriptor attributes.
#define ATTR_ENUMERABLE 0x01
#define ATTR_CONFIGURABLE 0x02
#define ATTR_WRITABLE 0x04

#define ATTR_NONENUMERABLE 0x08
#define ATTR_NONCONFIGURABLE 0x10
#define ATTR_NONWRITABLE 0x20

// Property descriptor kind, must be different from the descriptor attributes.
#define DATA_DESCRIPTOR_KIND 0x100
#define ACCESSOR_DESCRIPTOR_KIND 0x200

// Property descriptor array indices.
#define PROP_DESC_ATTRS_AND_KIND_INDEX 0
#define PROP_DESC_VALUE_INDEX 1
#define PROP_DESC_GETTER_INDEX 1
#define PROP_DESC_SETTER_INDEX 2

// The extended slot of cloned self-hosted function, in which the self-hosted
// name for self-hosted builtins is stored.
#define LAZY_FUNCTION_NAME_SLOT 0

// The iterated object for Array/String/Map/Set iterators.
#define ITERATOR_SLOT_TARGET 0
// Used for list, i.e. Array and String, iterators.
#define ITERATOR_SLOT_NEXT_INDEX 1
// Item kind for Array iterators.
#define ARRAY_ITERATOR_SLOT_ITEM_KIND 2
// Item kind for Map/Set iterators.
#define MAP_SET_ITERATOR_SLOT_ITEM_KIND 1

#define ITEM_KIND_KEY 0
#define ITEM_KIND_VALUE 1
#define ITEM_KIND_KEY_AND_VALUE 2

#define REGEXP_SOURCE_SLOT 1
#define REGEXP_FLAGS_SLOT 2

#define REGEXP_IGNORECASE_FLAG 0x01
#define REGEXP_GLOBAL_FLAG 0x02
#define REGEXP_MULTILINE_FLAG 0x04
#define REGEXP_STICKY_FLAG 0x08
#define REGEXP_UNICODE_FLAG 0x10
#define REGEXP_DOTALL_FLAG 0x20
#define REGEXP_HASINDICES_FLAG 0x40
#define REGEXP_UNICODESETS_FLAG 0x80

#define REGEXP_STRING_ITERATOR_REGEXP_SLOT 0
#define REGEXP_STRING_ITERATOR_STRING_SLOT 1
#define REGEXP_STRING_ITERATOR_SOURCE_SLOT 2
#define REGEXP_STRING_ITERATOR_FLAGS_SLOT 3
#define REGEXP_STRING_ITERATOR_LASTINDEX_SLOT 4

// Control values for Iterator.range
#define ITERATOR_RANGE_SLOT_START 0
#define ITERATOR_RANGE_SLOT_END 1
#define ITERATOR_RANGE_SLOT_STEP 2
#define ITERATOR_RANGE_SLOT_INCLUSIVE_END 3
#define ITERATOR_RANGE_SLOT_ZERO 4
#define ITERATOR_RANGE_SLOT_ONE 5
#define ITERATOR_RANGE_SLOT_CURRENT_COUNT 6

#define REGEXP_STRING_ITERATOR_LASTINDEX_DONE -1
#define REGEXP_STRING_ITERATOR_LASTINDEX_SLOW -2

#define DATE_METHOD_LOCALE_TIME_STRING 0
#define DATE_METHOD_LOCALE_DATE_STRING 1
#define DATE_METHOD_LOCALE_STRING 2

#define INTL_INTERNALS_OBJECT_SLOT 0
#define INTL_SEGMENTS_STRING_SLOT 1
#define INTL_SEGMENT_ITERATOR_STRING_SLOT 1
#define INTL_SEGMENT_ITERATOR_INDEX_SLOT 3

#define WRAP_FOR_VALID_ITERATOR_ITERATOR_SLOT 0
#define WRAP_FOR_VALID_ITERATOR_NEXT_METHOD_SLOT 1

#define ITERATOR_HELPER_GENERATOR_SLOT 0

#define ASYNC_ITERATOR_HELPER_GENERATOR_SLOT 0

// For explicit resource management.
#define USING_HINT_SYNC 0
#define USING_HINT_ASYNC 1

#define DISPOSABLE_STACK_DISPOSABLE_RESOURCE_STACK_SLOT 0
#define DISPOSABLE_STACK_STATE_SLOT 1

#define DISPOSABLE_STACK_STATE_PENDING 0
#define DISPOSABLE_STACK_STATE_DISPOSED 1

#endif
