/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/KnownClass.h"

#include "jit/MIR.h"
#include "vm/ArrayObject.h"
#include "vm/Iteration.h"
#include "vm/JSFunction.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/RegExpObject.h"

using namespace js;
using namespace js::jit;

KnownClass jit::GetObjectKnownClass(const MDefinition* def) {
  MOZ_ASSERT(def->type() == MIRType::Object);

  switch (def->op()) {
    case MDefinition::Opcode::NewArray:
    case MDefinition::Opcode::NewArrayDynamicLength:
      return KnownClass::Array;

    case MDefinition::Opcode::NewObject:
    case MDefinition::Opcode::CreateThis:
    case MDefinition::Opcode::CreateThisWithTemplate:
      return KnownClass::PlainObject;

    case MDefinition::Opcode::Lambda:
    case MDefinition::Opcode::LambdaArrow:
    case MDefinition::Opcode::FunctionWithProto:
      return KnownClass::Function;

    case MDefinition::Opcode::RegExp:
      return KnownClass::RegExp;

    case MDefinition::Opcode::NewIterator:
      switch (def->toNewIterator()->type()) {
        case MNewIterator::ArrayIterator:
          return KnownClass::ArrayIterator;
        case MNewIterator::StringIterator:
          return KnownClass::StringIterator;
        case MNewIterator::RegExpStringIterator:
          return KnownClass::RegExpStringIterator;
      }
      MOZ_CRASH("unreachable");

    case MDefinition::Opcode::Phi: {
      if (def->numOperands() == 0) {
        return KnownClass::None;
      }

      MDefinition* op = def->getOperand(0);
      // Check for Phis to avoid recursion for now.
      if (op->isPhi()) {
        return KnownClass::None;
      }

      KnownClass known = GetObjectKnownClass(op);
      if (known == KnownClass::None) {
        return KnownClass::None;
      }

      for (size_t i = 1; i < def->numOperands(); i++) {
        op = def->getOperand(i);
        if (op->isPhi() || GetObjectKnownClass(op) != known) {
          return KnownClass::None;
        }
      }

      return known;
    }

    default:
      break;
  }

  return KnownClass::None;
}

const JSClass* jit::GetObjectKnownJSClass(const MDefinition* def) {
  switch (GetObjectKnownClass(def)) {
    case KnownClass::PlainObject:
      return &PlainObject::class_;
    case KnownClass::Array:
      return &ArrayObject::class_;
    case KnownClass::Function:
      return &JSFunction::class_;
    case KnownClass::RegExp:
      return &RegExpObject::class_;
    case KnownClass::ArrayIterator:
      return &ArrayIteratorObject::class_;
    case KnownClass::StringIterator:
      return &StringIteratorObject::class_;
    case KnownClass::RegExpStringIterator:
      return &RegExpStringIteratorObject::class_;
    case KnownClass::None:
      break;
  }

  return nullptr;
}
