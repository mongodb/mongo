/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineIC_h
#define jit_BaselineIC_h

#include "mozilla/Assertions.h"

#include "builtin/TypedObject.h"
#include "gc/Barrier.h"
#include "jit/BaselineICList.h"
#include "jit/BaselineJIT.h"
#include "jit/SharedIC.h"
#include "jit/SharedICRegisters.h"
#include "js/GCVector.h"
#include "vm/ArrayObject.h"
#include "vm/BytecodeUtil.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"
#include "vm/UnboxedObject.h"

namespace js {
namespace jit {

// WarmUpCounter_Fallback

// A WarmUpCounter IC chain has only the fallback stub.
class ICWarmUpCounter_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICWarmUpCounter_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::WarmUpCounter_Fallback, stubCode)
    { }

  public:
    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::WarmUpCounter_Fallback, Engine::Baseline)
        { }

        ICWarmUpCounter_Fallback* getStub(ICStubSpace* space) override {
            return newStub<ICWarmUpCounter_Fallback>(space, getStubCode());
        }
    };
};


// TypeUpdate

extern const VMFunction DoTypeUpdateFallbackInfo;

// The TypeUpdate fallback is not a regular fallback, since it just
// forwards to a different entry point in the main fallback stub.
class ICTypeUpdate_Fallback : public ICStub
{
    friend class ICStubSpace;

    explicit ICTypeUpdate_Fallback(JitCode* stubCode)
      : ICStub(ICStub::TypeUpdate_Fallback, stubCode)
    {}

  public:
    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::TypeUpdate_Fallback, Engine::Baseline)
        { }

        ICTypeUpdate_Fallback* getStub(ICStubSpace* space) override {
            return newStub<ICTypeUpdate_Fallback>(space, getStubCode());
        }
    };
};

class ICTypeUpdate_PrimitiveSet : public TypeCheckPrimitiveSetStub
{
    friend class ICStubSpace;

    ICTypeUpdate_PrimitiveSet(JitCode* stubCode, uint16_t flags)
        : TypeCheckPrimitiveSetStub(TypeUpdate_PrimitiveSet, stubCode, flags)
    {}

  public:
    class Compiler : public TypeCheckPrimitiveSetStub::Compiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        Compiler(JSContext* cx, ICTypeUpdate_PrimitiveSet* existingStub, JSValueType type)
          : TypeCheckPrimitiveSetStub::Compiler(cx, TypeUpdate_PrimitiveSet,
                                                existingStub, type)
        {}

        ICTypeUpdate_PrimitiveSet* updateStub() {
            TypeCheckPrimitiveSetStub* stub =
                this->TypeCheckPrimitiveSetStub::Compiler::updateStub();
            if (!stub)
                return nullptr;
            return stub->toUpdateStub();
        }

        ICTypeUpdate_PrimitiveSet* getStub(ICStubSpace* space) override {
            MOZ_ASSERT(!existingStub_);
            return newStub<ICTypeUpdate_PrimitiveSet>(space, getStubCode(), flags_);
        }
    };
};

// Type update stub to handle a singleton object.
class ICTypeUpdate_SingleObject : public ICStub
{
    friend class ICStubSpace;

    GCPtrObject obj_;

    ICTypeUpdate_SingleObject(JitCode* stubCode, JSObject* obj);

  public:
    GCPtrObject& object() {
        return obj_;
    }

    static size_t offsetOfObject() {
        return offsetof(ICTypeUpdate_SingleObject, obj_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        HandleObject obj_;
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        Compiler(JSContext* cx, HandleObject obj)
          : ICStubCompiler(cx, TypeUpdate_SingleObject, Engine::Baseline),
            obj_(obj)
        { }

        ICTypeUpdate_SingleObject* getStub(ICStubSpace* space) override {
            return newStub<ICTypeUpdate_SingleObject>(space, getStubCode(), obj_);
        }
    };
};

// Type update stub to handle a single ObjectGroup.
class ICTypeUpdate_ObjectGroup : public ICStub
{
    friend class ICStubSpace;

    GCPtrObjectGroup group_;

    ICTypeUpdate_ObjectGroup(JitCode* stubCode, ObjectGroup* group);

  public:
    GCPtrObjectGroup& group() {
        return group_;
    }

