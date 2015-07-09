/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MacroAssembler_h
#define jit_MacroAssembler_h

#include "mozilla/MathAlgorithms.h"

#include "jscompartment.h"

#if defined(JS_CODEGEN_X86)
# include "jit/x86/MacroAssembler-x86.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/MacroAssembler-x64.h"
#elif defined(JS_CODEGEN_ARM)
# include "jit/arm/MacroAssembler-arm.h"
#elif defined(JS_CODEGEN_MIPS)
# include "jit/mips/MacroAssembler-mips.h"
#elif defined(JS_CODEGEN_NONE)
# include "jit/none/MacroAssembler-none.h"
#else
# error "Unknown architecture!"
#endif
#include "jit/AtomicOp.h"
#include "jit/IonInstrumentation.h"
#include "jit/JitCompartment.h"
#include "jit/VMFunctions.h"
#include "vm/ProxyObject.h"
#include "vm/Shape.h"

#ifdef IS_LITTLE_ENDIAN
#define IMM32_16ADJ(X) X << 16
#else
#define IMM32_16ADJ(X) X
#endif

namespace js {
namespace jit {

// The public entrypoint for emitting assembly. Note that a MacroAssembler can
// use cx->lifoAlloc, so take care not to interleave masm use with other
// lifoAlloc use if one will be destroyed before the other.
class MacroAssembler : public MacroAssemblerSpecific
{
    MacroAssembler* thisFromCtor() {
        return this;
    }

  public:
    class AutoRooter : public JS::AutoGCRooter
    {
        MacroAssembler* masm_;

      public:
        AutoRooter(JSContext* cx, MacroAssembler* masm)
          : JS::AutoGCRooter(cx, IONMASM),
            masm_(masm)
        { }

        MacroAssembler* masm() const {
            return masm_;
        }
    };

    /*
     * Base class for creating a branch.
     */
    class Branch
    {
        bool init_;
        Condition cond_;
        Label* jump_;
        Register reg_;

      public:
        Branch()
          : init_(false),
            cond_(Equal),
            jump_(nullptr),
            reg_(Register::FromCode(0))      // Quell compiler warnings.
        { }

        Branch(Condition cond, Register reg, Label* jump)
          : init_(true),
            cond_(cond),
            jump_(jump),
            reg_(reg)
        { }

        bool isInitialized() const {
            return init_;
        }

        Condition cond() const {
            return cond_;
        }

        Label* jump() const {
            return jump_;
        }

        Register reg() const {
            return reg_;
        }

        void invertCondition() {
            cond_ = InvertCondition(cond_);
        }

        void relink(Label* jump) {
            jump_ = jump;
        }

        virtual void emit(MacroAssembler& masm) = 0;
    };

    /*
     * Creates a branch based on a specific TypeSet::Type.
     * Note: emits number test (int/double) for TypeSet::DoubleType()
     */
    class BranchType : public Branch
    {
        TypeSet::Type type_;

      public:
        BranchType()
          : Branch(),
            type_(TypeSet::UnknownType())
        { }

        BranchType(Condition cond, Register reg, TypeSet::Type type, Label* jump)
          : Branch(cond, reg, jump),
            type_(type)
        { }

        void emit(MacroAssembler& masm) {
            MOZ_ASSERT(isInitialized());
            MIRType mirType = MIRType_None;

            if (type_.isPrimitive()) {
                if (type_.isMagicArguments())
                    mirType = MIRType_MagicOptimizedArguments;
                else
                    mirType = MIRTypeFromValueType(type_.primitive());
            } else if (type_.isAnyObject()) {
                mirType = MIRType_Object;
            } else {
                MOZ_CRASH("Unknown conversion to mirtype");
            }

            if (mirType == MIRType_Double)
                masm.branchTestNumber(cond(), reg(), jump());
            else
                masm.branchTestMIRType(cond(), reg(), mirType, jump());
        }

    };

    /*
     * Creates a branch based on a GCPtr.
     */
    class BranchGCPtr : public Branch
    {
        ImmGCPtr ptr_;

      public:
        BranchGCPtr()
          : Branch(),
            ptr_(ImmGCPtr(nullptr))
        { }

        BranchGCPtr(Condition cond, Register reg, ImmGCPtr ptr, Label* jump)
          : Branch(cond, reg, jump),
            ptr_(ptr)
        { }

        void emit(MacroAssembler& masm) {
            MOZ_ASSERT(isInitialized());
            masm.branchPtr(cond(), reg(), ptr_, jump());
        }
    };

    mozilla::Maybe<AutoRooter> autoRooter_;
    mozilla::Maybe<JitContext> jitContext_;
    mozilla::Maybe<AutoJitContextAlloc> alloc_;

  private:
    // This field is used to manage profiling instrumentation output. If
    // provided and enabled, then instrumentation will be emitted around call
    // sites.
    bool emitProfilingInstrumentation_;

    // Labels for handling exceptions and failures.
    NonAssertingLabel failureLabel_;

  public:
    MacroAssembler()
      : emitProfilingInstrumentation_(false)
    {
        JitContext* jcx = GetJitContext();
        JSContext* cx = jcx->cx;
        if (cx)
            constructRoot(cx);

        if (!jcx->temp) {
            MOZ_ASSERT(cx);
            alloc_.emplace(cx);
        }

        moveResolver_.setAllocator(*jcx->temp);
#ifdef JS_CODEGEN_ARM
        initWithAllocator();
        m_buffer.id = jcx->getNextAssemblerId();
#endif
    }

    // This constructor should only be used when there is no JitContext active
    // (for example, Trampoline-$(ARCH).cpp and IonCaches.cpp).
    explicit MacroAssembler(JSContext* cx, IonScript* ion = nullptr,
                            JSScript* script = nullptr, jsbytecode* pc = nullptr)
      : emitProfilingInstrumentation_(false)
    {
        constructRoot(cx);
        jitContext_.emplace(cx, (js::jit::TempAllocator*)nullptr);
        alloc_.emplace(cx);
        moveResolver_.setAllocator(*jitContext_->temp);
#ifdef JS_CODEGEN_ARM
        initWithAllocator();
        m_buffer.id = GetJitContext()->getNextAssemblerId();
#endif
        if (ion) {
            setFramePushed(ion->frameSize());
            if (pc && cx->runtime()->spsProfiler.enabled())
                emitProfilingInstrumentation_ = true;
        }
    }

