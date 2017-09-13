/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineIC_h
#define jit_BaselineIC_h

#include "mozilla/Assertions.h"

#include "jscntxt.h"
#include "jscompartment.h"
#include "jsgc.h"
#include "jsopcode.h"

#include "builtin/TypedObject.h"
#include "jit/BaselineICList.h"
#include "jit/BaselineJIT.h"
#include "jit/SharedIC.h"
#include "jit/SharedICRegisters.h"
#include "js/TraceableVector.h"
#include "vm/ArrayObject.h"
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::WarmUpCounter_Fallback, Engine::Baseline)
        { }

        ICWarmUpCounter_Fallback* getStub(ICStubSpace* space) {
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::TypeUpdate_Fallback, Engine::Baseline)
        { }

        ICTypeUpdate_Fallback* getStub(ICStubSpace* space) {
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ICTypeUpdate_PrimitiveSet* existingStub, JSValueType type)
          : TypeCheckPrimitiveSetStub::Compiler(cx, TypeUpdate_PrimitiveSet,
                                                Engine::Baseline, existingStub, type)
        {}

        ICTypeUpdate_PrimitiveSet* updateStub() {
            TypeCheckPrimitiveSetStub* stub =
                this->TypeCheckPrimitiveSetStub::Compiler::updateStub();
            if (!stub)
                return nullptr;
            return stub->toUpdateStub();
        }

        ICTypeUpdate_PrimitiveSet* getStub(ICStubSpace* space) {
            MOZ_ASSERT(!existingStub_);
            return newStub<ICTypeUpdate_PrimitiveSet>(space, getStubCode(), flags_);
        }
    };
};

// Type update stub to handle a singleton object.
class ICTypeUpdate_SingleObject : public ICStub
{
    friend class ICStubSpace;

    HeapPtrObject obj_;

    ICTypeUpdate_SingleObject(JitCode* stubCode, JSObject* obj);

  public:
    HeapPtrObject& object() {
        return obj_;
    }

    static size_t offsetOfObject() {
        return offsetof(ICTypeUpdate_SingleObject, obj_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        HandleObject obj_;
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, HandleObject obj)
          : ICStubCompiler(cx, TypeUpdate_SingleObject, Engine::Baseline),
            obj_(obj)
        { }

        ICTypeUpdate_SingleObject* getStub(ICStubSpace* space) {
            return newStub<ICTypeUpdate_SingleObject>(space, getStubCode(), obj_);
        }
    };
};

// Type update stub to handle a single ObjectGroup.
class ICTypeUpdate_ObjectGroup : public ICStub
{
    friend class ICStubSpace;

    HeapPtrObjectGroup group_;

    ICTypeUpdate_ObjectGroup(JitCode* stubCode, ObjectGroup* group);

  public:
    HeapPtrObjectGroup& group() {
        return group_;
    }

    static size_t offsetOfGroup() {
        return offsetof(ICTypeUpdate_ObjectGroup, group_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        HandleObjectGroup group_;
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, HandleObjectGroup group)
          : ICStubCompiler(cx, TypeUpdate_ObjectGroup, Engine::Baseline),
            group_(group)
        { }

        ICTypeUpdate_ObjectGroup* getStub(ICStubSpace* space) {
            return newStub<ICTypeUpdate_ObjectGroup>(space, getStubCode(), group_);
        }
    };
};

class ICNewArray_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    HeapPtrObject templateObject_;

    // The group used for objects created here is always available, even if the
    // template object itself is not.
    HeapPtrObjectGroup templateGroup_;

    ICNewArray_Fallback(JitCode* stubCode, ObjectGroup* templateGroup)
      : ICFallbackStub(ICStub::NewArray_Fallback, stubCode),
        templateObject_(nullptr), templateGroup_(templateGroup)
    {}

  public:
    class Compiler : public ICStubCompiler {
        RootedObjectGroup templateGroup;
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ObjectGroup* templateGroup)
          : ICStubCompiler(cx, ICStub::NewArray_Fallback, Engine::Baseline),
            templateGroup(cx, templateGroup)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICNewArray_Fallback>(space, getStubCode(), templateGroup);
        }
    };

    HeapPtrObject& templateObject() {
        return templateObject_;
    }

    void setTemplateObject(JSObject* obj) {
        MOZ_ASSERT(obj->group() == templateGroup());
        templateObject_ = obj;
    }

    HeapPtrObjectGroup& templateGroup() {
        return templateGroup_;
    }

    void setTemplateGroup(ObjectGroup* group) {
        templateObject_ = nullptr;
        templateGroup_ = group;
    }
};

class ICNewObject_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    HeapPtrObject templateObject_;

    explicit ICNewObject_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::NewObject_Fallback, stubCode), templateObject_(nullptr)
    {}

  public:
    class Compiler : public ICStubCompiler {
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::NewObject_Fallback, Engine::Baseline)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICNewObject_Fallback>(space, getStubCode());
        }
    };

    HeapPtrObject& templateObject() {
        return templateObject_;
    }

    void setTemplateObject(JSObject* obj) {
        templateObject_ = obj;
    }
};

class ICNewObject_WithTemplate : public ICStub
{
    friend class ICStubSpace;

    explicit ICNewObject_WithTemplate(JitCode* stubCode)
      : ICStub(ICStub::NewObject_WithTemplate, stubCode)
    {}
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::ToBool_Fallback, Engine::Baseline) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICToBool_Fallback>(space, getStubCode());
        }
    };
};

class ICToBool_Int32 : public ICStub
{
    friend class ICStubSpace;

    explicit ICToBool_Int32(JitCode* stubCode)
      : ICStub(ICStub::ToBool_Int32, stubCode) {}

  public:
    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::ToBool_Int32, Engine::Baseline) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICToBool_Int32>(space, getStubCode());
        }
    };
};

class ICToBool_String : public ICStub
{
    friend class ICStubSpace;

    explicit ICToBool_String(JitCode* stubCode)
      : ICStub(ICStub::ToBool_String, stubCode) {}

  public:
    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::ToBool_String, Engine::Baseline) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICToBool_String>(space, getStubCode());
        }
    };
};

class ICToBool_NullUndefined : public ICStub
{
    friend class ICStubSpace;

    explicit ICToBool_NullUndefined(JitCode* stubCode)
      : ICStub(ICStub::ToBool_NullUndefined, stubCode) {}

  public:
    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::ToBool_NullUndefined, Engine::Baseline) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICToBool_NullUndefined>(space, getStubCode());
        }
    };
};

class ICToBool_Double : public ICStub
{
    friend class ICStubSpace;

    explicit ICToBool_Double(JitCode* stubCode)
      : ICStub(ICStub::ToBool_Double, stubCode) {}

  public:
    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::ToBool_Double, Engine::Baseline) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICToBool_Double>(space, getStubCode());
        }
    };
};

class ICToBool_Object : public ICStub
{
    friend class ICStubSpace;

    explicit ICToBool_Object(JitCode* stubCode)
      : ICStub(ICStub::ToBool_Object, stubCode) {}

  public:
    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::ToBool_Object, Engine::Baseline) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICToBool_Object>(space, getStubCode());
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::ToNumber_Fallback, Engine::Baseline) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICToNumber_Fallback>(space, getStubCode());
        }
    };
};

// GetElem
//      JSOP_GETELEM

class ICGetElem_Fallback : public ICMonitoredFallbackStub
{
    friend class ICStubSpace;

    explicit ICGetElem_Fallback(JitCode* stubCode)
      : ICMonitoredFallbackStub(ICStub::GetElem_Fallback, stubCode)
    { }

    static const uint16_t EXTRA_NON_NATIVE = 0x1;
    static const uint16_t EXTRA_NEGATIVE_INDEX = 0x2;
    static const uint16_t EXTRA_UNOPTIMIZABLE_ACCESS = 0x4;

  public:
    static const uint32_t MAX_OPTIMIZED_STUBS = 16;

    void noteNonNativeAccess() {
        extra_ |= EXTRA_NON_NATIVE;
    }
    bool hasNonNativeAccess() const {
        return extra_ & EXTRA_NON_NATIVE;
    }

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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::GetElem_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
            ICGetElem_Fallback* stub = newStub<ICGetElem_Fallback>(space, getStubCode());
            if (!stub)
                return nullptr;
            if (!stub->initMonitoringChain(cx, space, engine_))
                return nullptr;
            return stub;
        }
    };
};

class ICGetElemNativeStub : public ICMonitoredStub
{
  public:
    enum AccessType { FixedSlot = 0, DynamicSlot, UnboxedProperty, NativeGetter, ScriptedGetter };

  protected:
    HeapReceiverGuard receiverGuard_;

    static const unsigned NEEDS_ATOMIZE_SHIFT = 0;
    static const uint16_t NEEDS_ATOMIZE_MASK = 0x1;

    static const unsigned ACCESSTYPE_SHIFT = 1;
    static const uint16_t ACCESSTYPE_MASK = 0x3;

    static const unsigned ISSYMBOL_SHIFT = 3;
    static const uint16_t ISSYMBOL_MASK = 0x1;

    ICGetElemNativeStub(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                        ReceiverGuard guard, AccessType acctype, bool needsAtomize, bool isSymbol);

    ~ICGetElemNativeStub();

  public:
    HeapReceiverGuard& receiverGuard() {
        return receiverGuard_;
    }
    static size_t offsetOfReceiverGuard() {
        return offsetof(ICGetElemNativeStub, receiverGuard_);
    }

    AccessType accessType() const {
        return static_cast<AccessType>((extra_ >> ACCESSTYPE_SHIFT) & ACCESSTYPE_MASK);
    }