    static size_t offsetOfGroup() {
        return offsetof(ICTypeUpdate_ObjectGroup, group_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        HandleObjectGroup group_;
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        Compiler(JSContext* cx, HandleObjectGroup group)
          : ICStubCompiler(cx, TypeUpdate_ObjectGroup, Engine::Baseline),
            group_(group)
        { }

        ICTypeUpdate_ObjectGroup* getStub(ICStubSpace* space) override {
            return newStub<ICTypeUpdate_ObjectGroup>(space, getStubCode(), group_);
        }
    };
};

class ICTypeUpdate_AnyValue : public ICStub
{
    friend class ICStubSpace;

    explicit ICTypeUpdate_AnyValue(JitCode* stubCode)
      : ICStub(TypeUpdate_AnyValue, stubCode)
    {}

  public:
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, TypeUpdate_AnyValue, Engine::Baseline)
        {}

        ICTypeUpdate_AnyValue* getStub(ICStubSpace* space) override {
            return newStub<ICTypeUpdate_AnyValue>(space, getStubCode());
        }
    };
};

// ToBool
//      JSOP_IFNE

class ICToBool_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICToBool_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::ToBool_Fallback, stubCode) {}

  public:
    static const uint32_t MAX_OPTIMIZED_STUBS = 8;

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::ToBool_Fallback, Engine::Baseline) {}

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICToBool_Fallback>(space, getStubCode());
        }
    };
};

// ToNumber
//     JSOP_POS

class ICToNumber_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICToNumber_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::ToNumber_Fallback, stubCode) {}

  public:
    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::ToNumber_Fallback, Engine::Baseline) {}

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICToNumber_Fallback>(space, getStubCode());
        }
    };
};

// GetElem
//      JSOP_GETELEM
//      JSOP_GETELEM_SUPER

class ICGetElem_Fallback : public ICMonitoredFallbackStub
{
    friend class ICStubSpace;

    explicit ICGetElem_Fallback(JitCode* stubCode)
      : ICMonitoredFallbackStub(ICStub::GetElem_Fallback, stubCode)
    { }

    static const uint16_t EXTRA_NEGATIVE_INDEX = 0x1;
    static const uint16_t EXTRA_UNOPTIMIZABLE_ACCESS = 0x2;

  public:
    void noteNegativeIndex() {
        extra_ |= EXTRA_NEGATIVE_INDEX;
    }
    bool hasNegativeIndex() const {
        return extra_ & EXTRA_NEGATIVE_INDEX;
    }
    void noteUnoptimizableAccess() {
        extra_ |= EXTRA_UNOPTIMIZABLE_ACCESS;
    }
    bool hadUnoptimizableAccess() const {
        return extra_ & EXTRA_UNOPTIMIZABLE_ACCESS;
    }

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool hasReceiver_;
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

        virtual int32_t getKey() const override {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(hasReceiver_) << 17);
        }

      public:
        explicit Compiler(JSContext* cx, bool hasReceiver = false)
          : ICStubCompiler(cx, ICStub::GetElem_Fallback, Engine::Baseline),
            hasReceiver_(hasReceiver)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICGetElem_Fallback>(space, getStubCode());
        }
    };
};

// SetElem
//      JSOP_SETELEM
//      JSOP_INITELEM

class ICSetElem_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICSetElem_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::SetElem_Fallback, stubCode)
    { }

    static const size_t HasDenseAddFlag = 0x1;
    static const size_t HasTypedArrayOOBFlag = 0x2;

  public:
    void noteHasDenseAdd() { extra_ |= HasDenseAddFlag; }
    bool hasDenseAdd() const { return extra_ & HasDenseAddFlag; }

    void noteHasTypedArrayOOB() { extra_ |= HasTypedArrayOOBFlag; }
    bool hasTypedArrayOOB() const { return extra_ & HasTypedArrayOOBFlag; }

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::SetElem_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICSetElem_Fallback>(space, getStubCode());
        }
    };
};

// In
//      JSOP_IN
class ICIn_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICIn_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::In_Fallback, stubCode)
    { }

  public:
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::In_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICIn_Fallback>(space, getStubCode());
        }
    };
};

// HasOwn
//      JSOP_HASOWN
class ICHasOwn_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICHasOwn_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::HasOwn_Fallback, stubCode)
    { }

  public:
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::HasOwn_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICHasOwn_Fallback>(space, getStubCode());
        }
    };
};

