/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonInstrumentatjit_h
#define jit_IonInstrumentatjit_h

namespace js {

class GeckoProfilerRuntime;

namespace jit {

class MacroAssembler;

typedef GeckoProfilerInstrumentation<MacroAssembler, Register> BaseInstrumentation;

class IonInstrumentation : public BaseInstrumentation
{
  public:
    IonInstrumentation(GeckoProfilerRuntime* profiler, jsbytecode** pc)
      : BaseInstrumentation(profiler)
    {
        MOZ_ASSERT(pc != nullptr);
    }
};

} // namespace jit
} // namespace js

#endif /* jit_IonInstrumentatjit_h */
