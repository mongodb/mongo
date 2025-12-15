/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ABIArgGenerator_h
#define jit_ABIArgGenerator_h

#include "mozilla/Assertions.h"

#include <stdint.h>

#include "jit/Assembler.h"
#include "jit/IonTypes.h"
#include "jit/RegisterSets.h"
#include "wasm/WasmFrame.h"

namespace js::jit {

static inline MIRType ToMIRType(MIRType t) { return t; }

static inline MIRType ToMIRType(ABIType argType) {
  switch (argType) {
    case ABIType::General:
      return MIRType::Pointer;
    case ABIType::Float64:
      return MIRType::Double;
    case ABIType::Float32:
      return MIRType::Float32;
    case ABIType::Int32:
      return MIRType::Int32;
    case ABIType::Int64:
      return MIRType::Int64;
    case ABIType::Void:
      return MIRType::None;
    default:
      break;
  }
  MOZ_CRASH("unexpected argType");
}

template <class VecT, class ABIArgGeneratorT>
class ABIArgIterBase {
  ABIArgGeneratorT gen_;
  const VecT& types_;
  unsigned i_;

  void settle() {
    if (!done()) gen_.next(ToMIRType(types_[i_]));
  }

 public:
  explicit ABIArgIterBase(const VecT& types) : types_(types), i_(0) {
    settle();
  }
  void operator++(int) {
    MOZ_ASSERT(!done());
    i_++;
    settle();
  }
  bool done() const { return i_ == types_.length(); }

  ABIArg* operator->() {
    MOZ_ASSERT(!done());
    return &gen_.current();
  }
  ABIArg& operator*() {
    MOZ_ASSERT(!done());
    return gen_.current();
  }

  unsigned index() const {
    MOZ_ASSERT(!done());
    return i_;
  }
  MIRType mirType() const {
    MOZ_ASSERT(!done());
    return ToMIRType(types_[i_]);
  }
  uint32_t stackBytesConsumedSoFar() const {
    return gen_.stackBytesConsumedSoFar();
  }
};

// This is not an alias because we want to allow class template argument
// deduction.
template <class VecT>
class ABIArgIter : public ABIArgIterBase<VecT, ABIArgGenerator> {
 public:
  explicit ABIArgIter(const VecT& types)
      : ABIArgIterBase<VecT, ABIArgGenerator>(types) {}
};

class WasmABIArgGenerator : public ABIArgGenerator {
 public:
  WasmABIArgGenerator() {
    increaseStackOffset(wasm::FrameWithInstances::sizeOfInstanceFields());
  }
};

template <class VecT>
class WasmABIArgIter : public ABIArgIterBase<VecT, WasmABIArgGenerator> {
 public:
  explicit WasmABIArgIter(const VecT& types)
      : ABIArgIterBase<VecT, WasmABIArgGenerator>(types) {}
};

}  // namespace js::jit

#endif /* jit_ABIArgGenerator_h */