// GetName
//      JSOP_GETNAME
//      JSOP_GETGNAME
class ICGetName_Fallback : public ICMonitoredFallbackStub
{
    friend class ICStubSpace;

    explicit ICGetName_Fallback(JitCode* stubCode)
      : ICMonitoredFallbackStub(ICStub::GetName_Fallback, stubCode)
    { }

  public:
    static const size_t UNOPTIMIZABLE_ACCESS_BIT = 0;

    void noteUnoptimizableAccess() {
        extra_ |= (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }
    bool hadUnoptimizableAccess() const {
        return extra_ & (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }

    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::GetName_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICGetName_Fallback>(space, getStubCode());
        }
    };
};

// BindName
//      JSOP_BINDNAME
class ICBindName_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICBindName_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::BindName_Fallback, stubCode)
    { }

  public:
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::BindName_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICBindName_Fallback>(space, getStubCode());
        }
    };
};

// GetIntrinsic
//      JSOP_GETINTRINSIC
class ICGetIntrinsic_Fallback : public ICMonitoredFallbackStub
{
    friend class ICStubSpace;

    explicit ICGetIntrinsic_Fallback(JitCode* stubCode)
      : ICMonitoredFallbackStub(ICStub::GetIntrinsic_Fallback, stubCode)
    { }

  public:
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::GetIntrinsic_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICGetIntrinsic_Fallback>(space, getStubCode());
        }
    };
};

// SetProp
//     JSOP_SETPROP
//     JSOP_SETNAME
//     JSOP_SETGNAME
//     JSOP_INITPROP

class ICSetProp_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICSetProp_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::SetProp_Fallback, stubCode)
    { }

  public:
    static const size_t UNOPTIMIZABLE_ACCESS_BIT = 0;
    void noteUnoptimizableAccess() {
        extra_ |= (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }
    bool hadUnoptimizableAccess() const {
        return extra_ & (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }

    class Compiler : public ICStubCompiler {
      protected:
        CodeOffset bailoutReturnOffset_;
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;
        void postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::SetProp_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICSetProp_Fallback>(space, getStubCode());
        }
    };
};

// Call
//      JSOP_CALL
//      JSOP_CALL_IGNORES_RV
//      JSOP_FUNAPPLY
//      JSOP_FUNCALL
//      JSOP_NEW
//      JSOP_SPREADCALL
//      JSOP_SPREADNEW
//      JSOP_SPREADEVAL

class ICCallStubCompiler : public ICStubCompiler
{
  protected:
    ICCallStubCompiler(JSContext* cx, ICStub::Kind kind)
      : ICStubCompiler(cx, kind, Engine::Baseline)
    { }

    enum FunApplyThing {
        FunApply_MagicArgs,
        FunApply_Array
    };

    void pushCallArguments(MacroAssembler& masm, AllocatableGeneralRegisterSet regs,
                           Register argcReg, bool isJitCall, bool isConstructing = false);
    void pushSpreadCallArguments(MacroAssembler& masm, AllocatableGeneralRegisterSet regs,
                                 Register argcReg, bool isJitCall, bool isConstructing);
    void guardSpreadCall(MacroAssembler& masm, Register argcReg, Label* failure,
                         bool isConstructing);
    Register guardFunApply(MacroAssembler& masm, AllocatableGeneralRegisterSet regs,
                           Register argcReg, FunApplyThing applyThing,
                           Label* failure);
    void pushCallerArguments(MacroAssembler& masm, AllocatableGeneralRegisterSet regs);
    void pushArrayArguments(MacroAssembler& masm, Address arrayVal,
                            AllocatableGeneralRegisterSet regs);
};

class ICCall_Fallback : public ICMonitoredFallbackStub
{
    friend class ICStubSpace;
  public:
    static const unsigned UNOPTIMIZABLE_CALL_FLAG = 0x1;

    static const uint32_t MAX_OPTIMIZED_STUBS = 16;

  private:
    explicit ICCall_Fallback(JitCode* stubCode)
      : ICMonitoredFallbackStub(ICStub::Call_Fallback, stubCode)
    {}

  public:
    void noteUnoptimizableCall() {
        extra_ |= UNOPTIMIZABLE_CALL_FLAG;
    }
    bool hadUnoptimizableCall() const {
        return extra_ & UNOPTIMIZABLE_CALL_FLAG;
    }

