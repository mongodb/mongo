/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_List_h
#define vm_List_h

#include "NamespaceImports.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

namespace js {

/**
 * The List specification type, ECMA-262 6.2.1.
 * <https://tc39.github.io/ecma262/#sec-list-and-record-specification-type>
 *
 * Lists are simple mutable sequences of values. Many standards use them.
 * Abstractly, they're not objects; they don't have properties or prototypes;
 * they're for internal specification use only. ListObject is our most direct
 * implementation of a List: store the values in the slots of a JSObject.
 *
 * We often implement Lists in other ways. For example, builtin/Utilities.js
 * contains a completely unrelated List constructor that's used in self-hosted
 * code. And AsyncGeneratorObject optimizes away the ListObject in the common
 * case where its internal queue never holds more than one element.
 *
 * ListObjects must not be exposed to content scripts.
 */
class ListObject : public NativeObject {
 public:
  static const JSClass class_;

  [[nodiscard]] inline static ListObject* create(JSContext* cx);

  uint32_t length() const { return getDenseInitializedLength(); }

  bool isEmpty() const { return length() == 0; }

  const Value& get(uint32_t index) const { return getDenseElement(index); }

  template <class T>
  T& getAs(uint32_t index) const {
    return get(index).toObject().as<T>();
  }

  /**
   * Add an element to the end of the list. Returns false on OOM.
   */
  [[nodiscard]] inline bool append(JSContext* cx, HandleValue value);

  /**
   * Adds |value| and |size| elements to a list consisting of (value, size)
   * pairs stored in successive elements.
   *
   * This function is intended for use by streams code's queue-with-sizes data
   * structure and related operations.  See builtin/streams/QueueWithSizes*.
   * (You *could* use this on any list of even length without issue, but it's
   * hard to imagine realistic situations where you'd want to...)
   */
  [[nodiscard]] inline bool appendValueAndSize(JSContext* cx, HandleValue value,
                                               double size);

  /**
   * Remove and return the first element of the list.
   *
   * Precondition: This list is not empty.
   */
  inline JS::Value popFirst(JSContext* cx);

  /**
   * Remove the first two elements from a nonempty list of (value, size) pairs
   * of elements.
   */
  inline void popFirstPair(JSContext* cx);

  /**
   * Remove and return the first element of the list.
   *
   * Precondition: This list is not empty, and the first element
   * is an object of class T.
   */
  template <class T>
  inline T& popFirstAs(JSContext* cx);
};

}  // namespace js

#endif  // vm_List_h
