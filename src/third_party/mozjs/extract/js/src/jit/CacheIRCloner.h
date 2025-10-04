/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIRCloner_h
#define jit_CacheIRCloner_h

#include "mozilla/Attributes.h"

#include <stdint.h>

#include "NamespaceImports.h"

#include "jit/CacheIR.h"
#include "jit/CacheIROpsGenerated.h"
#include "jit/CacheIRReader.h"
#include "jit/CacheIRWriter.h"
#include "js/Id.h"
#include "js/Value.h"

class JSAtom;
class JSObject;
class JSString;

namespace JS {
class Symbol;
}

namespace js {

class BaseScript;
class GetterSetter;
class Shape;

namespace gc {
class AllocSite;
}

namespace jit {

class CacheIRStubInfo;
class ICCacheIRStub;
class JitCode;

class MOZ_RAII CacheIRCloner {
 public:
  explicit CacheIRCloner(ICCacheIRStub* stubInfo);

  void cloneOp(CacheOp op, CacheIRReader& reader, CacheIRWriter& writer);

  CACHE_IR_CLONE_GENERATED

 private:
  const CacheIRStubInfo* stubInfo_;
  const uint8_t* stubData_;

  uintptr_t readStubWord(uint32_t offset);
  int64_t readStubInt64(uint32_t offset);

  Shape* getShapeField(uint32_t stubOffset);
  Shape* getWeakShapeField(uint32_t stubOffset);
  GetterSetter* getWeakGetterSetterField(uint32_t stubOffset);
  JSObject* getObjectField(uint32_t stubOffset);
  JSObject* getWeakObjectField(uint32_t stubOffset);
  JSString* getStringField(uint32_t stubOffset);
  JSAtom* getAtomField(uint32_t stubOffset);
  JS::Symbol* getSymbolField(uint32_t stubOffset);
  BaseScript* getWeakBaseScriptField(uint32_t stubOffset);
  JitCode* getJitCodeField(uint32_t stubOffset);
  uint32_t getRawInt32Field(uint32_t stubOffset);
  const void* getRawPointerField(uint32_t stubOffset);
  jsid getIdField(uint32_t stubOffset);
  const Value getValueField(uint32_t stubOffset);
  uint64_t getRawInt64Field(uint32_t stubOffset);
  double getDoubleField(uint32_t stubOffset);
  gc::AllocSite* getAllocSiteField(uint32_t stubOffset);
};

}  // namespace jit
}  // namespace js

#endif /* jit_CacheIRCloner_h */