    bool scriptedStubsAreGeneralized() const {
        return hasStub(Call_AnyScripted);
    }
    bool nativeStubsAreGeneralized() const {
        // Return hasStub(Call_AnyNative) after Call_AnyNative stub is added.
        return false;
    }

    // Compiler for this stub kind.
    class Compiler : public ICCallStubCompiler {
      protected:
        bool isConstructing_;
        bool isSpread_;
        CodeOffset bailoutReturnOffset_;
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;
        void postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code) override;

        virtual int32_t getKey() const override {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(isSpread_) << 17) |
                  (static_cast<int32_t>(isConstructing_) << 18);
        }

      public:
        Compiler(JSContext* cx, bool isConstructing, bool isSpread)
          : ICCallStubCompiler(cx, ICStub::Call_Fallback),
            isConstructing_(isConstructing),
            isSpread_(isSpread)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICCall_Fallback>(space, getStubCode());
        }
    };
};

class ICCall_Scripted : public ICMonitoredStub
{
    friend class ICStubSpace;
  public:
    // The maximum number of inlineable spread call arguments. Keep this small
    // to avoid controllable stack overflows by attackers passing large arrays
    // to spread call. This value is shared with ICCall_Native.
    static const uint32_t MAX_ARGS_SPREAD_LENGTH = 16;

  protected:
    GCPtrFunction callee_;
    GCPtrObject templateObject_;
    uint32_t pcOffset_;

    ICCall_Scripted(JitCode* stubCode, ICStub* firstMonitorStub,
                    JSFunction* callee, JSObject* templateObject,
                    uint32_t pcOffset);

  public:
    static ICCall_Scripted* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                  ICCall_Scripted& other);

    GCPtrFunction& callee() {
        return callee_;
    }
    GCPtrObject& templateObject() {
        return templateObject_;
    }

    static size_t offsetOfCallee() {
        return offsetof(ICCall_Scripted, callee_);
    }
    static size_t offsetOfPCOffset() {
        return offsetof(ICCall_Scripted, pcOffset_);
    }
};

class ICCall_AnyScripted : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    uint32_t pcOffset_;

    ICCall_AnyScripted(JitCode* stubCode, ICStub* firstMonitorStub, uint32_t pcOffset)
      : ICMonitoredStub(ICStub::Call_AnyScripted, stubCode, firstMonitorStub),
        pcOffset_(pcOffset)
    { }

  public:
    static ICCall_AnyScripted* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                     ICCall_AnyScripted& other);

    static size_t offsetOfPCOffset() {
        return offsetof(ICCall_AnyScripted, pcOffset_);
    }
};

// Compiler for Call_Scripted and Call_AnyScripted stubs.
class ICCallScriptedCompiler : public ICCallStubCompiler {
  protected:
    ICStub* firstMonitorStub_;
    bool isConstructing_;
    bool isSpread_;
    RootedFunction callee_;
    RootedObject templateObject_;
    uint32_t pcOffset_;
    MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

    virtual int32_t getKey() const override {
        return static_cast<int32_t>(engine_) |
              (static_cast<int32_t>(kind) << 1) |
              (static_cast<int32_t>(isConstructing_) << 17) |
              (static_cast<int32_t>(isSpread_) << 18);
    }

  public:
    ICCallScriptedCompiler(JSContext* cx, ICStub* firstMonitorStub,
                           JSFunction* callee, JSObject* templateObject,
                           bool isConstructing, bool isSpread, uint32_t pcOffset)
      : ICCallStubCompiler(cx, ICStub::Call_Scripted),
        firstMonitorStub_(firstMonitorStub),
        isConstructing_(isConstructing),
        isSpread_(isSpread),
        callee_(cx, callee),
        templateObject_(cx, templateObject),
        pcOffset_(pcOffset)
    { }

    ICCallScriptedCompiler(JSContext* cx, ICStub* firstMonitorStub, bool isConstructing,
                           bool isSpread, uint32_t pcOffset)
      : ICCallStubCompiler(cx, ICStub::Call_AnyScripted),
        firstMonitorStub_(firstMonitorStub),
        isConstructing_(isConstructing),
        isSpread_(isSpread),
        callee_(cx, nullptr),
        templateObject_(cx, nullptr),
        pcOffset_(pcOffset)
    { }