    // asm.js compilation handles its own JitContext-pushing
    struct AsmJSToken {};
    explicit MacroAssembler(AsmJSToken)
      : emitProfilingInstrumentation_(false)
    {
#ifdef JS_CODEGEN_ARM
        initWithAllocator();
        m_buffer.id = 0;
#endif
    }

    void enableProfilingInstrumentation() {
        emitProfilingInstrumentation_ = true;
    }

    void resetForNewCodeGenerator(TempAllocator& alloc) {
        setFramePushed(0);
        moveResolver_.clearTempObjectPool();
        moveResolver_.setAllocator(alloc);
    }

    void constructRoot(JSContext* cx) {
        autoRooter_.emplace(cx, this);
    }

    MoveResolver& moveResolver() {
        return moveResolver_;
    }

    size_t instructionsSize() const {
        return size();
    }

    // Emits a test of a value against all types in a TypeSet. A scratch
    // register is required.
    template <typename Source, typename TypeSet>
    void guardTypeSet(const Source& address, const TypeSet* types, BarrierKind kind, Register scratch, Label* miss);
    template <typename TypeSet>
    void guardObjectType(Register obj, const TypeSet* types, Register scratch, Label* miss);
    template <typename Source>
    void guardType(const Source& address, TypeSet::Type type, Register scratch, Label* miss);

    void loadObjShape(Register objReg, Register dest) {
        loadPtr(Address(objReg, JSObject::offsetOfShape()), dest);
    }
    void loadBaseShape(Register objReg, Register dest) {
        loadPtr(Address(objReg, JSObject::offsetOfShape()), dest);

        loadPtr(Address(dest, Shape::offsetOfBase()), dest);
    }
    void loadObjClass(Register objReg, Register dest) {
        loadPtr(Address(objReg, JSObject::offsetOfGroup()), dest);
        loadPtr(Address(dest, ObjectGroup::offsetOfClasp()), dest);
    }
    void branchTestObjClass(Condition cond, Register obj, Register scratch, const js::Class* clasp,
                            Label* label) {
        loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);
        branchPtr(cond, Address(scratch, ObjectGroup::offsetOfClasp()), ImmPtr(clasp), label);
    }
    void branchTestObjShape(Condition cond, Register obj, const Shape* shape, Label* label) {
        branchPtr(cond, Address(obj, JSObject::offsetOfShape()), ImmGCPtr(shape), label);
    }
    void branchTestObjShape(Condition cond, Register obj, Register shape, Label* label) {
        branchPtr(cond, Address(obj, JSObject::offsetOfShape()), shape, label);
    }
    void branchTestProxyHandlerFamily(Condition cond, Register proxy, Register scratch,
                                      const void* handlerp, Label* label) {
        Address handlerAddr(proxy, ProxyObject::offsetOfHandler());
        loadPtr(handlerAddr, scratch);
        Address familyAddr(scratch, BaseProxyHandler::offsetOfFamily());
        branchPtr(cond, familyAddr, ImmPtr(handlerp), label);
    }

    template <typename Value>
    void branchTestMIRType(Condition cond, const Value& val, MIRType type, Label* label) {
        switch (type) {
          case MIRType_Null:      return branchTestNull(cond, val, label);
          case MIRType_Undefined: return branchTestUndefined(cond, val, label);
          case MIRType_Boolean:   return branchTestBoolean(cond, val, label);
          case MIRType_Int32:     return branchTestInt32(cond, val, label);
          case MIRType_String:    return branchTestString(cond, val, label);
          case MIRType_Symbol:    return branchTestSymbol(cond, val, label);
          case MIRType_Object:    return branchTestObject(cond, val, label);
          case MIRType_Double:    return branchTestDouble(cond, val, label);
          case MIRType_MagicOptimizedArguments: // Fall through.
          case MIRType_MagicIsConstructing:
          case MIRType_MagicHole: return branchTestMagic(cond, val, label);
          default:
            MOZ_CRASH("Bad MIRType");
        }
    }

    // Branches to |label| if |reg| is false. |reg| should be a C++ bool.
    void branchIfFalseBool(Register reg, Label* label) {
        // Note that C++ bool is only 1 byte, so ignore the higher-order bits.
        branchTest32(Assembler::Zero, reg, Imm32(0xFF), label);
    }

    // Branches to |label| if |reg| is true. |reg| should be a C++ bool.
    void branchIfTrueBool(Register reg, Label* label) {
        // Note that C++ bool is only 1 byte, so ignore the higher-order bits.
        branchTest32(Assembler::NonZero, reg, Imm32(0xFF), label);
    }

    void loadObjPrivate(Register obj, uint32_t nfixed, Register dest) {
        loadPtr(Address(obj, NativeObject::getPrivateDataOffset(nfixed)), dest);
    }

    void loadObjProto(Register obj, Register dest) {
        loadPtr(Address(obj, JSObject::offsetOfGroup()), dest);
        loadPtr(Address(dest, ObjectGroup::offsetOfProto()), dest);
    }

    void loadStringLength(Register str, Register dest) {
        load32(Address(str, JSString::offsetOfLength()), dest);
    }

    void loadFunctionFromCalleeToken(Address token, Register dest) {
        loadPtr(token, dest);
        andPtr(Imm32(uint32_t(CalleeTokenMask)), dest);
    }
    void PushCalleeToken(Register callee, bool constructing) {
        if (constructing) {
            orPtr(Imm32(CalleeToken_FunctionConstructing), callee);
            Push(callee);
            andPtr(Imm32(uint32_t(CalleeTokenMask)), callee);
        } else {
            static_assert(CalleeToken_Function == 0, "Non-constructing call requires no tagging");
            Push(callee);
        }
    }

    void loadStringChars(Register str, Register dest);
    void loadStringChar(Register str, Register index, Register output);

    void branchIfRope(Register str, Label* label) {
        Address flags(str, JSString::offsetOfFlags());
        static_assert(JSString::ROPE_FLAGS == 0, "Rope type flags must be 0");
        branchTest32(Assembler::Zero, flags, Imm32(JSString::TYPE_FLAGS_MASK), label);
    }

    void loadJSContext(Register dest) {
        loadPtr(AbsoluteAddress(GetJitContext()->runtime->addressOfJSContext()), dest);
    }
    void loadJitActivation(Register dest) {
        loadPtr(AbsoluteAddress(GetJitContext()->runtime->addressOfActivation()), dest);
    }

    template<typename T>
    void loadTypedOrValue(const T& src, TypedOrValueRegister dest) {
        if (dest.hasValue())
            loadValue(src, dest.valueReg());
        else
            loadUnboxedValue(src, dest.type(), dest.typedReg());
    }