    bool needsAtomize() const {
        return (extra_ >> NEEDS_ATOMIZE_SHIFT) & NEEDS_ATOMIZE_MASK;
    }

    bool isSymbol() const {
        return (extra_ >> ISSYMBOL_SHIFT) & ISSYMBOL_MASK;
    }
};

template <class T>
class ICGetElemNativeStubImpl : public ICGetElemNativeStub
{
  protected:
    HeapPtr<T> key_;

    ICGetElemNativeStubImpl(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                            ReceiverGuard guard, const T* key, AccessType acctype, bool needsAtomize)
      : ICGetElemNativeStub(kind, stubCode, firstMonitorStub, guard, acctype, needsAtomize,
                            mozilla::IsSame<T, JS::Symbol*>::value),
        key_(*key)
    {}

  public:
    HeapPtr<T>& key() {
        return key_;
    }
    static size_t offsetOfKey() {
        return offsetof(ICGetElemNativeStubImpl, key_);
    }
};

typedef ICGetElemNativeStub::AccessType AccType;

template <class T>
class ICGetElemNativeSlotStub : public ICGetElemNativeStubImpl<T>
{
  protected:
    uint32_t offset_;

    ICGetElemNativeSlotStub(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                            ReceiverGuard guard, const T* key, AccType acctype, bool needsAtomize,
                            uint32_t offset)
      : ICGetElemNativeStubImpl<T>(kind, stubCode, firstMonitorStub, guard, key, acctype, needsAtomize),
        offset_(offset)
    {
        MOZ_ASSERT(kind == ICStub::GetElem_NativeSlotName ||
                   kind == ICStub::GetElem_NativeSlotSymbol ||
                   kind == ICStub::GetElem_NativePrototypeSlotName ||
                   kind == ICStub::GetElem_NativePrototypeSlotSymbol ||
                   kind == ICStub::GetElem_UnboxedPropertyName);
        MOZ_ASSERT(acctype == ICGetElemNativeStub::FixedSlot ||
                   acctype == ICGetElemNativeStub::DynamicSlot ||
                   acctype == ICGetElemNativeStub::UnboxedProperty);
    }

  public:
    uint32_t offset() const {
        return offset_;
    }

    static size_t offsetOfOffset() {
        return offsetof(ICGetElemNativeSlotStub, offset_);
    }
};

template <class T>
class ICGetElemNativeGetterStub : public ICGetElemNativeStubImpl<T>
{
  protected:
    HeapPtrFunction getter_;
    uint32_t pcOffset_;

    ICGetElemNativeGetterStub(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                              ReceiverGuard guard, const T* key, AccType acctype, bool needsAtomize,
                              JSFunction* getter, uint32_t pcOffset);

  public:
    HeapPtrFunction& getter() {
        return getter_;
    }
    static size_t offsetOfGetter() {
        return offsetof(ICGetElemNativeGetterStub, getter_);
    }

    static size_t offsetOfPCOffset() {
        return offsetof(ICGetElemNativeGetterStub, pcOffset_);
    }
};

template <class T>
ICStub::Kind
getGetElemStubKind(ICStub::Kind kind)
{
    MOZ_ASSERT(kind == ICStub::GetElem_NativeSlotName ||
               kind == ICStub::GetElem_NativePrototypeSlotName ||
               kind == ICStub::GetElem_NativePrototypeCallNativeName ||
               kind == ICStub::GetElem_NativePrototypeCallScriptedName);
    return static_cast<ICStub::Kind>(kind + mozilla::IsSame<T, JS::Symbol*>::value);
}

template <class T>
class ICGetElem_NativeSlot : public ICGetElemNativeSlotStub<T>
{
    friend class ICStubSpace;
    ICGetElem_NativeSlot(JitCode* stubCode, ICStub* firstMonitorStub, ReceiverGuard guard,
                         const T* key, AccType acctype, bool needsAtomize, uint32_t offset)
      : ICGetElemNativeSlotStub<T>(getGetElemStubKind<T>(ICStub::GetElem_NativeSlotName),
                                   stubCode, firstMonitorStub, guard,
                                   key, acctype, needsAtomize, offset)
    {}
};

class ICGetElem_NativeSlotName :
      public ICGetElem_NativeSlot<PropertyName*>
{};
class ICGetElem_NativeSlotSymbol :
      public ICGetElem_NativeSlot<JS::Symbol*>
{};

template <class T>
class ICGetElem_UnboxedProperty : public ICGetElemNativeSlotStub<T>
{
    friend class ICStubSpace;
    ICGetElem_UnboxedProperty(JitCode* stubCode, ICStub* firstMonitorStub,
                              ReceiverGuard guard, const T* key, AccType acctype,
                              bool needsAtomize, uint32_t offset)
      : ICGetElemNativeSlotStub<T>(ICStub::GetElem_UnboxedPropertyName, stubCode, firstMonitorStub,
                                   guard, key, acctype, needsAtomize, offset)
     {}
};

class ICGetElem_UnboxedPropertyName :
      public ICGetElem_UnboxedProperty<PropertyName*>
{};

template <class T>
class ICGetElem_NativePrototypeSlot : public ICGetElemNativeSlotStub<T>
{
    friend class ICStubSpace;
    HeapPtrObject holder_;
    HeapPtrShape holderShape_;

    ICGetElem_NativePrototypeSlot(JitCode* stubCode, ICStub* firstMonitorStub, ReceiverGuard guard,
                                  const T* key, AccType acctype, bool needsAtomize, uint32_t offset,
                                  JSObject* holder, Shape* holderShape);

  public:
    HeapPtrObject& holder() {
        return holder_;
    }
    static size_t offsetOfHolder() {
        return offsetof(ICGetElem_NativePrototypeSlot, holder_);
    }

    HeapPtrShape& holderShape() {
        return holderShape_;
    }
    static size_t offsetOfHolderShape() {
        return offsetof(ICGetElem_NativePrototypeSlot, holderShape_);
    }
};

class ICGetElem_NativePrototypeSlotName :
      public ICGetElem_NativePrototypeSlot<PropertyName*>
{};
class ICGetElem_NativePrototypeSlotSymbol :
      public ICGetElem_NativePrototypeSlot<JS::Symbol*>
{};

template <class T>
class ICGetElemNativePrototypeCallStub : public ICGetElemNativeGetterStub<T>
{
    friend class ICStubSpace;
    HeapPtrObject holder_;
    HeapPtrShape holderShape_;

  protected:
    ICGetElemNativePrototypeCallStub(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                                     ReceiverGuard guard, const T* key, AccType acctype,
                                     bool needsAtomize, JSFunction* getter, uint32_t pcOffset,
                                     JSObject* holder, Shape* holderShape);

  public:
    HeapPtrObject& holder() {
        return holder_;
    }
    static size_t offsetOfHolder() {
        return offsetof(ICGetElemNativePrototypeCallStub, holder_);
    }

    HeapPtrShape& holderShape() {
        return holderShape_;
    }
    static size_t offsetOfHolderShape() {
        return offsetof(ICGetElemNativePrototypeCallStub, holderShape_);
    }
};

template <class T>
class ICGetElem_NativePrototypeCallNative : public ICGetElemNativePrototypeCallStub<T>
{
    friend class ICStubSpace;

    ICGetElem_NativePrototypeCallNative(JitCode* stubCode, ICStub* firstMonitorStub,
                                        ReceiverGuard guard, const T* key, AccType acctype,
                                        bool needsAtomize, JSFunction* getter, uint32_t pcOffset,
                                        JSObject* holder, Shape* holderShape)
      : ICGetElemNativePrototypeCallStub<T>(getGetElemStubKind<T>(
                                            ICStub::GetElem_NativePrototypeCallNativeName),
                                            stubCode, firstMonitorStub, guard, key,
                                            acctype, needsAtomize, getter, pcOffset, holder,
                                            holderShape)
    {}

  public:
    static ICGetElem_NativePrototypeCallNative<T>* Clone(JSContext* cx, ICStubSpace* space,
                                                         ICStub* firstMonitorStub,
                                                         ICGetElem_NativePrototypeCallNative<T>& other);
};

class ICGetElem_NativePrototypeCallNativeName :
      public ICGetElem_NativePrototypeCallNative<PropertyName*>
{};
class ICGetElem_NativePrototypeCallNativeSymbol :
      public ICGetElem_NativePrototypeCallNative<JS::Symbol*>
{};

template <class T>
class ICGetElem_NativePrototypeCallScripted : public ICGetElemNativePrototypeCallStub<T>
{
    friend class ICStubSpace;

    ICGetElem_NativePrototypeCallScripted(JitCode* stubCode, ICStub* firstMonitorStub,
                                          ReceiverGuard guard, const T* key, AccType acctype,
                                          bool needsAtomize, JSFunction* getter, uint32_t pcOffset,
                                          JSObject* holder, Shape* holderShape)
      : ICGetElemNativePrototypeCallStub<T>(getGetElemStubKind<T>(
                                            ICStub::GetElem_NativePrototypeCallScriptedName),
                                            stubCode, firstMonitorStub, guard, key, acctype,
                                            needsAtomize, getter, pcOffset, holder, holderShape)
    {}

  public:
    static ICGetElem_NativePrototypeCallScripted<T>*
    Clone(JSContext* cx, ICStubSpace* space,
          ICStub* firstMonitorStub,
          ICGetElem_NativePrototypeCallScripted<T>& other);
};

class ICGetElem_NativePrototypeCallScriptedName :
      public ICGetElem_NativePrototypeCallScripted<PropertyName*>
{};
class ICGetElem_NativePrototypeCallScriptedSymbol :
      public ICGetElem_NativePrototypeCallScripted<JS::Symbol*>
{};

