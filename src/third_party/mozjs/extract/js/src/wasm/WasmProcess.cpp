/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2017 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmProcess.h"

#include "mozilla/Attributes.h"

#include "gc/Memory.h"
#include "threading/ExclusiveData.h"
#include "vm/MutexIDs.h"
#include "vm/Runtime.h"
#include "wasm/WasmBuiltinModule.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmStaticTypeDefs.h"

using namespace js;
using namespace wasm;

mozilla::Atomic<bool> wasm::CodeExists(false);

// Per-process map from values of program-counter (pc) to CodeBlocks.
//
// Whenever a new CodeBlock is ready to use, it has to be registered so that
// we can have fast lookups from pc to CodeBlocks in numerous places. Since
// wasm compilation may be tiered, and the second tier doesn't have access to
// any JSContext/JS::Compartment/etc lying around, we have to use a process-wide
// map instead.

// This field is only atomic to handle buggy scenarios where we crash during
// startup or shutdown and thus racily perform wasm::LookupCodeBlock() from
// the crashing thread.

static mozilla::Atomic<ThreadSafeCodeBlockMap*> sThreadSafeCodeBlockMap(
    nullptr);

bool wasm::RegisterCodeBlock(const CodeBlock* cs) {
  if (cs->length() == 0) {
    return true;
  }

  // This function cannot race with startup/shutdown.
  ThreadSafeCodeBlockMap* map = sThreadSafeCodeBlockMap;
  MOZ_RELEASE_ASSERT(map);
  bool result = map->insert(cs);
  if (result) {
    CodeExists = true;
  }
  return result;
}

void wasm::UnregisterCodeBlock(const CodeBlock* cs) {
  if (cs->length() == 0) {
    return;
  }

  // This function cannot race with startup/shutdown.
  ThreadSafeCodeBlockMap* map = sThreadSafeCodeBlockMap;
  MOZ_RELEASE_ASSERT(map);
  size_t newCount = map->remove(cs);
  if (newCount == 0) {
    CodeExists = false;
  }
}

const CodeBlock* wasm::LookupCodeBlock(
    const void* pc, const CodeRange** codeRange /*= nullptr */) {
  ThreadSafeCodeBlockMap* map = sThreadSafeCodeBlockMap;
  if (!map) {
    return nullptr;
  }

  return map->lookup(pc, codeRange);
}

const Code* wasm::LookupCode(const void* pc,
                             const CodeRange** codeRange /* = nullptr */) {
  const CodeBlock* found = LookupCodeBlock(pc, codeRange);
  MOZ_ASSERT_IF(!found && codeRange, !*codeRange);
  return found ? found->code : nullptr;
}

bool wasm::InCompiledCode(void* pc) {
  if (LookupCodeBlock(pc)) {
    return true;
  }

  const CodeRange* codeRange;
  const uint8_t* codeBase;
  return LookupBuiltinThunk(pc, &codeRange, &codeBase);
}

#ifdef WASM_SUPPORTS_HUGE_MEMORY
/*
 * Some 64 bit systems greatly limit the range of available virtual memory. We
 * require about 6GiB for each wasm huge memory, which can exhaust the address
 * spaces of these systems quickly. In order to avoid this, we only enable huge
 * memory if we observe a large enough address space.
 *
 * This number is conservatively chosen to continue using huge memory on our
 * smallest address space system, Android on ARM64 (39 bits), along with a bit
 * for error in detecting the address space limit.
 */
static const size_t MinAddressBitsForHugeMemory = 38;

/*
 * In addition to the above, some systems impose an independent limit on the
 * amount of virtual memory that may be used.
 */
static const size_t MinVirtualMemoryLimitForHugeMemory =
    size_t(1) << MinAddressBitsForHugeMemory;
#endif

static bool sHugeMemoryEnabled32 = false;

bool wasm::IsHugeMemoryEnabled(wasm::AddressType t) {
  if (t == AddressType::I64) {
    // No support for huge memory with 64-bit memories
    return false;
  }
  return sHugeMemoryEnabled32;
}

void ConfigureHugeMemory() {
#ifdef WASM_SUPPORTS_HUGE_MEMORY
  MOZ_ASSERT(!sHugeMemoryEnabled32);

  if (JS::Prefs::wasm_disable_huge_memory()) {
    return;
  }

  if (gc::SystemAddressBits() < MinAddressBitsForHugeMemory) {
    return;
  }

  if (gc::VirtualMemoryLimit() != size_t(-1) &&
      gc::VirtualMemoryLimit() < MinVirtualMemoryLimitForHugeMemory) {
    return;
  }

  sHugeMemoryEnabled32 = true;
#endif
}

const TagType* wasm::sWrappedJSValueTagType = nullptr;

static bool InitTagForJSValue() {
  MutableTagType type = js_new<TagType>();
  if (!type || !type->initialize(StaticTypeDefs::jsTag)) {
    return false;
  }
  MOZ_ASSERT(WrappedJSValueTagType_ValueOffset == type->argOffsets()[0]);

  type.forget(&sWrappedJSValueTagType);

  return true;
}

bool wasm::Init() {
  MOZ_RELEASE_ASSERT(!sThreadSafeCodeBlockMap);

  // Assert invariants that should universally hold true, but cannot be checked
  // at compile time.
  uintptr_t pageSize = gc::SystemPageSize();
  MOZ_RELEASE_ASSERT(wasm::NullPtrGuardSize <= pageSize);
  // MONGODB MODIFICATION: Use portable mechanism to check for nullptr, to satisfy MSVC.
  MOZ_RELEASE_ASSERT(reinterpret_cast<intptr_t>(nullptr) == AnyRef::NullRefValue);

  ConfigureHugeMemory();

  AutoEnterOOMUnsafeRegion oomUnsafe;
  ThreadSafeCodeBlockMap* map = js_new<ThreadSafeCodeBlockMap>();
  if (!map) {
    oomUnsafe.crash("js::wasm::Init");
  }

  if (!StaticTypeDefs::init()) {
    oomUnsafe.crash("js::wasm::Init");
  }

  // This uses StaticTypeDefs
  if (!BuiltinModuleFuncs::init()) {
    oomUnsafe.crash("js::wasm::Init");
  }

  sThreadSafeCodeBlockMap = map;

  if (!InitTagForJSValue()) {
    oomUnsafe.crash("js::wasm::Init");
  }

  return true;
}

void wasm::ShutDown() {
  // If there are live runtimes then we are already pretty much leaking the
  // world, so to avoid spurious assertions (which are valid and valuable when
  // there are not live JSRuntimes), don't bother releasing anything here.
  if (JSRuntime::hasLiveRuntimes()) {
    return;
  }

  BuiltinModuleFuncs::destroy();
  StaticTypeDefs::destroy();
  PurgeCanonicalTypes();

  if (sWrappedJSValueTagType) {
    sWrappedJSValueTagType->Release();
    sWrappedJSValueTagType = nullptr;
  }

  // After signalling shutdown by clearing sThreadSafeCodeBlockMap, wait for
  // concurrent wasm::LookupCodeBlock()s to finish.
  ThreadSafeCodeBlockMap* map = sThreadSafeCodeBlockMap;
  MOZ_RELEASE_ASSERT(map);
  sThreadSafeCodeBlockMap = nullptr;
  while (map->numActiveLookups() > 0) {
  }

  ReleaseBuiltinThunks();
  js_delete(map);
}
