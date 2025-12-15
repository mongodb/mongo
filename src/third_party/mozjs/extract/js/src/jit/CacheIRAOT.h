/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIRAOT_h
#define jit_CacheIRAOT_h

#include "mozilla/Span.h"

#include "jit/CacheIR.h"
#include "jit/CacheIRWriter.h"

struct JSContext;

namespace js {
namespace jit {

class JitZone;

struct AOTStubFieldData {
  StubField::Type type;
  uint64_t data;
};

struct CacheIRAOTStub {
  CacheKind kind;
  uint32_t numOperandIds;
  uint32_t numInputOperands;
  uint32_t numInstructions;
  TypeData typeData;
  uint32_t stubDataSize;
  const AOTStubFieldData* stubfields;
  size_t stubfieldCount;
  const uint32_t* operandLastUsed;  // length: numOperandIds
  const uint8_t* data;
  size_t dataLength;
};

mozilla::Span<const CacheIRAOTStub> GetAOTStubs();
void FillAOTICs(JSContext* cx, JitZone* zone);

}  // namespace jit
}  // namespace js

#endif /* jit_CacheIRAOT_h */
