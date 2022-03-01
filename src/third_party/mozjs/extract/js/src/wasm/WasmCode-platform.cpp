/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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

#include "wasm/WasmCode.h"

#ifdef _MSC_VER
#  include <windef.h>  // For InterlockedCompareExchange64
#endif

namespace js {
namespace wasm {

void JumpTables::setJitEntryIfNull(size_t i, void* target) const {
  // Make sure that compare-and-write is atomic; see comment in
  // wasm::Module::finishTier2 to that effect.
  MOZ_ASSERT(i < numFuncs_);

  void* expected = nullptr;
#ifdef _MSC_VER
  (void)InterlockedCompareExchangePointer(&jit_.get()[i], target, expected);
#else
  (void)__atomic_compare_exchange_n(&jit_.get()[i], &expected, target,
                                    /*weak=*/false, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED);
#endif
}

}  // namespace wasm
}  // namespace js
