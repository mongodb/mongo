/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ABIFunctionType_h
#define jit_ABIFunctionType_h

#include <initializer_list>
#include <stdint.h>

#include "jit/ABIFunctionTypeGenerated.h"

namespace js {
namespace jit {

enum class ABIType {
  // A pointer sized integer
  General = 0x1,
  // A 32-bit integer
  Int32 = 0x2,
  // A 64-bit integer
  Int64 = 0x3,
  // A 32-bit floating point number
  Float32 = 0x4,
  // A 64-bit floating point number
  Float64 = 0x5,
  // No result
  Void = 0x6,
};

const size_t ABITypeArgShift = 0x3;
const size_t ABITypeArgMask = (1 << ABITypeArgShift) - 1;

namespace detail {

static constexpr uint64_t MakeABIFunctionType(
    ABIType ret, std::initializer_list<ABIType> args) {
  uint64_t abiType = 0;
  for (auto arg : args) {
    abiType <<= ABITypeArgShift;
    abiType |= (uint64_t)arg;
  }
  abiType <<= ABITypeArgShift;
  abiType |= (uint64_t)ret;
  return abiType;
}

}  // namespace detail

enum ABIFunctionType : uint64_t {
  // The enum must be explicitly typed to avoid UB: some validly constructed
  // members are larger than any explicitly declared members.
  ABI_FUNCTION_TYPE_ENUM
};

static constexpr ABIFunctionType MakeABIFunctionType(
    ABIType ret, std::initializer_list<ABIType> args) {
  return ABIFunctionType(detail::MakeABIFunctionType(ret, args));
}

}  // namespace jit
}  // namespace js

#endif /* jit_ABIFunctionType_h */
