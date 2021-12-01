/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_TypePolicy_h
#define jit_TypePolicy_h

#include "mozilla/TypeTraits.h"

#include "jit/IonTypes.h"
#include "jit/JitAllocPolicy.h"

namespace js {
namespace jit {

class MInstruction;
class MDefinition;

extern MDefinition*
AlwaysBoxAt(TempAllocator& alloc, MInstruction* at, MDefinition* operand);

// A type policy directs the type analysis phases, which insert conversion,
// boxing, unboxing, and type changes as necessary.
class TypePolicy
{
  public:
    // Analyze the inputs of the instruction and perform one of the following
    // actions for each input:
    //  * Nothing; the input already type-checks.
    //  * If untyped, optionally ask the input to try and specialize its value.
    //  * Replace the operand with a conversion instruction.
    //  * Insert an unconditional deoptimization (no conversion possible).
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) = 0;
};

struct TypeSpecializationData
{
  protected:
    // Specifies three levels of specialization:
    //  - < Value. This input is expected and required.
    //  - == None. This op should not be specialized.
    MIRType specialization_;

    MIRType thisTypeSpecialization() {
        return specialization_;
    }

  public:
    MIRType specialization() const {
        return specialization_;
    }
};

#define EMPTY_DATA_                                     \
    struct Data                                         \
    {                                                   \
        static TypePolicy* thisTypePolicy();            \
    }

#define INHERIT_DATA_(DATA_TYPE)                        \
    struct Data : public DATA_TYPE                      \
    {                                                   \
        static TypePolicy* thisTypePolicy();            \
    }

#define SPECIALIZATION_DATA_ INHERIT_DATA_(TypeSpecializationData)

class NoTypePolicy
{
  public:
    struct Data
    {
        static TypePolicy* thisTypePolicy() {
            return nullptr;
        }
    };
};

class BoxInputsPolicy final : public TypePolicy
{
  public:
    SPECIALIZATION_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

class ArithPolicy final : public TypePolicy
{
  public:
    SPECIALIZATION_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override;
};

class AllDoublePolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

class BitwisePolicy final : public TypePolicy
{
  public:
    SPECIALIZATION_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override;
};

class ComparePolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override;
};

class SameValuePolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override;
};

// Policy for MTest instructions.
class TestPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

class TypeBarrierPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

class CallPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override;
};

// Policy for MPow. First operand Double; second Double or Int32.
class PowPolicy final : public TypePolicy
{
  public:
    SPECIALIZATION_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

// Expect a string for operand Op. If the input is a Value, it is unboxed.
template <unsigned Op>
class StringPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

// Expect a string for operand Op. Else a ToString instruction is inserted.
template <unsigned Op>
class ConvertToStringPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

// Expect an Boolean for operand Op. If the input is a Value, it is unboxed.
template <unsigned Op>
class BooleanPolicy final : private TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

// Expects either an Int32 or a boxed Int32 for operand Op; may unbox if needed.
template <unsigned Op>
class UnboxedInt32Policy final : private TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

// Expect an Int for operand Op. Else a ToInt32 instruction is inserted.
template <unsigned Op>
class ConvertToInt32Policy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

// Expect an Int for operand Op. Else a TruncateToInt32 instruction is inserted.
template <unsigned Op>
class TruncateToInt32Policy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

// Expect a double for operand Op. If the input is a Value, it is unboxed.
template <unsigned Op>
class DoublePolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

// Expect a float32 for operand Op. If the input is a Value, it is unboxed.
template <unsigned Op>
class Float32Policy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

// Expect a float32 OR a double for operand Op, but will prioritize Float32
// if the result type is set as such. If the input is a Value, it is unboxed.
template <unsigned Op>
class FloatingPointPolicy final : public TypePolicy
{
  public:
    SPECIALIZATION_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override;
};

template <unsigned Op>
class NoFloatPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

// Policy for guarding variadic instructions such as object / array state
// instructions.
template <unsigned FirstOp>
class NoFloatPolicyAfter final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

// Box objects or strings as an input to a ToDouble instruction.
class ToDoublePolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

// Box objects, strings and undefined as input to a ToInt32 instruction.
class ToInt32Policy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

// Box objects as input to a ToString instruction.
class ToStringPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

template <unsigned Op>
class ObjectPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override {
        return staticAdjustInputs(alloc, ins);
    }
};

