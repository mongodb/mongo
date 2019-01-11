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

class LWasmUint32ToFloat32 : public LInstructionHelper<1, 1, 0>
{
  public:
    explicit LWasmUint32ToFloat32(const LAllocation&)
      : LInstructionHelper(LOp_Invalid)
    {
        MOZ_CRASH();
    }
};

class LUnbox : public LInstructionHelper<1, 2, 0>
{
  public:

    MUnbox* mir() const { MOZ_CRASH(); }
    const LAllocation* payload() { MOZ_CRASH(); }
    const LAllocation* type() { MOZ_CRASH(); }
    const char* extraName() const { MOZ_CRASH(); }
};
class LDivI : public LBinaryMath<1>
{
  public:
    LDivI(const LAllocation& , const LAllocation& ,
          const LDefinition& )
      : LBinaryMath(LOp_Invalid)
    {
        MOZ_CRASH();
    }
    MDiv* mir() const { MOZ_CRASH(); }
};
class LDivPowTwoI : public LInstructionHelper<1, 1, 0>
{
  public:
    LDivPowTwoI(const LAllocation&, int32_t) : LInstructionHelper(LOp_Invalid) { MOZ_CRASH(); }
    const LAllocation* numerator() { MOZ_CRASH(); }
    int32_t shift() { MOZ_CRASH(); }
    MDiv* mir() const { MOZ_CRASH(); }
};
class LModI : public LBinaryMath<1>
{
  public:
    LModI(const LAllocation&, const LAllocation&,
          const LDefinition&)
      : LBinaryMath(LOp_Invalid)
    {
        MOZ_CRASH();
    }

    const LDefinition* callTemp() { MOZ_CRASH(); }
    MMod* mir() const { MOZ_CRASH(); }
};
class LWasmUint32ToDouble : public LInstructionHelper<1, 1, 0>
{
  public:
    explicit LWasmUint32ToDouble(const LAllocation&)
      : LInstructionHelper(LOp_Invalid)
    {
        MOZ_CRASH();
    }
};
class LModPowTwoI : public LInstructionHelper<1, 1, 0>
{

  public:
    int32_t shift() { MOZ_CRASH(); }
    LModPowTwoI(const LAllocation& lhs, int32_t shift)
      : LInstructionHelper(LOp_Invalid)
    {
        MOZ_CRASH();
    }
    MMod* mir() const { MOZ_CRASH(); }
};

class LMulI : public LInstruction {};

} // namespace jit
} // namespace js

#endif /* jit_none_LIR_none_h */