// Compiler for GetElem_NativeSlot and GetElem_NativePrototypeSlot stubs.
template <class T>
class ICGetElemNativeCompiler : public ICStubCompiler
{
    ICStub* firstMonitorStub_;
    HandleObject obj_;
    HandleObject holder_;
    Handle<T> key_;
    AccType acctype_;
    bool needsAtomize_;
    uint32_t offset_;
    JSValueType unboxedType_;
    HandleFunction getter_;
    uint32_t pcOffset_;

    bool emitCheckKey(MacroAssembler& masm, Label& failure);
    bool emitCallNative(MacroAssembler& masm, Register objReg);
    bool emitCallScripted(MacroAssembler& masm, Register objReg);
    bool generateStubCode(MacroAssembler& masm);

  protected:
    virtual int32_t getKey() const {
        MOZ_ASSERT(static_cast<int32_t>(acctype_) <= 7);
        MOZ_ASSERT(static_cast<int32_t>(unboxedType_) <= 8);
        return static_cast<int32_t>(engine_) |
              (static_cast<int32_t>(kind) << 1) |
              (static_cast<int32_t>(needsAtomize_) << 17) |
              (static_cast<int32_t>(acctype_) << 18) |
              (static_cast<int32_t>(unboxedType_) << 21) |
              (static_cast<int32_t>(mozilla::IsSame<JS::Symbol*, T>::value) << 25) |
              (HeapReceiverGuard::keyBits(obj_) << 26);
    }

  public:
    ICGetElemNativeCompiler(JSContext* cx, ICStub::Kind kind, ICStub* firstMonitorStub,
                            HandleObject obj, HandleObject holder, Handle<T> key, AccType acctype,
                            bool needsAtomize, uint32_t offset,
                            JSValueType unboxedType = JSVAL_TYPE_MAGIC)
      : ICStubCompiler(cx, kind, Engine::Baseline),
        firstMonitorStub_(firstMonitorStub),
        obj_(obj),
        holder_(holder),
        key_(key),
        acctype_(acctype),
        needsAtomize_(needsAtomize),
        offset_(offset),
        unboxedType_(unboxedType),
        getter_(nullptr),
        pcOffset_(0)
    {}

    ICGetElemNativeCompiler(JSContext* cx, ICStub::Kind kind, ICStub* firstMonitorStub,
                            HandleObject obj, HandleObject holder, Handle<T> key, AccType acctype,
                            bool needsAtomize, HandleFunction getter, uint32_t pcOffset)
      : ICStubCompiler(cx, kind, Engine::Baseline),
        firstMonitorStub_(firstMonitorStub),
        obj_(obj),
        holder_(holder),
        key_(key),
        acctype_(acctype),
        needsAtomize_(needsAtomize),
        offset_(0),
        unboxedType_(JSVAL_TYPE_MAGIC),
        getter_(getter),
        pcOffset_(pcOffset)
    {}

    ICStub* getStub(ICStubSpace* space) {
        RootedReceiverGuard guard(cx, ReceiverGuard(obj_));
        if (kind == ICStub::GetElem_NativeSlotName || kind == ICStub::GetElem_NativeSlotSymbol) {
            MOZ_ASSERT(obj_ == holder_);
            return newStub<ICGetElem_NativeSlot<T>>(
                    space, getStubCode(), firstMonitorStub_, guard, key_.address(), acctype_,
                    needsAtomize_, offset_);
        }

	if (kind == ICStub::GetElem_UnboxedPropertyName) {
            MOZ_ASSERT(obj_ == holder_);
            return newStub<ICGetElem_UnboxedProperty<T>>(
                    space, getStubCode(), firstMonitorStub_, guard, key_.address(), acctype_,
                    needsAtomize_, offset_);
        }

        MOZ_ASSERT(obj_ != holder_);
        RootedShape holderShape(cx, holder_->as<NativeObject>().lastProperty());
        if (kind == ICStub::GetElem_NativePrototypeSlotName ||
            kind == ICStub::GetElem_NativePrototypeSlotSymbol)
        {
            return newStub<ICGetElem_NativePrototypeSlot<T>>(
                    space, getStubCode(), firstMonitorStub_, guard, key_.address(), acctype_,
                    needsAtomize_, offset_, holder_, holderShape);
        }

        if (kind == ICStub::GetElem_NativePrototypeCallNativeSymbol ||
            kind == ICStub::GetElem_NativePrototypeCallNativeName) {
            return newStub<ICGetElem_NativePrototypeCallNative<T>>(
                    space, getStubCode(), firstMonitorStub_, guard, key_.address(), acctype_,
                    needsAtomize_, getter_, pcOffset_, holder_, holderShape);
        }

        MOZ_ASSERT(kind == ICStub::GetElem_NativePrototypeCallScriptedName ||
                   kind == ICStub::GetElem_NativePrototypeCallScriptedSymbol);
        if (kind == ICStub::GetElem_NativePrototypeCallScriptedName ||
            kind == ICStub::GetElem_NativePrototypeCallScriptedSymbol) {
            return newStub<ICGetElem_NativePrototypeCallScripted<T>>(
                    space, getStubCode(), firstMonitorStub_, guard, key_.address(), acctype_,
                    needsAtomize_, getter_, pcOffset_, holder_, holderShape);
        }

        MOZ_CRASH("Invalid kind.");
    }
};

class ICGetElem_String : public ICStub
{
    friend class ICStubSpace;

    explicit ICGetElem_String(JitCode* stubCode)
      : ICStub(ICStub::GetElem_String, stubCode) {}

  public:
    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::GetElem_String, Engine::Baseline) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetElem_String>(space, getStubCode());
        }
    };
};

class ICGetElem_Dense : public ICMonitoredStub
{
    friend class ICStubSpace;

    HeapPtrShape shape_;

    ICGetElem_Dense(JitCode* stubCode, ICStub* firstMonitorStub, Shape* shape);

  public:
    static ICGetElem_Dense* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                  ICGetElem_Dense& other);

    static size_t offsetOfShape() {
        return offsetof(ICGetElem_Dense, shape_);
    }

    HeapPtrShape& shape() {
        return shape_;
    }

    class Compiler : public ICStubCompiler {
      ICStub* firstMonitorStub_;
      RootedShape shape_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, Shape* shape)
          : ICStubCompiler(cx, ICStub::GetElem_Dense, Engine::Baseline),
            firstMonitorStub_(firstMonitorStub),
            shape_(cx, shape)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetElem_Dense>(space, getStubCode(), firstMonitorStub_, shape_);
        }
    };
};

class ICGetElem_UnboxedArray : public ICMonitoredStub
{
    friend class ICStubSpace;

    HeapPtrObjectGroup group_;

    ICGetElem_UnboxedArray(JitCode* stubCode, ICStub* firstMonitorStub, ObjectGroup* group);

  public:
    static ICGetElem_UnboxedArray* Clone(JSContext* cx, ICStubSpace* space,
                                         ICStub* firstMonitorStub, ICGetElem_UnboxedArray& other);

    static size_t offsetOfGroup() {
        return offsetof(ICGetElem_UnboxedArray, group_);
    }

    HeapPtrObjectGroup& group() {
        return group_;
    }

    class Compiler : public ICStubCompiler {
      ICStub* firstMonitorStub_;
      RootedObjectGroup group_;
      JSValueType elementType_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(elementType_) << 17);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, ObjectGroup* group)
          : ICStubCompiler(cx, ICStub::GetElem_UnboxedArray, Engine::Baseline),
            firstMonitorStub_(firstMonitorStub),
            group_(cx, group),
            elementType_(group->unboxedLayoutDontCheckGeneration().elementType())
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetElem_UnboxedArray>(space, getStubCode(), firstMonitorStub_, group_);
        }
    };
};

// Accesses scalar elements of a typed array or typed object.
class ICGetElem_TypedArray : public ICStub
{
    friend class ICStubSpace;

  protected: // Protected to silence Clang warning.
    HeapPtrShape shape_;

    ICGetElem_TypedArray(JitCode* stubCode, Shape* shape, Scalar::Type type);

  public:
    static size_t offsetOfShape() {
        return offsetof(ICGetElem_TypedArray, shape_);
    }

    HeapPtrShape& shape() {
        return shape_;
    }

    class Compiler : public ICStubCompiler {
      RootedShape shape_;
      Scalar::Type type_;
      TypedThingLayout layout_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(type_) << 17) |
                  (static_cast<int32_t>(layout_) << 25);
        }

      public:
        Compiler(JSContext* cx, Shape* shape, Scalar::Type type)
          : ICStubCompiler(cx, ICStub::GetElem_TypedArray, Engine::Baseline),
            shape_(cx, shape),
            type_(type),
            layout_(GetTypedThingLayout(shape->getObjectClass()))
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetElem_TypedArray>(space, getStubCode(), shape_, type_);
        }
    };
};

class ICGetElem_Arguments : public ICMonitoredStub
{
    friend class ICStubSpace;
  public:
    enum Which { Mapped, Unmapped, Magic };

  private:
    ICGetElem_Arguments(JitCode* stubCode, ICStub* firstMonitorStub, Which which)
      : ICMonitoredStub(ICStub::GetElem_Arguments, stubCode, firstMonitorStub)
    {
        extra_ = static_cast<uint16_t>(which);
    }