    template<typename T>
    void loadElementTypedOrValue(const T& src, TypedOrValueRegister dest, bool holeCheck,
                                 Label* hole) {
        if (dest.hasValue()) {
            loadValue(src, dest.valueReg());
            if (holeCheck)
                branchTestMagic(Assembler::Equal, dest.valueReg(), hole);
        } else {
            if (holeCheck)
                branchTestMagic(Assembler::Equal, src, hole);
            loadUnboxedValue(src, dest.type(), dest.typedReg());
        }
    }

    template <typename T>
    void storeTypedOrValue(TypedOrValueRegister src, const T& dest) {
        if (src.hasValue()) {
            storeValue(src.valueReg(), dest);
        } else if (IsFloatingPointType(src.type())) {
            FloatRegister reg = src.typedReg().fpu();
            if (src.type() == MIRType_Float32) {
                convertFloat32ToDouble(reg, ScratchDoubleReg);
                reg = ScratchDoubleReg;
            }
            storeDouble(reg, dest);
        } else {
            storeValue(ValueTypeFromMIRType(src.type()), src.typedReg().gpr(), dest);
        }
    }

    template <typename T>
    void storeObjectOrNull(Register src, const T& dest) {
        Label notNull, done;
        branchTestPtr(Assembler::NonZero, src, src, &notNull);
        storeValue(NullValue(), dest);
        jump(&done);
        bind(&notNull);
        storeValue(JSVAL_TYPE_OBJECT, src, dest);
        bind(&done);
    }

    template <typename T>
    void storeConstantOrRegister(ConstantOrRegister src, const T& dest) {
        if (src.constant())
            storeValue(src.value(), dest);
        else
            storeTypedOrValue(src.reg(), dest);
    }

    void storeCallResult(Register reg) {
        if (reg != ReturnReg)
            mov(ReturnReg, reg);
    }

    void storeCallFloatResult(FloatRegister reg) {
        if (reg != ReturnDoubleReg)
            moveDouble(ReturnDoubleReg, reg);
    }

    void storeCallResultValue(AnyRegister dest) {
#if defined(JS_NUNBOX32)
        unboxValue(ValueOperand(JSReturnReg_Type, JSReturnReg_Data), dest);
#elif defined(JS_PUNBOX64)
        unboxValue(ValueOperand(JSReturnReg), dest);
#else
#error "Bad architecture"
#endif
    }

    void storeCallResultValue(ValueOperand dest) {
#if defined(JS_NUNBOX32)
        // reshuffle the return registers used for a call result to store into
        // dest, using ReturnReg as a scratch register if necessary. This must
        // only be called after returning from a call, at a point when the
        // return register is not live. XXX would be better to allow wrappers
        // to store the return value to different places.
        if (dest.typeReg() == JSReturnReg_Data) {
            if (dest.payloadReg() == JSReturnReg_Type) {
                // swap the two registers.
                mov(JSReturnReg_Type, ReturnReg);
                mov(JSReturnReg_Data, JSReturnReg_Type);
                mov(ReturnReg, JSReturnReg_Data);
            } else {
                mov(JSReturnReg_Data, dest.payloadReg());
                mov(JSReturnReg_Type, dest.typeReg());
            }
        } else {
            mov(JSReturnReg_Type, dest.typeReg());
            mov(JSReturnReg_Data, dest.payloadReg());
        }
#elif defined(JS_PUNBOX64)
        if (dest.valueReg() != JSReturnReg)
            movq(JSReturnReg, dest.valueReg());
#else
#error "Bad architecture"
#endif
    }

    void storeCallResultValue(TypedOrValueRegister dest) {
        if (dest.hasValue())
            storeCallResultValue(dest.valueReg());
        else
            storeCallResultValue(dest.typedReg());
    }

    template <typename T>
    Register extractString(const T& source, Register scratch) {
        return extractObject(source, scratch);
    }

    void PushRegsInMask(RegisterSet set, FloatRegisterSet simdSet);
    void PushRegsInMask(RegisterSet set) {
        PushRegsInMask(set, FloatRegisterSet());
    }
    void PushRegsInMask(GeneralRegisterSet set) {
        PushRegsInMask(RegisterSet(set, FloatRegisterSet()));
    }
    void PopRegsInMask(RegisterSet set) {
        PopRegsInMaskIgnore(set, RegisterSet());
    }
    void PopRegsInMask(RegisterSet set, FloatRegisterSet simdSet) {
        PopRegsInMaskIgnore(set, RegisterSet(), simdSet);
    }
    void PopRegsInMask(GeneralRegisterSet set) {
        PopRegsInMask(RegisterSet(set, FloatRegisterSet()));
    }
    void PopRegsInMaskIgnore(RegisterSet set, RegisterSet ignore, FloatRegisterSet simdSet);
    void PopRegsInMaskIgnore(RegisterSet set, RegisterSet ignore) {
        PopRegsInMaskIgnore(set, ignore, FloatRegisterSet());
    }

    void branchIfFunctionHasNoScript(Register fun, Label* label) {
        // 16-bit loads are slow and unaligned 32-bit loads may be too so
        // perform an aligned 32-bit load and adjust the bitmask accordingly.
        MOZ_ASSERT(JSFunction::offsetOfNargs() % sizeof(uint32_t) == 0);
        MOZ_ASSERT(JSFunction::offsetOfFlags() == JSFunction::offsetOfNargs() + 2);
        Address address(fun, JSFunction::offsetOfNargs());
        int32_t bit = IMM32_16ADJ(JSFunction::INTERPRETED);
        branchTest32(Assembler::Zero, address, Imm32(bit), label);
    }
    void branchIfInterpreted(Register fun, Label* label) {
        // 16-bit loads are slow and unaligned 32-bit loads may be too so
        // perform an aligned 32-bit load and adjust the bitmask accordingly.
        MOZ_ASSERT(JSFunction::offsetOfNargs() % sizeof(uint32_t) == 0);
        MOZ_ASSERT(JSFunction::offsetOfFlags() == JSFunction::offsetOfNargs() + 2);
        Address address(fun, JSFunction::offsetOfNargs());
        int32_t bit = IMM32_16ADJ(JSFunction::INTERPRETED);
        branchTest32(Assembler::NonZero, address, Imm32(bit), label);
    }

    void branchIfNotInterpretedConstructor(Register fun, Register scratch, Label* label);

