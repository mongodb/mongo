/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
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

#include "wasm/WasmRealm.h"

#include "vm/Realm.h"
#include "wasm/WasmDebug.h"
#include "wasm/WasmInstance.h"

#include "debugger/DebugAPI-inl.h"
#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace wasm;

wasm::Realm::Realm(JSRuntime* rt) : runtime_(rt) {}

wasm::Realm::~Realm() { MOZ_ASSERT(instances_.empty()); }

struct InstanceComparator {
  const Instance& target;
  explicit InstanceComparator(const Instance& target) : target(target) {}

  int operator()(const Instance* instance) const {
    if (instance == &target) {
      return 0;
    }

    // Instances can share code, so the segments can be equal (though they
    // can't partially overlap).  If the codeBases are equal, we sort by
    // Instance address.  Thus a Code may map to many instances.

    // Compare by the first tier, always.

    Tier instanceTier = instance->code().stableTier();
    Tier targetTier = target.code().stableTier();

    if (instance->codeBase(instanceTier) == target.codeBase(targetTier)) {
      return instance < &target ? -1 : 1;
    }

    return target.codeBase(targetTier) < instance->codeBase(instanceTier) ? -1
                                                                          : 1;
  }
};

bool wasm::Realm::registerInstance(JSContext* cx,
                                   Handle<WasmInstanceObject*> instanceObj) {
  MOZ_ASSERT(runtime_ == cx->runtime());

  Instance& instance = instanceObj->instance();
  MOZ_ASSERT(this == &instance.realm()->wasm);

  instance.ensureProfilingLabels(cx->runtime()->geckoProfiler().enabled());

  if (instance.debugEnabled() &&
      instance.realm()->debuggerObservesAllExecution()) {
    instance.debug().ensureEnterFrameTrapsState(cx, &instance, true);
  }

  {
    if (!instances_.reserve(instances_.length() + 1)) {
      return false;
    }

    auto runtimeInstances = cx->runtime()->wasmInstances.lock();
    if (!runtimeInstances->reserve(runtimeInstances->length() + 1)) {
      return false;
    }

    // To avoid implementing rollback, do not fail after mutations start.

    InstanceComparator cmp(instance);
    size_t index;

    // The following section is not unsafe, but simulated OOM do not consider
    // the fact that these insert calls are guarded by the previous reserve
    // calls.
    AutoEnterOOMUnsafeRegion oomUnsafe;
    (void)oomUnsafe;

    MOZ_ALWAYS_FALSE(
        BinarySearchIf(instances_, 0, instances_.length(), cmp, &index));
    MOZ_ALWAYS_TRUE(instances_.insert(instances_.begin() + index, &instance));

    MOZ_ALWAYS_FALSE(BinarySearchIf(runtimeInstances.get(), 0,
                                    runtimeInstances->length(), cmp, &index));
    MOZ_ALWAYS_TRUE(
        runtimeInstances->insert(runtimeInstances->begin() + index, &instance));
  }

  // Notify the debugger after wasmInstances is unlocked.
  DebugAPI::onNewWasmInstance(cx, instanceObj);
  return true;
}

void wasm::Realm::unregisterInstance(Instance& instance) {
  InstanceComparator cmp(instance);
  size_t index;

  if (BinarySearchIf(instances_, 0, instances_.length(), cmp, &index)) {
    instances_.erase(instances_.begin() + index);
  }

  auto runtimeInstances = runtime_->wasmInstances.lock();
  if (BinarySearchIf(runtimeInstances.get(), 0, runtimeInstances->length(), cmp,
                     &index)) {
    runtimeInstances->erase(runtimeInstances->begin() + index);
  }
}

void wasm::Realm::ensureProfilingLabels(bool profilingEnabled) {
  for (Instance* instance : instances_) {
    instance->ensureProfilingLabels(profilingEnabled);
  }
}

void wasm::Realm::addSizeOfExcludingThis(MallocSizeOf mallocSizeOf,
                                         size_t* realmTables) {
  *realmTables += instances_.sizeOfExcludingThis(mallocSizeOf);
}

void wasm::InterruptRunningCode(JSContext* cx) {
  auto runtimeInstances = cx->runtime()->wasmInstances.lock();
  for (Instance* instance : runtimeInstances.get()) {
    instance->setInterrupt();
  }
}

void wasm::ResetInterruptState(JSContext* cx) {
  auto runtimeInstances = cx->runtime()->wasmInstances.lock();
  for (Instance* instance : runtimeInstances.get()) {
    instance->resetInterrupt(cx);
  }
}