  public:
    static ICGetElem_Arguments* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                      ICGetElem_Arguments& other);

    Which which() const {
        return static_cast<Which>(extra_);
    }

    class Compiler : public ICStubCompiler {
      ICStub* firstMonitorStub_;
      Which which_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(which_) << 17);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, Which which)
          : ICStubCompiler(cx, ICStub::GetElem_Arguments, Engine::Baseline),
            firstMonitorStub_(firstMonitorStub),
            which_(which)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetElem_Arguments>(space, getStubCode(), firstMonitorStub_, which_);
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

  public:
    static const uint32_t MAX_OPTIMIZED_STUBS = 8;

    void noteArrayWriteHole() {
        extra_ = 1;
    }
    bool hasArrayWriteHole() const {
        return extra_;
    }

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::SetElem_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICSetElem_Fallback>(space, getStubCode());
        }
    };
};

class ICSetElem_DenseOrUnboxedArray : public ICUpdatedStub
{
    friend class ICStubSpace;

    HeapPtrShape shape_; // null for unboxed arrays
    HeapPtrObjectGroup group_;

    ICSetElem_DenseOrUnboxedArray(JitCode* stubCode, Shape* shape, ObjectGroup* group);

  public:
    static size_t offsetOfShape() {
        return offsetof(ICSetElem_DenseOrUnboxedArray, shape_);
    }
    static size_t offsetOfGroup() {
        return offsetof(ICSetElem_DenseOrUnboxedArray, group_);
    }

    HeapPtrShape& shape() {
        return shape_;
    }
    HeapPtrObjectGroup& group() {
        return group_;
    }

    class Compiler : public ICStubCompiler {
        RootedShape shape_;
        RootedObjectGroup group_;
        JSValueType unboxedType_;

        bool generateStubCode(MacroAssembler& masm);

      public:
        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(unboxedType_) << 17);
        }

        Compiler(JSContext* cx, Shape* shape, HandleObjectGroup group)
          : ICStubCompiler(cx, ICStub::SetElem_DenseOrUnboxedArray, Engine::Baseline),
            shape_(cx, shape),
            group_(cx, group),
            unboxedType_(shape
                         ? JSVAL_TYPE_MAGIC
                         : group->unboxedLayoutDontCheckGeneration().elementType())
        {}

        ICUpdatedStub* getStub(ICStubSpace* space) {
            ICSetElem_DenseOrUnboxedArray* stub =
                newStub<ICSetElem_DenseOrUnboxedArray>(space, getStubCode(), shape_, group_);
            if (!stub || !stub->initUpdatingChain(cx, space))
                return nullptr;
            return stub;
        }

        bool needsUpdateStubs() {
            return unboxedType_ == JSVAL_TYPE_MAGIC || unboxedType_ == JSVAL_TYPE_OBJECT;
        }
    };
};

template <size_t ProtoChainDepth> class ICSetElem_DenseOrUnboxedArrayAddImpl;

class ICSetElem_DenseOrUnboxedArrayAdd : public ICUpdatedStub
{
    friend class ICStubSpace;

  public:
    static const size_t MAX_PROTO_CHAIN_DEPTH = 4;

  protected:
    HeapPtrObjectGroup group_;

    ICSetElem_DenseOrUnboxedArrayAdd(JitCode* stubCode, ObjectGroup* group, size_t protoChainDepth);

  public:
    static size_t offsetOfGroup() {
        return offsetof(ICSetElem_DenseOrUnboxedArrayAdd, group_);
    }

    HeapPtrObjectGroup& group() {
        return group_;
    }
    size_t protoChainDepth() const {
        MOZ_ASSERT(extra_ <= MAX_PROTO_CHAIN_DEPTH);
        return extra_;
    }

    template <size_t ProtoChainDepth>
    ICSetElem_DenseOrUnboxedArrayAddImpl<ProtoChainDepth>* toImplUnchecked() {
        return static_cast<ICSetElem_DenseOrUnboxedArrayAddImpl<ProtoChainDepth>*>(this);
    }

    template <size_t ProtoChainDepth>
    ICSetElem_DenseOrUnboxedArrayAddImpl<ProtoChainDepth>* toImpl() {
        MOZ_ASSERT(ProtoChainDepth == protoChainDepth());
        return toImplUnchecked<ProtoChainDepth>();
    }
};

template <size_t ProtoChainDepth>
class ICSetElem_DenseOrUnboxedArrayAddImpl : public ICSetElem_DenseOrUnboxedArrayAdd
{
    friend class ICStubSpace;

    // Note: for unboxed arrays, the first shape is null.
    static const size_t NumShapes = ProtoChainDepth + 1;
    mozilla::Array<HeapPtrShape, NumShapes> shapes_;

    ICSetElem_DenseOrUnboxedArrayAddImpl(JitCode* stubCode, ObjectGroup* group,
                                         Handle<ShapeVector> shapes)
      : ICSetElem_DenseOrUnboxedArrayAdd(stubCode, group, ProtoChainDepth)
    {
        MOZ_ASSERT(shapes.length() == NumShapes);
        for (size_t i = 0; i < NumShapes; i++)
            shapes_[i].init(shapes[i]);
    }

  public:
    void traceShapes(JSTracer* trc) {
        for (size_t i = 0; i < NumShapes; i++) {
            if (shapes_[i])
                TraceEdge(trc, &shapes_[i], "baseline-setelem-denseadd-stub-shape");
        }
    }
    Shape* shape(size_t i) const {
        MOZ_ASSERT(i < NumShapes);
        return shapes_[i];
    }
    static size_t offsetOfShape(size_t idx) {
        return offsetof(ICSetElem_DenseOrUnboxedArrayAddImpl, shapes_) + idx * sizeof(HeapPtrShape);
    }
};

class ICSetElemDenseOrUnboxedArrayAddCompiler : public ICStubCompiler {
    RootedObject obj_;
    size_t protoChainDepth_;
    JSValueType unboxedType_;

    bool generateStubCode(MacroAssembler& masm);

  protected:
    virtual int32_t getKey() const {
        return static_cast<int32_t>(engine_) |
              (static_cast<int32_t>(kind) << 1) |
              (static_cast<int32_t>(protoChainDepth_) << 17) |
              (static_cast<int32_t>(unboxedType_) << 20);
    }

  public:
    ICSetElemDenseOrUnboxedArrayAddCompiler(JSContext* cx, HandleObject obj, size_t protoChainDepth)
        : ICStubCompiler(cx, ICStub::SetElem_DenseOrUnboxedArrayAdd, Engine::Baseline),
          obj_(cx, obj),
          protoChainDepth_(protoChainDepth),
          unboxedType_(obj->is<UnboxedArrayObject>()
                       ? obj->as<UnboxedArrayObject>().elementType()
                       : JSVAL_TYPE_MAGIC)
    {}

    template <size_t ProtoChainDepth>
    ICUpdatedStub* getStubSpecific(ICStubSpace* space, Handle<ShapeVector> shapes);

    ICUpdatedStub* getStub(ICStubSpace* space);

    bool needsUpdateStubs() {
        return unboxedType_ == JSVAL_TYPE_MAGIC || unboxedType_ == JSVAL_TYPE_OBJECT;
    }
};

// Accesses scalar elements of a typed array or typed object.
class ICSetElem_TypedArray : public ICStub
{
    friend class ICStubSpace;

  protected: // Protected to silence Clang warning.
    HeapPtrShape shape_;

    ICSetElem_TypedArray(JitCode* stubCode, Shape* shape, Scalar::Type type,
                         bool expectOutOfBounds);

  public:
    Scalar::Type type() const {
        return (Scalar::Type) (extra_ & 0xff);
    }

    bool expectOutOfBounds() const {
        return (extra_ >> 8) & 1;
    }

    static size_t offsetOfShape() {
        return offsetof(ICSetElem_TypedArray, shape_);
    }

    HeapPtrShape& shape() {
        return shape_;
    }

    class Compiler : public ICStubCompiler {
        RootedShape shape_;
        Scalar::Type type_;
        TypedThingLayout layout_;
        bool expectOutOfBounds_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(type_) << 17) |
                  (static_cast<int32_t>(layout_) << 25) |
                  (static_cast<int32_t>(expectOutOfBounds_) << 29);
        }

      public:
        Compiler(JSContext* cx, Shape* shape, Scalar::Type type, bool expectOutOfBounds)
          : ICStubCompiler(cx, ICStub::SetElem_TypedArray, Engine::Baseline),
            shape_(cx, shape),
            type_(type),
            layout_(GetTypedThingLayout(shape->getObjectClass())),
            expectOutOfBounds_(expectOutOfBounds)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICSetElem_TypedArray>(space, getStubCode(), shape_, type_,
                                                 expectOutOfBounds_);
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
    static const uint32_t MAX_OPTIMIZED_STUBS = 8;

    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::In_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICIn_Fallback>(space, getStubCode());
        }
    };
};

// Base class for In_Native and In_NativePrototype stubs.
class ICInNativeStub : public ICStub
{
    HeapPtrShape shape_;
    HeapPtrPropertyName name_;

  protected:
    ICInNativeStub(ICStub::Kind kind, JitCode* stubCode, HandleShape shape,
                   HandlePropertyName name);

  public:
    HeapPtrShape& shape() {
        return shape_;
    }
    static size_t offsetOfShape() {
        return offsetof(ICInNativeStub, shape_);
    }

    HeapPtrPropertyName& name() {
        return name_;
    }
    static size_t offsetOfName() {
        return offsetof(ICInNativeStub, name_);
    }
};

// Stub for confirming an own property on a native object.
class ICIn_Native : public ICInNativeStub
{
    friend class ICStubSpace;

    ICIn_Native(JitCode* stubCode, HandleShape shape, HandlePropertyName name)
      : ICInNativeStub(In_Native, stubCode, shape, name)
    {}
};

// Stub for confirming a property on a native object's prototype. Note that due to
// the shape teleporting optimization, we only have to guard on the object's shape
// and the holder's shape.
class ICIn_NativePrototype : public ICInNativeStub
{
    friend class ICStubSpace;