    ICStub* getStub(ICStubSpace* space) override {
        if (callee_) {
            return newStub<ICCall_Scripted>(space, getStubCode(), firstMonitorStub_, callee_,
                                            templateObject_, pcOffset_);
        }
        return newStub<ICCall_AnyScripted>(space, getStubCode(), firstMonitorStub_, pcOffset_);
    }
};

class ICCall_Native : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    GCPtrFunction callee_;
    GCPtrObject templateObject_;
    uint32_t pcOffset_;

#ifdef JS_SIMULATOR
    void* native_;
#endif

    ICCall_Native(JitCode* stubCode, ICStub* firstMonitorStub,
                  JSFunction* callee, JSObject* templateObject,
                  uint32_t pcOffset);

  public:
    static ICCall_Native* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                ICCall_Native& other);

    GCPtrFunction& callee() {
        return callee_;
    }
    GCPtrObject& templateObject() {
        return templateObject_;
    }

    static size_t offsetOfCallee() {
        return offsetof(ICCall_Native, callee_);
    }
    static size_t offsetOfPCOffset() {
        return offsetof(ICCall_Native, pcOffset_);
    }

#ifdef JS_SIMULATOR
    static size_t offsetOfNative() {
        return offsetof(ICCall_Native, native_);
    }
#endif

    // Compiler for this stub kind.
    class Compiler : public ICCallStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        bool isConstructing_;
        bool ignoresReturnValue_;
        bool isSpread_;
        RootedFunction callee_;
        RootedObject templateObject_;
        uint32_t pcOffset_;
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

        virtual int32_t getKey() const override {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(isSpread_) << 17) |
                  (static_cast<int32_t>(isConstructing_) << 18) |
                  (static_cast<int32_t>(ignoresReturnValue_) << 19);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub,
                 HandleFunction callee, HandleObject templateObject,
                 bool isConstructing, bool ignoresReturnValue, bool isSpread, uint32_t pcOffset)
          : ICCallStubCompiler(cx, ICStub::Call_Native),
            firstMonitorStub_(firstMonitorStub),
            isConstructing_(isConstructing),
            ignoresReturnValue_(ignoresReturnValue),
            isSpread_(isSpread),
            callee_(cx, callee),
            templateObject_(cx, templateObject),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICCall_Native>(space, getStubCode(), firstMonitorStub_, callee_,
                                          templateObject_, pcOffset_);
        }
    };
};

class ICCall_ClassHook : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    const Class* clasp_;
    void* native_;
    GCPtrObject templateObject_;
    uint32_t pcOffset_;

    ICCall_ClassHook(JitCode* stubCode, ICStub* firstMonitorStub,
                     const Class* clasp, Native native, JSObject* templateObject,
                     uint32_t pcOffset);

  public:
    static ICCall_ClassHook* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                   ICCall_ClassHook& other);

    const Class* clasp() {
        return clasp_;
    }
    void* native() {
        return native_;
    }
    GCPtrObject& templateObject() {
        return templateObject_;
    }

    static size_t offsetOfClass() {
        return offsetof(ICCall_ClassHook, clasp_);
    }
    static size_t offsetOfNative() {
        return offsetof(ICCall_ClassHook, native_);
    }
    static size_t offsetOfPCOffset() {
        return offsetof(ICCall_ClassHook, pcOffset_);
    }

    // Compiler for this stub kind.
    class Compiler : public ICCallStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        bool isConstructing_;
        const Class* clasp_;
        Native native_;
        RootedObject templateObject_;
        uint32_t pcOffset_;
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

        virtual int32_t getKey() const override {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(isConstructing_) << 17);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub,
                 const Class* clasp, Native native,
                 HandleObject templateObject, uint32_t pcOffset,
                 bool isConstructing)
          : ICCallStubCompiler(cx, ICStub::Call_ClassHook),
            firstMonitorStub_(firstMonitorStub),
            isConstructing_(isConstructing),
            clasp_(clasp),
            native_(native),
            templateObject_(cx, templateObject),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICCall_ClassHook>(space, getStubCode(), firstMonitorStub_, clasp_,
                                             native_, templateObject_, pcOffset_);
        }
    };
};