    using MacroAssemblerSpecific::Push;
    using MacroAssemblerSpecific::Pop;

    void Push(jsid id, Register scratchReg) {
        if (JSID_IS_GCTHING(id)) {
            // If we're pushing a gcthing, then we can't just push the tagged jsid
            // value since the GC won't have any idea that the push instruction
            // carries a reference to a gcthing.  Need to unpack the pointer,
            // push it using ImmGCPtr, and then rematerialize the id at runtime.

            if (JSID_IS_STRING(id)) {
                JSString* str = JSID_TO_STRING(id);
                MOZ_ASSERT(((size_t)str & JSID_TYPE_MASK) == 0);
                MOZ_ASSERT(JSID_TYPE_STRING == 0x0);
                Push(ImmGCPtr(str));
            } else {
                MOZ_ASSERT(JSID_IS_SYMBOL(id));
                JS::Symbol* sym = JSID_TO_SYMBOL(id);
                movePtr(ImmGCPtr(sym), scratchReg);
                orPtr(Imm32(JSID_TYPE_SYMBOL), scratchReg);
                Push(scratchReg);
            }
        } else {
            Push(ImmWord(JSID_BITS(id)));
        }
    }

    void Push(TypedOrValueRegister v) {
        if (v.hasValue()) {
            Push(v.valueReg());
        } else if (IsFloatingPointType(v.type())) {
            FloatRegister reg = v.typedReg().fpu();
            if (v.type() == MIRType_Float32) {
                convertFloat32ToDouble(reg, ScratchDoubleReg);
                reg = ScratchDoubleReg;
            }
            Push(reg);
        } else {
            Push(ValueTypeFromMIRType(v.type()), v.typedReg().gpr());
        }
    }

    void Push(ConstantOrRegister v) {
        if (v.constant())
            Push(v.value());
        else
            Push(v.reg());
    }

    void Push(const ValueOperand& val) {
        pushValue(val);
        framePushed_ += sizeof(Value);
    }

    void Push(const Value& val) {
        pushValue(val);
        framePushed_ += sizeof(Value);
    }

    void Push(JSValueType type, Register reg) {
        pushValue(type, reg);
        framePushed_ += sizeof(Value);
    }

    void PushValue(const Address& addr) {
        MOZ_ASSERT(addr.base != StackPointer);
        pushValue(addr);
        framePushed_ += sizeof(Value);
    }

    void PushEmptyRooted(VMFunction::RootType rootType);
    void popRooted(VMFunction::RootType rootType, Register cellReg, const ValueOperand& valueReg);

    void adjustStack(int amount) {
        if (amount > 0)
            freeStack(amount);
        else if (amount < 0)
            reserveStack(-amount);
    }

    void bumpKey(Int32Key* key, int diff) {
        if (key->isRegister())
            add32(Imm32(diff), key->reg());
        else
            key->bumpConstant(diff);
    }

    void storeKey(const Int32Key& key, const Address& dest) {
        if (key.isRegister())
            store32(key.reg(), dest);
        else
            store32(Imm32(key.constant()), dest);
    }

    template<typename T>
    void branchKey(Condition cond, const T& length, const Int32Key& key, Label* label) {
        if (key.isRegister())
            branch32(cond, length, key.reg(), label);
        else
            branch32(cond, length, Imm32(key.constant()), label);
    }

    void branchTestNeedsIncrementalBarrier(Condition cond, Label* label) {
        MOZ_ASSERT(cond == Zero || cond == NonZero);
        CompileZone* zone = GetJitContext()->compartment->zone();
        AbsoluteAddress needsBarrierAddr(zone->addressOfNeedsIncrementalBarrier());
        branchTest32(cond, needsBarrierAddr, Imm32(0x1), label);
    }

    template <typename T>
    void callPreBarrier(const T& address, MIRType type) {
        Label done;

        if (type == MIRType_Value)
            branchTestGCThing(Assembler::NotEqual, address, &done);

        Push(PreBarrierReg);
        computeEffectiveAddress(address, PreBarrierReg);

        const JitRuntime* rt = GetJitContext()->runtime->jitRuntime();
        JitCode* preBarrier = rt->preBarrier(type);

        call(preBarrier);
        Pop(PreBarrierReg);

        bind(&done);
    }

    template <typename T>
    void patchableCallPreBarrier(const T& address, MIRType type) {
        Label done;

        // All barriers are off by default.
        // They are enabled if necessary at the end of CodeGenerator::generate().
        CodeOffsetLabel nopJump = toggledJump(&done);
        writePrebarrierOffset(nopJump);

        callPreBarrier(address, type);
        jump(&done);

        align(8);
        bind(&done);
    }

    void canonicalizeDouble(FloatRegister reg) {
        Label notNaN;
        branchDouble(DoubleOrdered, reg, reg, &notNaN);
        loadConstantDouble(JS::GenericNaN(), reg);
        bind(&notNaN);
    }

    void canonicalizeFloat(FloatRegister reg) {
        Label notNaN;
        branchFloat(DoubleOrdered, reg, reg, &notNaN);
        loadConstantFloat32(float(JS::GenericNaN()), reg);
        bind(&notNaN);
    }

    template<typename T>
    void loadFromTypedArray(Scalar::Type arrayType, const T& src, AnyRegister dest, Register temp, Label* fail,
                            bool canonicalizeDoubles = true);

    template<typename T>
    void loadFromTypedArray(Scalar::Type arrayType, const T& src, const ValueOperand& dest, bool allowDouble,
                            Register temp, Label* fail);

    template<typename S, typename T>
    void storeToTypedIntArray(Scalar::Type arrayType, const S& value, const T& dest) {
        switch (arrayType) {
          case Scalar::Int8:
          case Scalar::Uint8:
          case Scalar::Uint8Clamped:
            store8(value, dest);
            break;
          case Scalar::Int16:
          case Scalar::Uint16:
            store16(value, dest);
            break;
          case Scalar::Int32:
          case Scalar::Uint32:
            store32(value, dest);
            break;
          default:
            MOZ_CRASH("Invalid typed array type");
        }
    }

    template<typename T>
    void compareExchangeToTypedIntArray(Scalar::Type arrayType, const T& mem, Register oldval, Register newval,
                                        Register temp, AnyRegister output);

    template<typename S, typename T>
    void atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType, const S& value,
                                    const T& mem, Register temp1, Register temp2, AnyRegister output);