    HeapPtrObject holder_;
    HeapPtrShape holderShape_;

    ICIn_NativePrototype(JitCode* stubCode, HandleShape shape, HandlePropertyName name,
                         HandleObject holder, HandleShape holderShape);

  public:
    HeapPtrObject& holder() {
        return holder_;
    }
    HeapPtrShape& holderShape() {
        return holderShape_;
    }
    static size_t offsetOfHolder() {
        return offsetof(ICIn_NativePrototype, holder_);
    }
    static size_t offsetOfHolderShape() {
        return offsetof(ICIn_NativePrototype, holderShape_);
    }
};

// Compiler for In_Native and In_NativePrototype stubs.
class ICInNativeCompiler : public ICStubCompiler
{
    RootedObject obj_;
    RootedObject holder_;
    RootedPropertyName name_;

    bool generateStubCode(MacroAssembler& masm);

  public:
    ICInNativeCompiler(JSContext* cx, ICStub::Kind kind, HandleObject obj, HandleObject holder,
                       HandlePropertyName name)
      : ICStubCompiler(cx, kind, Engine::Baseline),
        obj_(cx, obj),
        holder_(cx, holder),
        name_(cx, name)
    {}

    ICStub* getStub(ICStubSpace* space) {
        RootedShape shape(cx, obj_->as<NativeObject>().lastProperty());
        if (kind == ICStub::In_Native) {
            MOZ_ASSERT(obj_ == holder_);
            return newStub<ICIn_Native>(space, getStubCode(), shape, name_);
        }

        MOZ_ASSERT(obj_ != holder_);
        MOZ_ASSERT(kind == ICStub::In_NativePrototype);
        RootedShape holderShape(cx, holder_->as<NativeObject>().lastProperty());
        return newStub<ICIn_NativePrototype>(space, getStubCode(), shape, name_, holder_,
                                             holderShape);
    }
};

template <size_t ProtoChainDepth> class ICIn_NativeDoesNotExistImpl;

class ICIn_NativeDoesNotExist : public ICStub
{
    friend class ICStubSpace;

    HeapPtrPropertyName name_;

  public:
    static const size_t MAX_PROTO_CHAIN_DEPTH = 8;

  protected:
    ICIn_NativeDoesNotExist(JitCode* stubCode, size_t protoChainDepth,
                            HandlePropertyName name);

  public:
    size_t protoChainDepth() const {
        MOZ_ASSERT(extra_ <= MAX_PROTO_CHAIN_DEPTH);
        return extra_;
    }
    HeapPtrPropertyName& name() {
        return name_;
    }

    template <size_t ProtoChainDepth>
    ICIn_NativeDoesNotExistImpl<ProtoChainDepth>* toImpl() {
        MOZ_ASSERT(ProtoChainDepth == protoChainDepth());
        return static_cast<ICIn_NativeDoesNotExistImpl<ProtoChainDepth>*>(this);
    }

    static size_t offsetOfShape(size_t idx);
    static size_t offsetOfName() {
        return offsetof(ICIn_NativeDoesNotExist, name_);
    }
};

template <size_t ProtoChainDepth>
class ICIn_NativeDoesNotExistImpl : public ICIn_NativeDoesNotExist
{
    friend class ICStubSpace;

  public:
    static const size_t MAX_PROTO_CHAIN_DEPTH = 8;
    static const size_t NumShapes = ProtoChainDepth + 1;

  private:
    mozilla::Array<HeapPtrShape, NumShapes> shapes_;

    ICIn_NativeDoesNotExistImpl(JitCode* stubCode, Handle<ShapeVector> shapes,
                                HandlePropertyName name);

  public:
    void traceShapes(JSTracer* trc) {
        for (size_t i = 0; i < NumShapes; i++)
            TraceEdge(trc, &shapes_[i], "baseline-innativedoesnotexist-stub-shape");
    }

    static size_t offsetOfShape(size_t idx) {
        return offsetof(ICIn_NativeDoesNotExistImpl, shapes_) + (idx * sizeof(HeapPtrShape));
    }
};

class ICInNativeDoesNotExistCompiler : public ICStubCompiler
{
    RootedObject obj_;
    RootedPropertyName name_;
    size_t protoChainDepth_;

  protected:
    virtual int32_t getKey() const {
        return static_cast<int32_t>(engine_) |
              (static_cast<int32_t>(kind) << 1) |
              (static_cast<int32_t>(protoChainDepth_) << 17);
    }

    bool generateStubCode(MacroAssembler& masm);

  public:
    ICInNativeDoesNotExistCompiler(JSContext* cx, HandleObject obj, HandlePropertyName name,
                                   size_t protoChainDepth);

    template <size_t ProtoChainDepth>
    ICStub* getStubSpecific(ICStubSpace* space, Handle<ShapeVector> shapes) {
        return newStub<ICIn_NativeDoesNotExistImpl<ProtoChainDepth>>(space, getStubCode(), shapes,
                                                                     name_);}

    ICStub* getStub(ICStubSpace* space);
};

class ICIn_Dense : public ICStub
{
    friend class ICStubSpace;

    HeapPtrShape shape_;

    ICIn_Dense(JitCode* stubCode, HandleShape shape);

  public:
    HeapPtrShape& shape() {
        return shape_;
    }
    static size_t offsetOfShape() {
        return offsetof(ICIn_Dense, shape_);
    }

    class Compiler : public ICStubCompiler {
      RootedShape shape_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, Shape* shape)
          : ICStubCompiler(cx, ICStub::In_Dense, Engine::Baseline),
            shape_(cx, shape)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICIn_Dense>(space, getStubCode(), shape_);
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
    static const uint32_t MAX_OPTIMIZED_STUBS = 8;
    static const size_t UNOPTIMIZABLE_ACCESS_BIT = 0;

    void noteUnoptimizableAccess() {
        extra_ |= (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }
    bool hadUnoptimizableAccess() const {
        return extra_ & (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }

    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::GetName_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
            ICGetName_Fallback* stub = newStub<ICGetName_Fallback>(space, getStubCode());
            if (!stub || !stub->initMonitoringChain(cx, space, engine_))
                return nullptr;
            return stub;
        }
    };
};

// Optimized lexical GETGNAME stub.
class ICGetName_GlobalLexical : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected: // Protected to silence Clang warning.
    uint32_t slot_;

    ICGetName_GlobalLexical(JitCode* stubCode, ICStub* firstMonitorStub, uint32_t slot);

  public:
    static size_t offsetOfSlot() {
        return offsetof(ICGetName_GlobalLexical, slot_);
    }

    class Compiler : public ICStubCompiler {
        ICStub* firstMonitorStub_;
        uint32_t slot_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t slot)
          : ICStubCompiler(cx, ICStub::GetName_GlobalLexical, Engine::Baseline),
            firstMonitorStub_(firstMonitorStub),
            slot_(slot)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetName_GlobalLexical>(space, getStubCode(), firstMonitorStub_, slot_);
        }
    };
};

// Optimized GETNAME/CALLNAME stub, making a variable number of hops to get an
// 'own' property off some scope object. Unlike GETPROP on an object's
// prototype, there is no teleporting optimization to take advantage of and
// shape checks are required all along the scope chain.
template <size_t NumHops>
class ICGetName_Scope : public ICMonitoredStub
{
    friend class ICStubSpace;

    static const size_t MAX_HOPS = 6;

    mozilla::Array<HeapPtrShape, NumHops + 1> shapes_;
    uint32_t offset_;

    ICGetName_Scope(JitCode* stubCode, ICStub* firstMonitorStub,
                    Handle<ShapeVector> shapes, uint32_t offset);

    static Kind GetStubKind() {
        return (Kind) (GetName_Scope0 + NumHops);
    }

  public:
    void traceScopes(JSTracer* trc) {
        for (size_t i = 0; i < NumHops + 1; i++)
            TraceEdge(trc, &shapes_[i], "baseline-scope-stub-shape");
    }

    static size_t offsetOfShape(size_t index) {
        MOZ_ASSERT(index <= NumHops);
        return offsetof(ICGetName_Scope, shapes_) + (index * sizeof(HeapPtrShape));
    }
    static size_t offsetOfOffset() {
        return offsetof(ICGetName_Scope, offset_);
    }

    class Compiler : public ICStubCompiler {
        ICStub* firstMonitorStub_;
        Rooted<ShapeVector> shapes_;
        bool isFixedSlot_;
        uint32_t offset_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

      protected:
        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(isFixedSlot_) << 17);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub,
                 ShapeVector&& shapes, bool isFixedSlot, uint32_t offset)
          : ICStubCompiler(cx, GetStubKind(), Engine::Baseline),
            firstMonitorStub_(firstMonitorStub),
            shapes_(cx, mozilla::Forward<ShapeVector>(shapes)),
            isFixedSlot_(isFixedSlot),
            offset_(offset)
        {
        }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetName_Scope>(space, getStubCode(), firstMonitorStub_, shapes_,
                                            offset_);
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::BindName_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::GetIntrinsic_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
            ICGetIntrinsic_Fallback* stub =
                newStub<ICGetIntrinsic_Fallback>(space, getStubCode());
            if (!stub || !stub->initMonitoringChain(cx, space, engine_))
                return nullptr;
            return stub;
        }
    };
};

// Stub that loads the constant result of a GETINTRINSIC operation.
class ICGetIntrinsic_Constant : public ICStub
{
    friend class ICStubSpace;

    HeapValue value_;

    ICGetIntrinsic_Constant(JitCode* stubCode, const Value& value);
    ~ICGetIntrinsic_Constant();