class ICCall_ScriptedApplyArray : public ICMonitoredStub
{
    friend class ICStubSpace;
  public:
    // The maximum length of an inlineable funcall array.
    // Keep this small to avoid controllable stack overflows by attackers passing large
    // arrays to fun.apply.
    static const uint32_t MAX_ARGS_ARRAY_LENGTH = 16;

  protected:
    uint32_t pcOffset_;

    ICCall_ScriptedApplyArray(JitCode* stubCode, ICStub* firstMonitorStub, uint32_t pcOffset)
      : ICMonitoredStub(ICStub::Call_ScriptedApplyArray, stubCode, firstMonitorStub),
        pcOffset_(pcOffset)
    {}

  public:
    static ICCall_ScriptedApplyArray* Clone(JSContext* cx,
                                            ICStubSpace* space,
                                            ICStub* firstMonitorStub,
                                            ICCall_ScriptedApplyArray& other);

    static size_t offsetOfPCOffset() {
        return offsetof(ICCall_ScriptedApplyArray, pcOffset_);
    }

    // Compiler for this stub kind.
    class Compiler : public ICCallStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        uint32_t pcOffset_;
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

        virtual int32_t getKey() const override {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset)
          : ICCallStubCompiler(cx, ICStub::Call_ScriptedApplyArray),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICCall_ScriptedApplyArray>(space, getStubCode(), firstMonitorStub_,
                                                      pcOffset_);
        }
    };
};

class ICCall_ScriptedApplyArguments : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    uint32_t pcOffset_;

    ICCall_ScriptedApplyArguments(JitCode* stubCode, ICStub* firstMonitorStub, uint32_t pcOffset)
      : ICMonitoredStub(ICStub::Call_ScriptedApplyArguments, stubCode, firstMonitorStub),
        pcOffset_(pcOffset)
    {}

  public:
    static ICCall_ScriptedApplyArguments* Clone(JSContext* cx,
                                                ICStubSpace* space,
                                                ICStub* firstMonitorStub,
                                                ICCall_ScriptedApplyArguments& other);

    static size_t offsetOfPCOffset() {
        return offsetof(ICCall_ScriptedApplyArguments, pcOffset_);
    }

    // Compiler for this stub kind.
    class Compiler : public ICCallStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        uint32_t pcOffset_;
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

        virtual int32_t getKey() const override {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset)
          : ICCallStubCompiler(cx, ICStub::Call_ScriptedApplyArguments),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICCall_ScriptedApplyArguments>(space, getStubCode(), firstMonitorStub_,
                                                          pcOffset_);
        }
    };
};

// Handles calls of the form |fun.call(...)| where fun is a scripted function.
class ICCall_ScriptedFunCall : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    uint32_t pcOffset_;

    ICCall_ScriptedFunCall(JitCode* stubCode, ICStub* firstMonitorStub, uint32_t pcOffset)
      : ICMonitoredStub(ICStub::Call_ScriptedFunCall, stubCode, firstMonitorStub),
        pcOffset_(pcOffset)
    {}

  public:
    static ICCall_ScriptedFunCall* Clone(JSContext* cx, ICStubSpace* space,
                                         ICStub* firstMonitorStub, ICCall_ScriptedFunCall& other);

    static size_t offsetOfPCOffset() {
        return offsetof(ICCall_ScriptedFunCall, pcOffset_);
    }

    // Compiler for this stub kind.
    class Compiler : public ICCallStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        uint32_t pcOffset_;
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

        virtual int32_t getKey() const override {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset)
          : ICCallStubCompiler(cx, ICStub::Call_ScriptedFunCall),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICCall_ScriptedFunCall>(space, getStubCode(), firstMonitorStub_,
                                                   pcOffset_);
        }
    };
};

