/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_none_LIR_none_h
#define jit_none_LIR_none_h

namespace js {
namespace jit {

class LUnboxFloatingPoint : public LInstruction
{
  public:
    static const size_t Input = 0;

    MUnbox* mir() const { MOZ_CRASH(); }

    const LDefinition* output() const { MOZ_CRASH(); }
    MIRType type() const { MOZ_CRASH(); }
};

class LTableSwitch : public LInstruction
{
  public:
    MTableSwitch* mir() { MOZ_CRASH(); }

    const LAllocation* index() { MOZ_CRASH(); }
    const LDefinition* tempInt() { MOZ_CRASH(); }
    const LDefinition* tempPointer() { MOZ_CRASH(); }
};

class LTableSwitchV : public LInstruction
{
  public:
    MTableSwitch* mir() { MOZ_CRASH(); }

    const LDefinition* tempInt() { MOZ_CRASH(); }
    const LDefinition* tempFloat() { MOZ_CRASH(); }
    const LDefinition* tempPointer() { MOZ_CRASH(); }

    static const size_t InputValue = 0;
};

class LGuardShape : public LInstruction {};
class LGuardObjectGroup : public LInstruction {};
class LMulI : public LInstruction {};

} // namespace jit
} // namespace js

#endif /* jit_none_LIR_none_h */
