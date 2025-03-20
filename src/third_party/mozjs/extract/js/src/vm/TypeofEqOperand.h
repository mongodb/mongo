/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_TypeofEqOperand_h
#define vm_TypeofEqOperand_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stdint.h>  // uint8_t

#include "jspubtd.h"     // JSType
#include "vm/Opcodes.h"  // JSOp

namespace js {

struct TypeofEqOperand {
  static constexpr uint8_t TYPE_MASK = 0x0f;
  static constexpr uint8_t NEQ_BIT = 0x80;

 private:
  uint8_t value;

  static uint8_t toNeqBit(JSOp compareOp) {
    MOZ_ASSERT(compareOp == JSOp::Eq || compareOp == JSOp::Ne);
    return compareOp == JSOp::Ne ? NEQ_BIT : 0;
  }

  explicit TypeofEqOperand(uint8_t value) : value(value) {}

 public:
  TypeofEqOperand(JSType type, JSOp compareOp)
      : value(type | toNeqBit(compareOp)) {}

  static TypeofEqOperand fromRawValue(uint8_t value) {
    return TypeofEqOperand(value);
  }

  JSType type() const { return JSType(value & TYPE_MASK); }
  JSOp compareOp() const { return (value & NEQ_BIT) ? JSOp::Ne : JSOp::Eq; }
  uint8_t rawValue() const { return value; }
};

static_assert((JSTYPE_LIMIT & TypeofEqOperand::TYPE_MASK) == JSTYPE_LIMIT);

}  // namespace js

#endif  // vm_TypeofEqOperand_h
