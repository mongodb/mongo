/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ConstantCompareOperand_h
#define vm_ConstantCompareOperand_h

#include <stdint.h>

#include "js/Value.h"

namespace js {

/*
 * Simple struct for encoding comparison ops with constant values (
 * that can be extracted during parsing) presently used with the
 * StrictConstantEq and StrictConstantNe opcodes. The operand encodes
 * the type of the constant and the payload if applicable within a
 * uint16_t. The type is encoded in the range of the top 8 bits, and the
 * payload in the bottom 8 bits.
 *
 * TODO (Bug 1958722): Investigate if larger payloads can be supported
 * in the empty bits.
 */
struct ConstantCompareOperand {
 public:
  enum class EncodedType : uint16_t {
    Int32 = JSVAL_TYPE_INT32,
    Boolean = JSVAL_TYPE_BOOLEAN,
    Null = JSVAL_TYPE_NULL,
    Undefined = JSVAL_TYPE_UNDEFINED,
  };
  static constexpr uint16_t OFFSET_OF_TYPE =
      sizeof(jsbytecode) + sizeof(JSValueType);
  static constexpr uint16_t OFFSET_OF_VALUE = sizeof(jsbytecode);

 private:
  uint16_t value_;

  static constexpr uint8_t SHIFT_TYPE = 8;
  static constexpr uint16_t MASK_TYPE = 0xFF00;
  static constexpr uint16_t MASK_VALUE = 0x00FF;

  static uint16_t encodeType(EncodedType type) {
    return static_cast<uint16_t>(type) << SHIFT_TYPE;
  }

  explicit ConstantCompareOperand(uint16_t value) : value_(value) {}

 public:
  explicit ConstantCompareOperand(int8_t value)
      : value_(encodeType(EncodedType::Int32) | static_cast<uint8_t>(value)) {
    MOZ_ASSERT(this->toInt32() == value);
  }
  explicit ConstantCompareOperand(bool value)
      : value_(encodeType(EncodedType::Boolean) | value) {
    MOZ_ASSERT(this->toBoolean() == value);
  }
  explicit ConstantCompareOperand(EncodedType type) : value_(encodeType(type)) {
    MOZ_ASSERT(type == EncodedType::Undefined || type == EncodedType::Null);
    MOZ_ASSERT_IF(type == EncodedType::Undefined, this->isUndefined());
    MOZ_ASSERT_IF(type == EncodedType::Null, this->isNull());
  }

  static ConstantCompareOperand fromRawValue(uint16_t value) {
    return ConstantCompareOperand(value);
  }

  static bool CanEncodeInt32ValueAsOperand(int32_t value) {
    return value >= INT8_MIN && value <= INT8_MAX;
  }

  EncodedType type() const {
    return static_cast<EncodedType>((value_ & MASK_TYPE) >> SHIFT_TYPE);
  }

  bool isNumber() const { return type() == EncodedType::Int32; }
  bool isBoolean() const { return type() == EncodedType::Boolean; }
  bool isNullOrUndefined() const {
    return type() == EncodedType::Undefined || type() == EncodedType::Null;
  }
  bool isUndefined() const { return type() == EncodedType::Undefined; }
  bool isNull() const { return type() == EncodedType::Null; }

  int32_t toInt32() const {
    MOZ_ASSERT(type() == EncodedType::Int32);
    return static_cast<int8_t>(value_ & MASK_VALUE);
  }

  double toNumber() const {
    MOZ_ASSERT(type() == EncodedType::Int32);
    return static_cast<int8_t>(value_ & MASK_VALUE);
  }

  bool toBoolean() const {
    MOZ_ASSERT(type() == EncodedType::Boolean);
    return bool(value_ & MASK_VALUE);
  }

  uint16_t rawValue() const { return value_; }
};

}  // namespace js

#endif /* vm_ConstantCompareOperand_h */
