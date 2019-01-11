/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

#ifndef wasm_compartment_h
#define wasm_compartment_h

#include "wasm/WasmJS.h"

namespace js {
namespace wasm {

typedef Vector<Instance*, 0, SystemAllocPolicy> InstanceVector;

// wasm::Compartment lives in JSCompartment and contains the wasm-related
// per-compartment state. wasm::Compartment tracks every live instance in the
// compartment and must be notified, via registerInstance(), of any new
// WasmInstanceObject.

class Compartment
{
    InstanceVector instances_;

  public:
    explicit Compartment(Zone* zone);
    ~Compartment();

    // Before a WasmInstanceObject can be considered fully constructed and
    // valid, it must be registered with the Compartment. If this method fails,
    // an error has been reported and the instance object must be abandoned.
    // After a successful registration, an Instance must call
    // unregisterInstance() before being destroyed.

    bool registerInstance(JSContext* cx, HandleWasmInstanceObject instanceObj);
    void unregisterInstance(Instance& instance);

    // Return a vector of all live instances in the compartment. The lifetime of
    // these Instances is determined by their owning WasmInstanceObject.
    // Note that accessing instances()[i]->object() triggers a read barrier
    // since instances() is effectively a weak list.

    const InstanceVector& instances() const { return instances_; }

    // Ensure all Instances in this JSCompartment have profiling labels created.

    void ensureProfilingLabels(bool profilingEnabled);

    // about:memory reporting

    void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf, size_t* compartmentTables);
};

} // namespace wasm
} // namespace js

#endif // wasm_compartment_h