  public:
    HeapValue& value() {
        return value_;
    }
    static size_t offsetOfValue() {
        return offsetof(ICGetIntrinsic_Constant, value_);
    }

    class Compiler : public ICStubCompiler {
        bool generateStubCode(MacroAssembler& masm);

        HandleValue value_;

      public:
        Compiler(JSContext* cx, HandleValue value)
          : ICStubCompiler(cx, ICStub::GetIntrinsic_Constant, Engine::Baseline),
            value_(value)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetIntrinsic_Constant>(space, getStubCode(), value_);
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
    static const uint32_t MAX_OPTIMIZED_STUBS = 8;

    static const size_t UNOPTIMIZABLE_ACCESS_BIT = 0;
    void noteUnoptimizableAccess() {
        extra_ |= (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }
    bool hadUnoptimizableAccess() const {
        return extra_ & (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }

    class Compiler : public ICStubCompiler {
      public:
        static const int32_t BASELINE_KEY =
            (static_cast<int32_t>(Engine::Baseline)) |
            (static_cast<int32_t>(ICStub::SetProp_Fallback) << 1);

      protected:
        uint32_t returnOffset_;
        bool generateStubCode(MacroAssembler& masm);
        void postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::SetProp_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICSetProp_Fallback>(space, getStubCode());
        }
    };
};

// Optimized SETPROP/SETGNAME/SETNAME stub.
class ICSetProp_Native : public ICUpdatedStub
{
    friend class ICStubSpace;

  protected: // Protected to silence Clang warning.
    HeapPtrObjectGroup group_;
    HeapPtrShape shape_;
    uint32_t offset_;

    ICSetProp_Native(JitCode* stubCode, ObjectGroup* group, Shape* shape, uint32_t offset);

  public:
    HeapPtrObjectGroup& group() {
        return group_;
    }
    HeapPtrShape& shape() {
        return shape_;
    }
    void notePreliminaryObject() {
        extra_ = 1;
    }
    bool hasPreliminaryObject() const {
        return extra_;
    }
    static size_t offsetOfGroup() {
        return offsetof(ICSetProp_Native, group_);
    }
    static size_t offsetOfShape() {
        return offsetof(ICSetProp_Native, shape_);
    }
    static size_t offsetOfOffset() {
        return offsetof(ICSetProp_Native, offset_);
    }

    class Compiler : public ICStubCompiler {
        RootedObject obj_;
        bool isFixedSlot_;
        uint32_t offset_;

      protected:
        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(isFixedSlot_) << 17) |
                  (static_cast<int32_t>(obj_->is<UnboxedPlainObject>()) << 18);
        }

        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, HandleObject obj, bool isFixedSlot, uint32_t offset)
          : ICStubCompiler(cx, ICStub::SetProp_Native, Engine::Baseline),
            obj_(cx, obj),
            isFixedSlot_(isFixedSlot),
            offset_(offset)
        {}

        ICSetProp_Native* getStub(ICStubSpace* space);
    };
};


template <size_t ProtoChainDepth> class ICSetProp_NativeAddImpl;

class ICSetProp_NativeAdd : public ICUpdatedStub
{
  public:
    static const size_t MAX_PROTO_CHAIN_DEPTH = 4;

  protected: // Protected to silence Clang warning.
    HeapPtrObjectGroup group_;
    HeapPtrShape newShape_;
    HeapPtrObjectGroup newGroup_;
    uint32_t offset_;

    ICSetProp_NativeAdd(JitCode* stubCode, ObjectGroup* group, size_t protoChainDepth,
                        Shape* newShape, ObjectGroup* newGroup, uint32_t offset);

  public:
    size_t protoChainDepth() const {
        return extra_;
    }
    HeapPtrObjectGroup& group() {
        return group_;
    }
    HeapPtrShape& newShape() {
        return newShape_;
    }
    HeapPtrObjectGroup& newGroup() {
        return newGroup_;
    }

    template <size_t ProtoChainDepth>
    ICSetProp_NativeAddImpl<ProtoChainDepth>* toImpl() {
        MOZ_ASSERT(ProtoChainDepth == protoChainDepth());
        return static_cast<ICSetProp_NativeAddImpl<ProtoChainDepth>*>(this);
    }

    static size_t offsetOfGroup() {
        return offsetof(ICSetProp_NativeAdd, group_);
    }
    static size_t offsetOfNewShape() {
        return offsetof(ICSetProp_NativeAdd, newShape_);
    }
    static size_t offsetOfNewGroup() {
        return offsetof(ICSetProp_NativeAdd, newGroup_);
    }
    static size_t offsetOfOffset() {
        return offsetof(ICSetProp_NativeAdd, offset_);
    }
};

template <size_t ProtoChainDepth>
class ICSetProp_NativeAddImpl : public ICSetProp_NativeAdd
{
    friend class ICStubSpace;

    static const size_t NumShapes = ProtoChainDepth + 1;
    mozilla::Array<HeapPtrShape, NumShapes> shapes_;

    ICSetProp_NativeAddImpl(JitCode* stubCode, ObjectGroup* group,
                            Handle<ShapeVector> shapes,
                            Shape* newShape, ObjectGroup* newGroup, uint32_t offset);

  public:
    void traceShapes(JSTracer* trc) {
        for (size_t i = 0; i < NumShapes; i++)
            TraceEdge(trc, &shapes_[i], "baseline-setpropnativeadd-stub-shape");
    }

    static size_t offsetOfShape(size_t idx) {
        return offsetof(ICSetProp_NativeAddImpl, shapes_) + (idx * sizeof(HeapPtrShape));
    }
};

class ICSetPropNativeAddCompiler : public ICStubCompiler
{
    RootedObject obj_;
    RootedShape oldShape_;
    RootedObjectGroup oldGroup_;
    size_t protoChainDepth_;
    bool isFixedSlot_;
    uint32_t offset_;

  protected:
    virtual int32_t getKey() const {
        return static_cast<int32_t>(engine_) |
              (static_cast<int32_t>(kind) << 1) |
              (static_cast<int32_t>(isFixedSlot_) << 17) |
              (static_cast<int32_t>(obj_->is<UnboxedPlainObject>()) << 18) |
              (static_cast<int32_t>(protoChainDepth_) << 19);
    }

    bool generateStubCode(MacroAssembler& masm);

  public:
    ICSetPropNativeAddCompiler(JSContext* cx, HandleObject obj,
                               HandleShape oldShape, HandleObjectGroup oldGroup,
                               size_t protoChainDepth, bool isFixedSlot, uint32_t offset);

    template <size_t ProtoChainDepth>
    ICUpdatedStub* getStubSpecific(ICStubSpace* space, Handle<ShapeVector> shapes)
    {
        RootedObjectGroup newGroup(cx, obj_->getGroup(cx));
        if (!newGroup)
            return nullptr;

        // Only specify newGroup when the object's group changes due to the
        // object becoming fully initialized per the acquired properties
        // analysis.
        if (newGroup == oldGroup_)
            newGroup = nullptr;

        RootedShape newShape(cx);
        if (obj_->isNative())
            newShape = obj_->as<NativeObject>().lastProperty();
        else
            newShape = obj_->as<UnboxedPlainObject>().maybeExpando()->lastProperty();

        return newStub<ICSetProp_NativeAddImpl<ProtoChainDepth>>(
            space, getStubCode(), oldGroup_, shapes, newShape, newGroup, offset_);
    }

    ICUpdatedStub* getStub(ICStubSpace* space);
};

class ICSetProp_Unboxed : public ICUpdatedStub
{
    friend class ICStubSpace;

    HeapPtrObjectGroup group_;
    uint32_t fieldOffset_;

    ICSetProp_Unboxed(JitCode* stubCode, ObjectGroup* group, uint32_t fieldOffset)
      : ICUpdatedStub(ICStub::SetProp_Unboxed, stubCode),
        group_(group),
        fieldOffset_(fieldOffset)
    {
        (void) fieldOffset_; // Silence clang warning
    }

  public:
    HeapPtrObjectGroup& group() {
        return group_;
    }

    static size_t offsetOfGroup() {
        return offsetof(ICSetProp_Unboxed, group_);
    }
    static size_t offsetOfFieldOffset() {
        return offsetof(ICSetProp_Unboxed, fieldOffset_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        RootedObjectGroup group_;
        uint32_t fieldOffset_;
        JSValueType fieldType_;

        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(fieldType_) << 17);
        }

      public:
        Compiler(JSContext* cx, ObjectGroup* group, uint32_t fieldOffset,
                 JSValueType fieldType)
          : ICStubCompiler(cx, ICStub::SetProp_Unboxed, Engine::Baseline),
            group_(cx, group),
            fieldOffset_(fieldOffset),
            fieldType_(fieldType)
        {}

        ICUpdatedStub* getStub(ICStubSpace* space) {
            ICUpdatedStub* stub = newStub<ICSetProp_Unboxed>(space, getStubCode(), group_,
                                                             fieldOffset_);
            if (!stub || !stub->initUpdatingChain(cx, space))
                return nullptr;
            return stub;
        }

        bool needsUpdateStubs() {
            return fieldType_ == JSVAL_TYPE_OBJECT;
        }
    };
};

class ICSetProp_TypedObject : public ICUpdatedStub
{
    friend class ICStubSpace;

    HeapPtrShape shape_;
    HeapPtrObjectGroup group_;
    uint32_t fieldOffset_;
    bool isObjectReference_;

    ICSetProp_TypedObject(JitCode* stubCode, Shape* shape, ObjectGroup* group,
                          uint32_t fieldOffset, bool isObjectReference)
      : ICUpdatedStub(ICStub::SetProp_TypedObject, stubCode),
        shape_(shape),
        group_(group),
        fieldOffset_(fieldOffset),
        isObjectReference_(isObjectReference)
    {
        (void) fieldOffset_; // Silence clang warning
    }