class ICCall_ConstStringSplit : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    uint32_t pcOffset_;
    GCPtrString expectedStr_;
    GCPtrString expectedSep_;
    GCPtrArrayObject templateObject_;

    ICCall_ConstStringSplit(JitCode* stubCode, ICStub* firstMonitorStub, uint32_t pcOffset,
                            JSString* str, JSString* sep, ArrayObject* templateObject)
      : ICMonitoredStub(ICStub::Call_ConstStringSplit, stubCode, firstMonitorStub),
        pcOffset_(pcOffset), expectedStr_(str), expectedSep_(sep),
        templateObject_(templateObject)
    { }

  public:
    static size_t offsetOfExpectedStr() {
        return offsetof(ICCall_ConstStringSplit, expectedStr_);
    }

    static size_t offsetOfExpectedSep() {
        return offsetof(ICCall_ConstStringSplit, expectedSep_);
    }

    static size_t offsetOfTemplateObject() {
        return offsetof(ICCall_ConstStringSplit, templateObject_);
    }

    GCPtrString& expectedStr() {
        return expectedStr_;
    }

    GCPtrString& expectedSep() {
        return expectedSep_;
    }

    GCPtrArrayObject& templateObject() {
        return templateObject_;
    }

    class Compiler : public ICCallStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        uint32_t pcOffset_;
        RootedString expectedStr_;
        RootedString expectedSep_;
        RootedArrayObject templateObject_;

        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

        virtual int32_t getKey() const override {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset, HandleString str,
                 HandleString sep, HandleArrayObject templateObject)
          : ICCallStubCompiler(cx, ICStub::Call_ConstStringSplit),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset),
            expectedStr_(cx, str),
            expectedSep_(cx, sep),
            templateObject_(cx, templateObject)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICCall_ConstStringSplit>(space, getStubCode(), firstMonitorStub_,
                                                    pcOffset_, expectedStr_, expectedSep_,
                                                    templateObject_);
        }
   };
};

class ICCall_IsSuspendedGenerator : public ICStub
{
    friend class ICStubSpace;

  protected:
    explicit ICCall_IsSuspendedGenerator(JitCode* stubCode)
      : ICStub(ICStub::Call_IsSuspendedGenerator, stubCode)
    {}

  public:
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::Call_IsSuspendedGenerator, Engine::Baseline)
        {}
        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICCall_IsSuspendedGenerator>(space, getStubCode());
        }
   };
};

// Stub for performing a TableSwitch, updating the IC's return address to jump
// to whatever point the switch is branching to.
class ICTableSwitch : public ICStub
{
    friend class ICStubSpace;

  protected: // Protected to silence Clang warning.
    void** table_;
    int32_t min_;
    int32_t length_;
    void* defaultTarget_;

    ICTableSwitch(JitCode* stubCode, void** table,
                  int32_t min, int32_t length, void* defaultTarget)
      : ICStub(TableSwitch, stubCode), table_(table),
        min_(min), length_(length), defaultTarget_(defaultTarget)
    {}

  public:
    void fixupJumpTable(JSScript* script, BaselineScript* baseline);

    class Compiler : public ICStubCompiler {
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

        jsbytecode* pc_;

      public:
        Compiler(JSContext* cx, jsbytecode* pc)
          : ICStubCompiler(cx, ICStub::TableSwitch, Engine::Baseline), pc_(pc)
        {}

        ICStub* getStub(ICStubSpace* space) override;
    };
};

// IC for constructing an iterator from an input value.
class ICGetIterator_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICGetIterator_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::GetIterator_Fallback, stubCode)
    { }

  public:
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::GetIterator_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICGetIterator_Fallback>(space, getStubCode());
        }
    };
};

// IC for testing if there are more values in an iterator.
class ICIteratorMore_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICIteratorMore_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::IteratorMore_Fallback, stubCode)
    { }

  public:
    void setHasNonStringResult() {
        extra_ = 1;
    }
    bool hasNonStringResult() const {
        MOZ_ASSERT(extra_ <= 1);
        return extra_;
    }

    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::IteratorMore_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICIteratorMore_Fallback>(space, getStubCode());
        }
    };
};

// IC for testing if there are more values in a native iterator.
class ICIteratorMore_Native : public ICStub
{
    friend class ICStubSpace;

    explicit ICIteratorMore_Native(JitCode* stubCode)
      : ICStub(ICStub::IteratorMore_Native, stubCode)
    { }

  public:
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::IteratorMore_Native, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICIteratorMore_Native>(space, getStubCode());
        }
    };
};

// IC for closing an iterator.
class ICIteratorClose_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICIteratorClose_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::IteratorClose_Fallback, stubCode)
    { }

  public:
    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::IteratorClose_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICIteratorClose_Fallback>(space, getStubCode());
        }
    };
};

