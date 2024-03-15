/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_TupleType_h
#define vm_TupleType_h

#include <cstdint>
#include <functional>
#include "vm/JSContext.h"
#include "vm/NativeObject.h"

namespace JS {

class TupleType final : public js::NativeObject {
 public:
  static const js::ClassSpec classSpec_;
  static const JSClass class_;
  static const JSClass protoClass_;

 public:
  static TupleType* create(JSContext* cx, uint32_t length,
                           const Value* elements);

  static TupleType* createUninitialized(JSContext* cx, uint32_t initialLength);

  static TupleType* createUnchecked(JSContext* cx,
                                    Handle<js::ArrayObject*> aObj);

  bool initializeNextElement(JSContext* cx, HandleValue elt);
  void finishInitialization(JSContext* cx);
  static js::Shape* getInitialShape(JSContext* cx);

  static bool copy(JSContext* cx, Handle<TupleType*> in,
                   MutableHandle<TupleType*> out);

  bool getOwnProperty(HandleId id, MutableHandleValue vp) const;
  inline uint32_t length() const { return getElementsHeader()->length; }

  // Methods defined on Tuple.prototype
  [[nodiscard]] static bool lengthAccessor(JSContext* cx, unsigned argc,
                                           Value* vp);

  // Comparison functions
  static bool sameValueZero(JSContext* cx, TupleType* lhs, TupleType* rhs,
                            bool* equal);
  static bool sameValue(JSContext* cx, TupleType* lhs, TupleType* rhs,
                        bool* equal);

  using ElementHasher = std::function<js::HashNumber(const Value& child)>;
  js::HashNumber hash(const ElementHasher& hasher) const;

  bool ensureAtomized(JSContext* cx);
  bool isAtomized() const { return getElementsHeader()->tupleIsAtomized(); }

  // This can be used to compare atomized tuples.
  static bool sameValueZero(TupleType* lhs, TupleType* rhs);

  static TupleType& thisTupleValue(const Value& val);

 private:
  template <bool Comparator(JSContext*, HandleValue, HandleValue, bool*)>
  static bool sameValueWith(JSContext* cx, TupleType* lhs, TupleType* rhs,
                            bool* equal);
};

}  // namespace JS

namespace js {

extern JSString* TupleToSource(JSContext* cx, Handle<TupleType*> tup);

bool IsTuple(const Value& v);

extern bool tuple_toReversed(JSContext* cx, unsigned argc, Value* vp);
extern bool tuple_with(JSContext* cx, unsigned argc, Value* vp);
extern bool tuple_slice(JSContext* cx, unsigned argc, Value* vp);
extern bool tuple_is_tuple(JSContext* cx, unsigned argc, Value* vp);
extern bool tuple_value_of(JSContext* cx, unsigned argc, Value* vp);
extern bool tuple_of(JSContext* cx, unsigned argc, Value* vp);
extern bool tuple_construct(JSContext* cx, unsigned argc, Value* vp);

}  // namespace js

#endif