  public:
    HeapPtrShape& shape() {
        return shape_;
    }
    HeapPtrObjectGroup& group() {
        return group_;
    }
    bool isObjectReference() {
        return isObjectReference_;
    }

    static size_t offsetOfShape() {
        return offsetof(ICSetProp_TypedObject, shape_);
    }
    static size_t offsetOfGroup() {
        return offsetof(ICSetProp_TypedObject, group_);
    }
    static size_t offsetOfFieldOffset() {
        return offsetof(ICSetProp_TypedObject, fieldOffset_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        RootedShape shape_;
        RootedObjectGroup group_;
        uint32_t fieldOffset_;
        TypedThingLayout layout_;
        Rooted<SimpleTypeDescr*> fieldDescr_;

        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(SimpleTypeDescrKey(fieldDescr_)) << 17) |
                  (static_cast<int32_t>(layout_) << 25);
        }

      public:
        Compiler(JSContext* cx, Shape* shape, ObjectGroup* group, uint32_t fieldOffset,
                 SimpleTypeDescr* fieldDescr)
          : ICStubCompiler(cx, ICStub::SetProp_TypedObject, Engine::Baseline),
            shape_(cx, shape),
            group_(cx, group),
            fieldOffset_(fieldOffset),
            layout_(GetTypedThingLayout(shape->getObjectClass())),
            fieldDescr_(cx, fieldDescr)
        {}

        ICUpdatedStub* getStub(ICStubSpace* space) {
            bool isObjectReference =
                fieldDescr_->is<ReferenceTypeDescr>() &&
                fieldDescr_->as<ReferenceTypeDescr>().type() == ReferenceTypeDescr::TYPE_OBJECT;
            ICUpdatedStub* stub = newStub<ICSetProp_TypedObject>(space, getStubCode(), shape_,
                                                                 group_, fieldOffset_,
                                                                 isObjectReference);
            if (!stub || !stub->initUpdatingChain(cx, space))
                return nullptr;
            return stub;
        }

        bool needsUpdateStubs() {
            return fieldDescr_->is<ReferenceTypeDescr>() &&
                   fieldDescr_->as<ReferenceTypeDescr>().type() != ReferenceTypeDescr::TYPE_STRING;
        }
    };
};

// Base stub for calling a setters on a native or unboxed object.
class ICSetPropCallSetter : public ICStub
{
    friend class ICStubSpace;

  protected:
    // Shape/group of receiver object. Used for both own and proto setters.
    HeapReceiverGuard receiverGuard_;

    // Holder and holder shape. For own setters, guarding on receiverGuard_ is
    // sufficient, although Ion may use holder_ and holderShape_ even for own
    // setters. In this case holderShape_ == receiverGuard_.shape_ (isOwnSetter
    // below relies on this).
    HeapPtrObject holder_;
    HeapPtrShape holderShape_;

    // Function to call.
    HeapPtrFunction setter_;

    // PC of call, for profiler
    uint32_t pcOffset_;

    ICSetPropCallSetter(Kind kind, JitCode* stubCode, ReceiverGuard receiverGuard,
                        JSObject* holder, Shape* holderShape, JSFunction* setter,
                        uint32_t pcOffset);

  public:
    HeapReceiverGuard& receiverGuard() {
        return receiverGuard_;
    }
    HeapPtrObject& holder() {
        return holder_;
    }
    HeapPtrShape& holderShape() {
        return holderShape_;
    }
    HeapPtrFunction& setter() {
        return setter_;
    }

    bool isOwnSetter() const {
        MOZ_ASSERT(holder_->isNative());
        MOZ_ASSERT(holderShape_);
        return receiverGuard_.shape() == holderShape_;
    }

    static size_t offsetOfReceiverGuard() {
        return offsetof(ICSetPropCallSetter, receiverGuard_);
    }
    static size_t offsetOfHolder() {
        return offsetof(ICSetPropCallSetter, holder_);
    }
    static size_t offsetOfHolderShape() {
        return offsetof(ICSetPropCallSetter, holderShape_);
    }
    static size_t offsetOfSetter() {
        return offsetof(ICSetPropCallSetter, setter_);
    }
    static size_t offsetOfPCOffset() {
        return offsetof(ICSetPropCallSetter, pcOffset_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        RootedObject receiver_;
        RootedObject holder_;
        RootedFunction setter_;
        uint32_t pcOffset_;

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (HeapReceiverGuard::keyBits(receiver_) << 17) |
                  (static_cast<int32_t>(receiver_ != holder_) << 20);
        }

      public:
        Compiler(JSContext* cx, ICStub::Kind kind, HandleObject receiver, HandleObject holder,
                 HandleFunction setter, uint32_t pcOffset)
          : ICStubCompiler(cx, kind, Engine::Baseline),
            receiver_(cx, receiver),
            holder_(cx, holder),
            setter_(cx, setter),
            pcOffset_(pcOffset)
        {
            MOZ_ASSERT(kind == ICStub::SetProp_CallScripted || kind == ICStub::SetProp_CallNative);
        }
    };
};

// Stub for calling a scripted setter on a native object.
class ICSetProp_CallScripted : public ICSetPropCallSetter
{
    friend class ICStubSpace;

  protected:
    ICSetProp_CallScripted(JitCode* stubCode, ReceiverGuard guard, JSObject* holder,
                           Shape* holderShape, JSFunction* setter, uint32_t pcOffset)
      : ICSetPropCallSetter(SetProp_CallScripted, stubCode, guard, holder, holderShape,
                            setter, pcOffset)
    {}

  public:
    static ICSetProp_CallScripted* Clone(JSContext* cx, ICStubSpace* space, ICStub*,
                                         ICSetProp_CallScripted& other);

    class Compiler : public ICSetPropCallSetter::Compiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, HandleObject obj, HandleObject holder, HandleFunction setter,
                 uint32_t pcOffset)
          : ICSetPropCallSetter::Compiler(cx, ICStub::SetProp_CallScripted,
                                          obj, holder, setter, pcOffset)
        {}

        ICStub* getStub(ICStubSpace* space) {
            ReceiverGuard guard(receiver_);
            Shape* holderShape = holder_->as<NativeObject>().lastProperty();
            return newStub<ICSetProp_CallScripted>(space, getStubCode(), guard, holder_,
                                                       holderShape, setter_, pcOffset_);
        }
    };
};

// Stub for calling a native setter on a native object.
class ICSetProp_CallNative : public ICSetPropCallSetter
{
    friend class ICStubSpace;

  protected:
    ICSetProp_CallNative(JitCode* stubCode, ReceiverGuard guard, JSObject* holder,
                         Shape* holderShape, JSFunction* setter, uint32_t pcOffset)
      : ICSetPropCallSetter(SetProp_CallNative, stubCode, guard, holder, holderShape,
                            setter, pcOffset)
    {}

  public:
    static ICSetProp_CallNative* Clone(JSContext* cx,
                                       ICStubSpace* space, ICStub*,
                                       ICSetProp_CallNative& other);

    class Compiler : public ICSetPropCallSetter::Compiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, HandleObject obj, HandleObject holder, HandleFunction setter,
                 uint32_t pcOffset)
          : ICSetPropCallSetter::Compiler(cx, ICStub::SetProp_CallNative,
                                          obj, holder, setter, pcOffset)
        {}

        ICStub* getStub(ICStubSpace* space) {
            ReceiverGuard guard(receiver_);
            Shape* holderShape = holder_->as<NativeObject>().lastProperty();
            return newStub<ICSetProp_CallNative>(space, getStubCode(), guard, holder_, holderShape,
                                                 setter_, pcOffset_);
        }
    };
};