    void storeToTypedFloatArray(Scalar::Type arrayType, FloatRegister value, const BaseIndex& dest);
    void storeToTypedFloatArray(Scalar::Type arrayType, FloatRegister value, const Address& dest);

    // Load a property from an UnboxedPlainObject.
    template <typename T>
    void loadUnboxedProperty(T address, JSValueType type, TypedOrValueRegister output);

    // Store a property to an UnboxedPlainObject, without triggering barriers.
    // If failure is null, the value definitely has a type suitable for storing
    // in the property.
    template <typename T>
    void storeUnboxedProperty(T address, JSValueType type,
                              ConstantOrRegister value, Label* failure);

    Register extractString(const Address& address, Register scratch) {
        return extractObject(address, scratch);
    }
    Register extractString(const ValueOperand& value, Register scratch) {
        return extractObject(value, scratch);
    }

    using MacroAssemblerSpecific::extractTag;
    Register extractTag(const TypedOrValueRegister& reg, Register scratch) {
        if (reg.hasValue())
            return extractTag(reg.valueReg(), scratch);
        mov(ImmWord(MIRTypeToTag(reg.type())), scratch);
        return scratch;
    }

    using MacroAssemblerSpecific::extractObject;
    Register extractObject(const TypedOrValueRegister& reg, Register scratch) {
        if (reg.hasValue())
            return extractObject(reg.valueReg(), scratch);
        MOZ_ASSERT(reg.type() == MIRType_Object);
        return reg.typedReg().gpr();
    }

    // Inline version of js_TypedArray_uint8_clamp_double.
    // This function clobbers the input register.
    void clampDoubleToUint8(FloatRegister input, Register output);

    using MacroAssemblerSpecific::ensureDouble;

    template <typename S>
    void ensureDouble(const S& source, FloatRegister dest, Label* failure) {
        Label isDouble, done;
        branchTestDouble(Assembler::Equal, source, &isDouble);
        branchTestInt32(Assembler::NotEqual, source, failure);

        convertInt32ToDouble(source, dest);
        jump(&done);

        bind(&isDouble);
        unboxDouble(source, dest);

        bind(&done);
    }

    // Emit type case branch on tag matching if the type tag in the definition
    // might actually be that type.
    void branchEqualTypeIfNeeded(MIRType type, MDefinition* maybeDef, Register tag, Label* label);

    // Inline allocation.
  private:
    void checkAllocatorState(Label* fail);
    bool shouldNurseryAllocate(gc::AllocKind allocKind, gc::InitialHeap initialHeap);
    void nurseryAllocate(Register result, Register slots, gc::AllocKind allocKind,
                         size_t nDynamicSlots, gc::InitialHeap initialHeap, Label* fail);
    void freeListAllocate(Register result, Register temp, gc::AllocKind allocKind, Label* fail);
    void allocateObject(Register result, Register slots, gc::AllocKind allocKind,
                        uint32_t nDynamicSlots, gc::InitialHeap initialHeap, Label* fail);
    void allocateNonObject(Register result, Register temp, gc::AllocKind allocKind, Label* fail);
    void copySlotsFromTemplate(Register obj, const NativeObject* templateObj,
                               uint32_t start, uint32_t end);
    void fillSlotsWithConstantValue(Address addr, Register temp, uint32_t start, uint32_t end,
                                    const Value& v);
    void fillSlotsWithUndefined(Address addr, Register temp, uint32_t start, uint32_t end);
    void fillSlotsWithUninitialized(Address addr, Register temp, uint32_t start, uint32_t end);
    void initGCSlots(Register obj, Register temp, NativeObject* templateObj, bool initFixedSlots);

  public:
    void callMallocStub(size_t nbytes, Register result, Label* fail);
    void callFreeStub(Register slots);
    void createGCObject(Register result, Register temp, JSObject* templateObj,
                        gc::InitialHeap initialHeap, Label* fail, bool initFixedSlots = true);

    void newGCThing(Register result, Register temp, JSObject* templateObj,
                     gc::InitialHeap initialHeap, Label* fail);
    void initGCThing(Register obj, Register temp, JSObject* templateObj,
                     bool initFixedSlots = true);

    void newGCString(Register result, Register temp, Label* fail);
    void newGCFatInlineString(Register result, Register temp, Label* fail);

    // Compares two strings for equality based on the JSOP.
    // This checks for identical pointers, atoms and length and fails for everything else.
    void compareStrings(JSOp op, Register left, Register right, Register result,
                        Label* fail);

    // If the JitCode that created this assembler needs to transition into the VM,
    // we want to store the JitCode on the stack in order to mark it during a GC.
    // This is a reference to a patch location where the JitCode* will be written.
  private:
    CodeOffsetLabel exitCodePatch_;

  private:
    void linkExitFrame();

  public:
    void PushStubCode() {
        exitCodePatch_ = PushWithPatch(ImmWord(-1));
    }

    void enterExitFrame(const VMFunction* f = nullptr) {
        linkExitFrame();
        // Push the ioncode. (Bailout or VM wrapper)
        PushStubCode();
        // Push VMFunction pointer, to mark arguments.
        Push(ImmPtr(f));
    }

    // The JitCode * argument here is one of the tokens defined in the various
    // exit frame layout classes, e.g. NativeExitFrameLayout::Token().
    void enterFakeExitFrame(JitCode* codeVal) {
        linkExitFrame();
        Push(ImmPtr(codeVal));
        Push(ImmPtr(nullptr));
    }

    void leaveExitFrame(size_t extraFrame = 0) {
        freeStack(ExitFooterFrame::Size() + extraFrame);
    }

    bool hasEnteredExitFrame() const {
        return exitCodePatch_.offset() != 0;
    }

    // Generates code used to complete a bailout.
    void generateBailoutTail(Register scratch, Register bailoutInfo);

    // These functions exist as small wrappers around sites where execution can
    // leave the currently running stream of instructions. They exist so that
    // instrumentation may be put in place around them if necessary and the
    // instrumentation is enabled. For the functions that return a uint32_t,
    // they are returning the offset of the assembler just after the call has
    // been made so that a safepoint can be made at that location.

    template <typename T>
    void callWithABI(const T& fun, MoveOp::Type result = MoveOp::GENERAL) {
        profilerPreCall();
        MacroAssemblerSpecific::callWithABI(fun, result);
        profilerPostReturn();
    }

    // see above comment for what is returned
    uint32_t callJit(Register callee) {
        profilerPreCall();
        MacroAssemblerSpecific::callJit(callee);
        uint32_t ret = currentOffset();
        profilerPostReturn();
        return ret;
    }

