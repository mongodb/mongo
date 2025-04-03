/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BindingKind_h
#define vm_BindingKind_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_ASSERT_IF
#include "mozilla/Casting.h"     // mozilla::AssertedCast

#include <stdint.h>  // uint16_t, uint32_t

#include "vm/BytecodeUtil.h"  // LOCALNO_LIMIT, ENVCOORD_SLOT_LIMIT

namespace js {

enum class BindingKind : uint8_t {
  Import,
  FormalParameter,
  Var,
  Let,
  Const,

  // So you think named lambda callee names are consts? Nope! They don't
  // throw when being assigned to in sloppy mode.
  NamedLambdaCallee,

  // ClassBodyScope bindings that aren't bindings in the spec, but are put into
  // a scope as an implementation detail: `.privateBrand`,
  // `.staticInitializers`, private names, and private accessor functions.
  Synthetic,

  // ClassBodyScope binding that stores the function object for a non-static
  // private method.
  PrivateMethod,
};

static inline bool BindingKindIsLexical(BindingKind kind) {
  return kind == BindingKind::Let || kind == BindingKind::Const;
}

class BindingLocation {
 public:
  enum class Kind {
    Global,
    Argument,
    Frame,
    Environment,
    Import,
    NamedLambdaCallee
  };

 private:
  Kind kind_;
  uint32_t slot_;

  BindingLocation(Kind kind, uint32_t slot) : kind_(kind), slot_(slot) {}

 public:
  static BindingLocation Global() {
    return BindingLocation(Kind::Global, UINT32_MAX);
  }

  static BindingLocation Argument(uint16_t slot) {
    return BindingLocation(Kind::Argument, slot);
  }

  static BindingLocation Frame(uint32_t slot) {
    MOZ_ASSERT(slot < LOCALNO_LIMIT);
    return BindingLocation(Kind::Frame, slot);
  }

  static BindingLocation Environment(uint32_t slot) {
    MOZ_ASSERT(slot < ENVCOORD_SLOT_LIMIT);
    return BindingLocation(Kind::Environment, slot);
  }

  static BindingLocation Import() {
    return BindingLocation(Kind::Import, UINT32_MAX);
  }

  static BindingLocation NamedLambdaCallee() {
    return BindingLocation(Kind::NamedLambdaCallee, UINT32_MAX);
  }

  bool operator==(const BindingLocation& other) const {
    return kind_ == other.kind_ && slot_ == other.slot_;
  }

  bool operator!=(const BindingLocation& other) const {
    return !operator==(other);
  }

  Kind kind() const { return kind_; }

  uint32_t slot() const {
    MOZ_ASSERT(kind_ == Kind::Frame || kind_ == Kind::Environment);
    return slot_;
  }

  uint16_t argumentSlot() const {
    MOZ_ASSERT(kind_ == Kind::Argument);
    return mozilla::AssertedCast<uint16_t>(slot_);
  }
};

}  // namespace js

#endif  // vm_BindingKind_h