// Call
//      JSOP_CALL
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
                           Register argcReg, bool checkNative, FunApplyThing applyThing,
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
    static const uint32_t MAX_SCRIPTED_STUBS = 7;
    static const uint32_t MAX_NATIVE_STUBS = 7;
  private:

    explicit ICCall_Fallback(JitCode* stubCode)
      : ICMonitoredFallbackStub(ICStub::Call_Fallback, stubCode)
    { }

  public:
    void noteUnoptimizableCall() {
        extra_ |= UNOPTIMIZABLE_CALL_FLAG;
    }
    bool hadUnoptimizableCall() const {
        return extra_ & UNOPTIMIZABLE_CALL_FLAG;
    }

    unsigned scriptedStubCount() const {
        return numStubsWithKind(Call_Scripted);
    }
    bool scriptedStubsAreGeneralized() const {
        return hasStub(Call_AnyScripted);
    }

    unsigned nativeStubCount() const {
        return numStubsWithKind(Call_Native);
    }
    bool nativeStubsAreGeneralized() const {
        // Return hasStub(Call_AnyNative) after Call_AnyNative stub is added.
        return false;
    }

    // Compiler for this stub kind.
    class Compiler : public ICCallStubCompiler {
      public:
        static const int32_t BASELINE_CALL_KEY =
            (static_cast<int32_t>(Engine::Baseline)) |
            (static_cast<int32_t>(ICStub::Call_Fallback) << 1) |
            (0 << 17) | // spread
            (0 << 18);  // constructing
        static const int32_t BASELINE_CONSTRUCT_KEY =
            (static_cast<int32_t>(Engine::Baseline)) |
            (static_cast<int32_t>(ICStub::Call_Fallback) << 1) |
            (0 << 17) | // spread
            (1 << 18);  // constructing

      protected:
        bool isConstructing_;
        bool isSpread_;
        uint32_t returnOffset_;
        bool generateStubCode(MacroAssembler& masm);
        void postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code);

        virtual int32_t getKey() const {
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

        ICStub* getStub(ICStubSpace* space) {
            ICCall_Fallback* stub = newStub<ICCall_Fallback>(space, getStubCode());
            if (!stub || !stub->initMonitoringChain(cx, space, engine_))
                return nullptr;
            return stub;
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
    HeapPtrFunction callee_;
    HeapPtrObject templateObject_;
    uint32_t pcOffset_;

    ICCall_Scripted(JitCode* stubCode, ICStub* firstMonitorStub,
                    JSFunction* callee, JSObject* templateObject,
                    uint32_t pcOffset);

  public:
    static ICCall_Scripted* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                  ICCall_Scripted& other);

    HeapPtrFunction& callee() {
        return callee_;
    }
    HeapPtrObject& templateObject() {
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
    bool generateStubCode(MacroAssembler& masm);

    virtual int32_t getKey() const {
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

    ICStub* getStub(ICStubSpace* space) {
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
    HeapPtrFunction callee_;
    HeapPtrObject templateObject_;
    uint32_t pcOffset_;

#ifdef JS_SIMULATOR
    void *native_;
#endif

    ICCall_Native(JitCode* stubCode, ICStub* firstMonitorStub,
                  JSFunction* callee, JSObject* templateObject,
                  uint32_t pcOffset);

  public:
    static ICCall_Native* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                ICCall_Native& other);

    HeapPtrFunction& callee() {
        return callee_;
    }
    HeapPtrObject& templateObject() {
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
        bool isSpread_;
        RootedFunction callee_;
        RootedObject templateObject_;
        uint32_t pcOffset_;
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(isConstructing_) << 17) |
                  (static_cast<int32_t>(isSpread_) << 18);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub,
                 HandleFunction callee, HandleObject templateObject,
                 bool isConstructing, bool isSpread, uint32_t pcOffset)
          : ICCallStubCompiler(cx, ICStub::Call_Native),
            firstMonitorStub_(firstMonitorStub),
            isConstructing_(isConstructing),
            isSpread_(isSpread),
            callee_(cx, callee),
            templateObject_(cx, templateObject),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) {
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
    HeapPtrObject templateObject_;
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
    HeapPtrObject& templateObject() {
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
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
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

        ICStub* getStub(ICStubSpace* space) {
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
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset)
          : ICCallStubCompiler(cx, ICStub::Call_ScriptedApplyArray),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) {
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
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset)
          : ICCallStubCompiler(cx, ICStub::Call_ScriptedApplyArguments),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) {
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
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset)
          : ICCallStubCompiler(cx, ICStub::Call_ScriptedFunCall),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCall_ScriptedFunCall>(space, getStubCode(), firstMonitorStub_,
                                                   pcOffset_);
        }
    };
};

class ICCall_StringSplit : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    uint32_t pcOffset_;
    HeapPtrString expectedThis_;
    HeapPtrString expectedArg_;
    HeapPtrObject templateObject_;

    ICCall_StringSplit(JitCode* stubCode, ICStub* firstMonitorStub, uint32_t pcOffset, JSString* thisString,
                       JSString* argString, JSObject* templateObject)
      : ICMonitoredStub(ICStub::Call_StringSplit, stubCode, firstMonitorStub),
        pcOffset_(pcOffset), expectedThis_(thisString), expectedArg_(argString),
        templateObject_(templateObject)
    { }

  public:
    static size_t offsetOfExpectedThis() {
        return offsetof(ICCall_StringSplit, expectedThis_);
    }

    static size_t offsetOfExpectedArg() {
        return offsetof(ICCall_StringSplit, expectedArg_);
    }

    static size_t offsetOfTemplateObject() {
        return offsetof(ICCall_StringSplit, templateObject_);
    }

    HeapPtrString& expectedThis() {
        return expectedThis_;
    }

    HeapPtrString& expectedArg() {
        return expectedArg_;
    }

    HeapPtrObject& templateObject() {
        return templateObject_;
    }

    class Compiler : public ICCallStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        uint32_t pcOffset_;
        RootedString expectedThis_;
        RootedString expectedArg_;
        RootedObject templateObject_;

        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset, HandleString thisString,
                 HandleString argString, HandleValue templateObject)
          : ICCallStubCompiler(cx, ICStub::Call_StringSplit),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset),
            expectedThis_(cx, thisString),
            expectedArg_(cx, argString),
            templateObject_(cx, &templateObject.toObject())
        { }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCall_StringSplit>(space, getStubCode(), firstMonitorStub_, pcOffset_,
                                               expectedThis_, expectedArg_, templateObject_);
        }
   };
};

class ICCall_IsSuspendedStarGenerator : public ICStub
{
    friend class ICStubSpace;

  protected:
    explicit ICCall_IsSuspendedStarGenerator(JitCode* stubCode)
      : ICStub(ICStub::Call_IsSuspendedStarGenerator, stubCode)
    {}

  public:
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::Call_IsSuspendedStarGenerator, Engine::Baseline)
        {}
        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCall_IsSuspendedStarGenerator>(space, getStubCode());
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
        bool generateStubCode(MacroAssembler& masm);

        jsbytecode* pc_;

      public:
        Compiler(JSContext* cx, jsbytecode* pc)
          : ICStubCompiler(cx, ICStub::TableSwitch, Engine::Baseline), pc_(pc)
        {}

        ICStub* getStub(ICStubSpace* space);
    };
};

// IC for constructing an iterator from an input value.
class ICIteratorNew_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICIteratorNew_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::IteratorNew_Fallback, stubCode)
    { }

  public:
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::IteratorNew_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICIteratorNew_Fallback>(space, getStubCode());
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::IteratorMore_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::IteratorMore_Native, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::IteratorClose_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
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
    static const uint32_t MAX_OPTIMIZED_STUBS = 4;

    void noteUnoptimizableAccess() {
        extra_ |= UNOPTIMIZABLE_ACCESS_BIT;
    }
    bool hadUnoptimizableAccess() const {
        return extra_ & UNOPTIMIZABLE_ACCESS_BIT;
    }

    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::InstanceOf_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICInstanceOf_Fallback>(space, getStubCode());
        }
    };
};

class ICInstanceOf_Function : public ICStub
{
    friend class ICStubSpace;

    HeapPtrShape shape_;
    HeapPtrObject prototypeObj_;
    uint32_t slot_;

    ICInstanceOf_Function(JitCode* stubCode, Shape* shape, JSObject* prototypeObj, uint32_t slot);

  public:
    HeapPtrShape& shape() {
        return shape_;
    }
    HeapPtrObject& prototypeObject() {
        return prototypeObj_;
    }
    uint32_t slot() const {
        return slot_;
    }
    static size_t offsetOfShape() {
        return offsetof(ICInstanceOf_Function, shape_);
    }
    static size_t offsetOfPrototypeObject() {
        return offsetof(ICInstanceOf_Function, prototypeObj_);
    }
    static size_t offsetOfSlot() {
        return offsetof(ICInstanceOf_Function, slot_);
    }

    class Compiler : public ICStubCompiler {
        RootedShape shape_;
        RootedObject prototypeObj_;
        uint32_t slot_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, Shape* shape, JSObject* prototypeObj, uint32_t slot)
          : ICStubCompiler(cx, ICStub::InstanceOf_Function, Engine::Baseline),
            shape_(cx, shape),
            prototypeObj_(cx, prototypeObj),
            slot_(slot)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICInstanceOf_Function>(space, getStubCode(), shape_, prototypeObj_,
                                                  slot_);
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
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::TypeOf_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICTypeOf_Fallback>(space, getStubCode());
        }
    };
};

class ICTypeOf_Typed : public ICFallbackStub
{
    friend class ICStubSpace;

    ICTypeOf_Typed(JitCode* stubCode, JSType type)
      : ICFallbackStub(ICStub::TypeOf_Typed, stubCode)
    {
        extra_ = uint16_t(type);
        MOZ_ASSERT(JSType(extra_) == type);
    }

  public:
    JSType type() const {
        return JSType(extra_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        JSType type_;
        RootedString typeString_;
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(type_) << 17);
        }

      public:
        Compiler(JSContext* cx, JSType type, HandleString string)
          : ICStubCompiler(cx, ICStub::TypeOf_Typed, Engine::Baseline),
            type_(type),
            typeString_(cx, string)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICTypeOf_Typed>(space, getStubCode(), type_);
        }
    };
};

class ICRest_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    HeapPtrArrayObject templateObject_;

    ICRest_Fallback(JitCode* stubCode, ArrayObject* templateObject)
      : ICFallbackStub(ICStub::Rest_Fallback, stubCode), templateObject_(templateObject)
    { }

  public:
    static const uint32_t MAX_OPTIMIZED_STUBS = 8;

    HeapPtrArrayObject& templateObject() {
        return templateObject_;
    }

    class Compiler : public ICStubCompiler {
      protected:
        RootedArrayObject templateObject;
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ArrayObject* templateObject)
          : ICStubCompiler(cx, ICStub::Rest_Fallback, Engine::Baseline),
            templateObject(cx, templateObject)
        { }

        ICStub* getStub(ICStubSpace* space) {
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::RetSub_Fallback, Engine::Baseline)
        { }

        ICStub* getStub(ICStubSpace* space) {
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

        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, uint32_t pcOffset, uint8_t* addr)
          : ICStubCompiler(cx, ICStub::RetSub_Resume, Engine::Baseline),
            pcOffset_(pcOffset),
            addr_(addr)
        { }

        ICStub* getStub(ICStubSpace* space) {
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

} // namespace jit
} // namespace js

#endif /* jit_BaselineIC_h */