    // see above comment for what is returned
    uint32_t callWithExitFrame(Label* target) {
        profilerPreCall();
        MacroAssemblerSpecific::callWithExitFrame(target);
        uint32_t ret = currentOffset();
        profilerPostReturn();
        return ret;
    }

    // see above comment for what is returned
    uint32_t callWithExitFrame(JitCode* target) {
        profilerPreCall();
        MacroAssemblerSpecific::callWithExitFrame(target);
        uint32_t ret = currentOffset();
        profilerPostReturn();
        return ret;
    }

    // see above comment for what is returned
    uint32_t callWithExitFrame(JitCode* target, Register dynStack) {
        profilerPreCall();
        MacroAssemblerSpecific::callWithExitFrame(target, dynStack);
        uint32_t ret = currentOffset();
        profilerPostReturn();
        return ret;
    }

    void branchTestObjectTruthy(bool truthy, Register objReg, Register scratch,
                                Label* slowCheck, Label* checked)
    {
        // The branches to out-of-line code here implement a conservative version
        // of the JSObject::isWrapper test performed in EmulatesUndefined.  If none
        // of the branches are taken, we can check class flags directly.
        loadObjClass(objReg, scratch);
        Address flags(scratch, Class::offsetOfFlags());

        branchTestClassIsProxy(true, scratch, slowCheck);

        Condition cond = truthy ? Assembler::Zero : Assembler::NonZero;
        branchTest32(cond, flags, Imm32(JSCLASS_EMULATES_UNDEFINED), checked);
    }

    void branchTestClassIsProxy(bool proxy, Register clasp, Label* label)
    {
        branchTest32(proxy ? Assembler::NonZero : Assembler::Zero,
                     Address(clasp, Class::offsetOfFlags()),
                     Imm32(JSCLASS_IS_PROXY), label);
    }

    void branchTestObjectIsProxy(bool proxy, Register object, Register scratch, Label* label)
    {
        loadObjClass(object, scratch);
        branchTestClassIsProxy(proxy, scratch, label);
    }

  private:
    // These two functions are helpers used around call sites throughout the
    // assembler. They are called from the above call wrappers to emit the
    // necessary instrumentation.
    void profilerPreCall() {
        if (!emitProfilingInstrumentation_)
            return;
        profilerPreCallImpl();
    }

    void profilerPostReturn() {
        if (!emitProfilingInstrumentation_)
            return;
        profilerPostReturnImpl();
    }

  public:
    void loadBaselineOrIonRaw(Register script, Register dest, Label* failure);
    void loadBaselineOrIonNoArgCheck(Register callee, Register dest, Label* failure);

    void loadBaselineFramePtr(Register framePtr, Register dest);

    void pushBaselineFramePtr(Register framePtr, Register scratch) {
        loadBaselineFramePtr(framePtr, scratch);
        push(scratch);
    }

  private:
    void handleFailure();

  public:
    Label* exceptionLabel() {
        // Exceptions are currently handled the same way as sequential failures.
        return &failureLabel_;
    }

    Label* failureLabel() {
        return &failureLabel_;
    }

    void finish();
    void link(JitCode* code);

    void assumeUnreachable(const char* output);

    template<typename T>
    void assertTestInt32(Condition cond, const T& value, const char* output);

    void printf(const char* output);
    void printf(const char* output, Register value);

#ifdef JS_TRACE_LOGGING
    void tracelogStartId(Register logger, uint32_t textId, bool force = false);
    void tracelogStartId(Register logger, Register textId);
    void tracelogStartEvent(Register logger, Register event);
    void tracelogStopId(Register logger, uint32_t textId, bool force = false);
    void tracelogStopId(Register logger, Register textId);
#endif

#define DISPATCH_FLOATING_POINT_OP(method, type, arg1d, arg1f, arg2)    \
    MOZ_ASSERT(IsFloatingPointType(type));                              \
    if (type == MIRType_Double)                                         \
        method##Double(arg1d, arg2);                                    \
    else                                                                \
        method##Float32(arg1f, arg2);                                   \

    void loadConstantFloatingPoint(double d, float f, FloatRegister dest, MIRType destType) {
        DISPATCH_FLOATING_POINT_OP(loadConstant, destType, d, f, dest);
    }
    void boolValueToFloatingPoint(ValueOperand value, FloatRegister dest, MIRType destType) {
        DISPATCH_FLOATING_POINT_OP(boolValueTo, destType, value, value, dest);
    }
    void int32ValueToFloatingPoint(ValueOperand value, FloatRegister dest, MIRType destType) {
        DISPATCH_FLOATING_POINT_OP(int32ValueTo, destType, value, value, dest);
    }
    void convertInt32ToFloatingPoint(Register src, FloatRegister dest, MIRType destType) {
        DISPATCH_FLOATING_POINT_OP(convertInt32To, destType, src, src, dest);
    }

#undef DISPATCH_FLOATING_POINT_OP

    void convertValueToFloatingPoint(ValueOperand value, FloatRegister output, Label* fail,
                                     MIRType outputType);
    bool convertValueToFloatingPoint(JSContext* cx, const Value& v, FloatRegister output,
                                     Label* fail, MIRType outputType);
    bool convertConstantOrRegisterToFloatingPoint(JSContext* cx, ConstantOrRegister src,
                                                  FloatRegister output, Label* fail,
                                                  MIRType outputType);
    void convertTypedOrValueToFloatingPoint(TypedOrValueRegister src, FloatRegister output,
                                            Label* fail, MIRType outputType);

    void convertInt32ValueToDouble(const Address& address, Register scratch, Label* done);
    void convertValueToDouble(ValueOperand value, FloatRegister output, Label* fail) {
        convertValueToFloatingPoint(value, output, fail, MIRType_Double);
    }
    bool convertValueToDouble(JSContext* cx, const Value& v, FloatRegister output, Label* fail) {
        return convertValueToFloatingPoint(cx, v, output, fail, MIRType_Double);
    }
    bool convertConstantOrRegisterToDouble(JSContext* cx, ConstantOrRegister src,
                                           FloatRegister output, Label* fail)
    {
        return convertConstantOrRegisterToFloatingPoint(cx, src, output, fail, MIRType_Double);
    }
    void convertTypedOrValueToDouble(TypedOrValueRegister src, FloatRegister output, Label* fail) {
        convertTypedOrValueToFloatingPoint(src, output, fail, MIRType_Double);
    }