// InstanceOf
//      JSOP_INSTANCEOF
class ICInstanceOf_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICInstanceOf_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::InstanceOf_Fallback, stubCode)
    { }

    static const uint16_t UNOPTIMIZABLE_ACCESS_BIT = 0x1;

  public:

    void noteUnoptimizableAccess() {
        extra_ |= UNOPTIMIZABLE_ACCESS_BIT;
    }
    bool hadUnoptimizableAccess() const {
        return extra_ & UNOPTIMIZABLE_ACCESS_BIT;
    }

    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::InstanceOf_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICInstanceOf_Fallback>(space, getStubCode());
        }
    };
};

// TypeOf
//      JSOP_TYPEOF
//      JSOP_TYPEOFEXPR
class ICTypeOf_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICTypeOf_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::TypeOf_Fallback, stubCode)
    { }

  public:
    static const uint32_t MAX_OPTIMIZED_STUBS = 6;

    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::TypeOf_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICTypeOf_Fallback>(space, getStubCode());
        }
    };
};

class ICRest_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    GCPtrArrayObject templateObject_;

    ICRest_Fallback(JitCode* stubCode, ArrayObject* templateObject)
      : ICFallbackStub(ICStub::Rest_Fallback, stubCode), templateObject_(templateObject)
    { }

  public:
    static const uint32_t MAX_OPTIMIZED_STUBS = 8;

    GCPtrArrayObject& templateObject() {
        return templateObject_;
    }

    class Compiler : public ICStubCompiler {
      protected:
        RootedArrayObject templateObject;
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        Compiler(JSContext* cx, ArrayObject* templateObject)
          : ICStubCompiler(cx, ICStub::Rest_Fallback, Engine::Baseline),
            templateObject(cx, templateObject)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICRest_Fallback>(space, getStubCode(), templateObject);
        }
    };
};

// Stub for JSOP_RETSUB ("returning" from a |finally| block).
class ICRetSub_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICRetSub_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::RetSub_Fallback, stubCode)
    { }

  public:
    static const uint32_t MAX_OPTIMIZED_STUBS = 8;

    class Compiler : public ICStubCompiler {
      protected:
        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::RetSub_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICRetSub_Fallback>(space, getStubCode());
        }
    };
};

// Optimized JSOP_RETSUB stub. Every stub maps a single pc offset to its
// native code address.
class ICRetSub_Resume : public ICStub
{
    friend class ICStubSpace;

  protected:
    uint32_t pcOffset_;
    uint8_t* addr_;

    ICRetSub_Resume(JitCode* stubCode, uint32_t pcOffset, uint8_t* addr)
      : ICStub(ICStub::RetSub_Resume, stubCode),
        pcOffset_(pcOffset),
        addr_(addr)
    { }

  public:
    static size_t offsetOfPCOffset() {
        return offsetof(ICRetSub_Resume, pcOffset_);
    }
    static size_t offsetOfAddr() {
        return offsetof(ICRetSub_Resume, addr_);
    }

    class Compiler : public ICStubCompiler {
        uint32_t pcOffset_;
        uint8_t* addr_;

        MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) override;

      public:
        Compiler(JSContext* cx, uint32_t pcOffset, uint8_t* addr)
          : ICStubCompiler(cx, ICStub::RetSub_Resume, Engine::Baseline),
            pcOffset_(pcOffset),
            addr_(addr)
        { }

        ICStub* getStub(ICStubSpace* space) override {
            return newStub<ICRetSub_Resume>(space, getStubCode(), pcOffset_, addr_);
        }
    };
};

inline bool
IsCacheableDOMProxy(JSObject* obj)
{
    if (!obj->is<ProxyObject>())
        return false;

    const BaseProxyHandler* handler = obj->as<ProxyObject>().handler();
    return handler->family() == GetDOMProxyHandlerFamily();
}

struct IonOsrTempData;

template <typename T>
void EmitICUnboxedPreBarrier(MacroAssembler &masm, const T& address, JSValueType type);

// Write an arbitrary value to a typed array or typed object address at dest.
// If the value could not be converted to the appropriate format, jump to
// failure.
template <typename T>
void StoreToTypedArray(JSContext* cx, MacroAssembler& masm, Scalar::Type type,
                       const ValueOperand& value, const T& dest, Register scratch,
                       Label* failure);

} // namespace jit
} // namespace js

#endif /* jit_BaselineIC_h */