// Single-object input. If the input is a Value, it is unboxed. If it is
// a primitive, we use ValueToNonNullObject.
typedef ObjectPolicy<0> SingleObjectPolicy;

// Convert an operand to have a type identical to the scalar type of the
// returned type of the instruction.
template <unsigned Op>
class SimdScalarPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override {
        return staticAdjustInputs(alloc, def);
    }
};

class SimdAllPolicy final : public TypePolicy
{
  public:
    SPECIALIZATION_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

template <unsigned Op>
class SimdPolicy final : public TypePolicy
{
  public:
    SPECIALIZATION_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

class SimdSelectPolicy final : public TypePolicy
{
  public:
    SPECIALIZATION_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

class SimdShufflePolicy final : public TypePolicy
{
  public:
    SPECIALIZATION_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

// SIMD value-type policy, use the returned type of the instruction to determine
// how to unbox its operand.
template <unsigned Op>
class SimdSameAsReturnedTypePolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override {
        return staticAdjustInputs(alloc, ins);
    }
};

template <unsigned Op>
class BoxPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override {
        return staticAdjustInputs(alloc, ins);
    }
};

// Boxes everything except inputs of type Type.
template <unsigned Op, MIRType Type>
class BoxExceptPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
    MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override {
        return staticAdjustInputs(alloc, ins);
    }
};

// Box if not a typical property id (string, symbol, int32).
template <unsigned Op>
class CacheIdPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
    MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override {
        return staticAdjustInputs(alloc, ins);
    }
};

// Combine multiple policies.
template <class... Policies>
class MixPolicy final : public TypePolicy
{
    template <class P>
    static bool staticAdjustInputsHelper(TempAllocator& alloc, MInstruction* ins) {
        return P::staticAdjustInputs(alloc, ins);
    }

    template <class P, class... Rest>
    static typename mozilla::EnableIf<(sizeof...(Rest) > 0), bool>::Type
    staticAdjustInputsHelper(TempAllocator& alloc, MInstruction* ins) {
        return P::staticAdjustInputs(alloc, ins) &&
               MixPolicy::staticAdjustInputsHelper<Rest...>(alloc, ins);
    }

  public:
    EMPTY_DATA_;
    static MOZ_MUST_USE bool staticAdjustInputs(TempAllocator& alloc, MInstruction* ins) {
        return MixPolicy::staticAdjustInputsHelper<Policies...>(alloc, ins);
    }
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override {
        return staticAdjustInputs(alloc, ins);
    }
};

class CallSetElementPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override;
};

// First operand will be boxed to a Value (except for an object)
// Second operand (if specified) will forcefully be unboxed to an object
class InstanceOfPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override;
};

class StoreTypedArrayHolePolicy;

class StoreUnboxedScalarPolicy : public TypePolicy
{
  private:
    static MOZ_MUST_USE bool adjustValueInput(TempAllocator& alloc, MInstruction* ins,
                                              Scalar::Type arrayType, MDefinition* value,
                                              int valueOperand);

    friend class StoreTypedArrayHolePolicy;

  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

class StoreTypedArrayHolePolicy final : public StoreUnboxedScalarPolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

class StoreUnboxedObjectOrNullPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override;
};

class StoreUnboxedStringPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* def) override;
};

// Accepts integers and doubles. Everything else is boxed.
class ClampPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

class FilterTypeSetPolicy final : public TypePolicy
{
  public:
    EMPTY_DATA_;
    virtual MOZ_MUST_USE bool adjustInputs(TempAllocator& alloc, MInstruction* ins) override;
};

#undef SPECIALIZATION_DATA_
#undef INHERIT_DATA_
#undef EMPTY_DATA_

} // namespace jit
} // namespace js

#endif /* jit_TypePolicy_h */