    void convertValueToFloat(ValueOperand value, FloatRegister output, Label* fail) {
        convertValueToFloatingPoint(value, output, fail, MIRType_Float32);
    }
    bool convertValueToFloat(JSContext* cx, const Value& v, FloatRegister output, Label* fail) {
        return convertValueToFloatingPoint(cx, v, output, fail, MIRType_Float32);
    }
    bool convertConstantOrRegisterToFloat(JSContext* cx, ConstantOrRegister src,
                                          FloatRegister output, Label* fail)
    {
        return convertConstantOrRegisterToFloatingPoint(cx, src, output, fail, MIRType_Float32);
    }
    void convertTypedOrValueToFloat(TypedOrValueRegister src, FloatRegister output, Label* fail) {
        convertTypedOrValueToFloatingPoint(src, output, fail, MIRType_Float32);
    }

    enum IntConversionBehavior {
        IntConversion_Normal,
        IntConversion_NegativeZeroCheck,
        IntConversion_Truncate,
        IntConversion_ClampToUint8,
    };

    enum IntConversionInputKind {
        IntConversion_NumbersOnly,
        IntConversion_NumbersOrBoolsOnly,
        IntConversion_Any
    };

    //
    // Functions for converting values to int.
    //
    void convertDoubleToInt(FloatRegister src, Register output, FloatRegister temp,
                            Label* truncateFail, Label* fail, IntConversionBehavior behavior);

    // Strings may be handled by providing labels to jump to when the behavior
    // is truncation or clamping. The subroutine, usually an OOL call, is
    // passed the unboxed string in |stringReg| and should convert it to a
    // double store into |temp|.
    void convertValueToInt(ValueOperand value, MDefinition* input,
                           Label* handleStringEntry, Label* handleStringRejoin,
                           Label* truncateDoubleSlow,
                           Register stringReg, FloatRegister temp, Register output,
                           Label* fail, IntConversionBehavior behavior,
                           IntConversionInputKind conversion = IntConversion_Any);
    void convertValueToInt(ValueOperand value, FloatRegister temp, Register output, Label* fail,
                           IntConversionBehavior behavior)
    {
        convertValueToInt(value, nullptr, nullptr, nullptr, nullptr, InvalidReg, temp, output,
                          fail, behavior);
    }
    bool convertValueToInt(JSContext* cx, const Value& v, Register output, Label* fail,
                           IntConversionBehavior behavior);
    bool convertConstantOrRegisterToInt(JSContext* cx, ConstantOrRegister src, FloatRegister temp,
                                        Register output, Label* fail, IntConversionBehavior behavior);
    void convertTypedOrValueToInt(TypedOrValueRegister src, FloatRegister temp, Register output,
                                  Label* fail, IntConversionBehavior behavior);

    //
    // Convenience functions for converting values to int32.
    //
    void convertValueToInt32(ValueOperand value, FloatRegister temp, Register output, Label* fail,
                             bool negativeZeroCheck)
    {
        convertValueToInt(value, temp, output, fail, negativeZeroCheck
                          ? IntConversion_NegativeZeroCheck
                          : IntConversion_Normal);
    }
    void convertValueToInt32(ValueOperand value, MDefinition* input,
                             FloatRegister temp, Register output, Label* fail,
                             bool negativeZeroCheck, IntConversionInputKind conversion = IntConversion_Any)
    {
        convertValueToInt(value, input, nullptr, nullptr, nullptr, InvalidReg, temp, output, fail,
                          negativeZeroCheck
                          ? IntConversion_NegativeZeroCheck
                          : IntConversion_Normal,
                          conversion);
    }
    bool convertValueToInt32(JSContext* cx, const Value& v, Register output, Label* fail,
                             bool negativeZeroCheck)
    {
        return convertValueToInt(cx, v, output, fail, negativeZeroCheck
                                 ? IntConversion_NegativeZeroCheck
                                 : IntConversion_Normal);
    }
    bool convertConstantOrRegisterToInt32(JSContext* cx, ConstantOrRegister src, FloatRegister temp,
                                          Register output, Label* fail, bool negativeZeroCheck)
    {
        return convertConstantOrRegisterToInt(cx, src, temp, output, fail, negativeZeroCheck
                                              ? IntConversion_NegativeZeroCheck
                                              : IntConversion_Normal);
    }
    void convertTypedOrValueToInt32(TypedOrValueRegister src, FloatRegister temp, Register output,
                                    Label* fail, bool negativeZeroCheck)
    {
        convertTypedOrValueToInt(src, temp, output, fail, negativeZeroCheck
                                 ? IntConversion_NegativeZeroCheck
                                 : IntConversion_Normal);
    }

    //
    // Convenience functions for truncating values to int32.
    //
    void truncateValueToInt32(ValueOperand value, FloatRegister temp, Register output, Label* fail) {
        convertValueToInt(value, temp, output, fail, IntConversion_Truncate);
    }
    void truncateValueToInt32(ValueOperand value, MDefinition* input,
                              Label* handleStringEntry, Label* handleStringRejoin,
                              Label* truncateDoubleSlow,
                              Register stringReg, FloatRegister temp, Register output, Label* fail)
    {
        convertValueToInt(value, input, handleStringEntry, handleStringRejoin, truncateDoubleSlow,
                          stringReg, temp, output, fail, IntConversion_Truncate);
    }
    void truncateValueToInt32(ValueOperand value, MDefinition* input,
                              FloatRegister temp, Register output, Label* fail)
    {
        convertValueToInt(value, input, nullptr, nullptr, nullptr, InvalidReg, temp, output, fail,
                          IntConversion_Truncate);
    }
    bool truncateValueToInt32(JSContext* cx, const Value& v, Register output, Label* fail) {
        return convertValueToInt(cx, v, output, fail, IntConversion_Truncate);
    }
    bool truncateConstantOrRegisterToInt32(JSContext* cx, ConstantOrRegister src, FloatRegister temp,
                                           Register output, Label* fail)
    {
        return convertConstantOrRegisterToInt(cx, src, temp, output, fail, IntConversion_Truncate);
    }
    void truncateTypedOrValueToInt32(TypedOrValueRegister src, FloatRegister temp, Register output,
                                     Label* fail)
    {
        convertTypedOrValueToInt(src, temp, output, fail, IntConversion_Truncate);
    }

    // Convenience functions for clamping values to uint8.
    void clampValueToUint8(ValueOperand value, FloatRegister temp, Register output, Label* fail) {
        convertValueToInt(value, temp, output, fail, IntConversion_ClampToUint8);
    }
    void clampValueToUint8(ValueOperand value, MDefinition* input,
                           Label* handleStringEntry, Label* handleStringRejoin,
                           Register stringReg, FloatRegister temp, Register output, Label* fail)
    {
        convertValueToInt(value, input, handleStringEntry, handleStringRejoin, nullptr,
                          stringReg, temp, output, fail, IntConversion_ClampToUint8);
    }
    void clampValueToUint8(ValueOperand value, MDefinition* input,
                           FloatRegister temp, Register output, Label* fail)
    {
        convertValueToInt(value, input, nullptr, nullptr, nullptr, InvalidReg, temp, output, fail,
                          IntConversion_ClampToUint8);
    }
    bool clampValueToUint8(JSContext* cx, const Value& v, Register output, Label* fail) {
        return convertValueToInt(cx, v, output, fail, IntConversion_ClampToUint8);
    }
    bool clampConstantOrRegisterToUint8(JSContext* cx, ConstantOrRegister src, FloatRegister temp,
                                        Register output, Label* fail)
    {
        return convertConstantOrRegisterToInt(cx, src, temp, output, fail,
                                              IntConversion_ClampToUint8);
    }
    void clampTypedOrValueToUint8(TypedOrValueRegister src, FloatRegister temp, Register output,
                                  Label* fail)
    {
        convertTypedOrValueToInt(src, temp, output, fail, IntConversion_ClampToUint8);
    }

  public:
    class AfterICSaveLive {
        friend class MacroAssembler;
        explicit AfterICSaveLive(uint32_t initialStack)
#ifdef JS_DEBUG
          : initialStack(initialStack)
#endif
        {}

      public:
#ifdef JS_DEBUG
        uint32_t initialStack;
#endif
        uint32_t alignmentPadding;
    };

    void alignFrameForICArguments(AfterICSaveLive& aic);
    void restoreFrameAlignmentForICArguments(AfterICSaveLive& aic);

    AfterICSaveLive icSaveLive(RegisterSet& liveRegs) {
        PushRegsInMask(liveRegs);
        AfterICSaveLive aic(framePushed());
        alignFrameForICArguments(aic);
        return aic;
    }

    bool icBuildOOLFakeExitFrame(void* fakeReturnAddr, AfterICSaveLive& aic) {
        return buildOOLFakeExitFrame(fakeReturnAddr);
    }

    void icRestoreLive(RegisterSet& liveRegs, AfterICSaveLive& aic) {
        restoreFrameAlignmentForICArguments(aic);
        MOZ_ASSERT(framePushed() == aic.initialStack);
        PopRegsInMask(liveRegs);
    }

    // Align the stack pointer based on the number of arguments which are pushed
    // on the stack, such that the JitFrameLayout would be correctly aligned on
    // the JitStackAlignment.
    void alignJitStackBasedOnNArgs(Register nargs);
    void alignJitStackBasedOnNArgs(uint32_t nargs);

    void assertStackAlignment(uint32_t alignment, int32_t offset = 0) {
#ifdef DEBUG
        Label ok, bad;
        MOZ_ASSERT(IsPowerOfTwo(alignment));

        // Wrap around the offset to be a non-negative number.
        offset %= alignment;
        if (offset < 0)
            offset += alignment;

        // Test if each bit from offset is set.
        uint32_t off = offset;
        while (off) {
            uint32_t lowestBit = 1 << mozilla::CountTrailingZeroes32(off);
            branchTestPtr(Assembler::Zero, StackPointer, Imm32(lowestBit), &bad);
            off ^= lowestBit;
        }

        // Check that all remaining bits are zero.
        branchTestPtr(Assembler::Zero, StackPointer, Imm32((alignment - 1) ^ offset), &ok);

        bind(&bad);
        breakpoint();
        bind(&ok);
#endif
    }

    void profilerPreCallImpl();
    void profilerPreCallImpl(Register reg, Register reg2);
    void profilerPostReturnImpl() {}
};

static inline Assembler::DoubleCondition
JSOpToDoubleCondition(JSOp op)
{
    switch (op) {
      case JSOP_EQ:
      case JSOP_STRICTEQ:
        return Assembler::DoubleEqual;
      case JSOP_NE:
      case JSOP_STRICTNE:
        return Assembler::DoubleNotEqualOrUnordered;
      case JSOP_LT:
        return Assembler::DoubleLessThan;
      case JSOP_LE:
        return Assembler::DoubleLessThanOrEqual;
      case JSOP_GT:
        return Assembler::DoubleGreaterThan;
      case JSOP_GE:
        return Assembler::DoubleGreaterThanOrEqual;
      default:
        MOZ_CRASH("Unexpected comparison operation");
    }
}

// Note: the op may have been inverted during lowering (to put constants in a
// position where they can be immediates), so it is important to use the
// lir->jsop() instead of the mir->jsop() when it is present.
static inline Assembler::Condition
JSOpToCondition(JSOp op, bool isSigned)
{
    if (isSigned) {
        switch (op) {
          case JSOP_EQ:
          case JSOP_STRICTEQ:
            return Assembler::Equal;
          case JSOP_NE:
          case JSOP_STRICTNE:
            return Assembler::NotEqual;
          case JSOP_LT:
            return Assembler::LessThan;
          case JSOP_LE:
            return Assembler::LessThanOrEqual;
          case JSOP_GT:
            return Assembler::GreaterThan;
          case JSOP_GE:
            return Assembler::GreaterThanOrEqual;
          default:
            MOZ_CRASH("Unrecognized comparison operation");
        }
    } else {
        switch (op) {
          case JSOP_EQ:
          case JSOP_STRICTEQ:
            return Assembler::Equal;
          case JSOP_NE:
          case JSOP_STRICTNE:
            return Assembler::NotEqual;
          case JSOP_LT:
            return Assembler::Below;
          case JSOP_LE:
            return Assembler::BelowOrEqual;
          case JSOP_GT:
            return Assembler::Above;
          case JSOP_GE:
            return Assembler::AboveOrEqual;
          default:
            MOZ_CRASH("Unrecognized comparison operation");
        }
    }
}

static inline size_t
StackDecrementForCall(uint32_t alignment, size_t bytesAlreadyPushed, size_t bytesToPush)
{
    return bytesToPush +
           ComputeByteAlignment(bytesAlreadyPushed + bytesToPush, alignment);
}

} // namespace jit
} // namespace js

#endif /* jit_MacroAssembler_h */
