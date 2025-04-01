/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonIC_h
#define jit_IonIC_h

#include "jit/CacheIR.h"
#include "jit/ICState.h"
#include "jit/shared/Assembler-shared.h"

namespace js {
namespace jit {

class CacheIRStubInfo;
class CacheIRWriter;
class IonScript;

// An optimized stub attached to an IonIC.
class IonICStub {
  // Code to jump to when this stub fails. This is either the next optimized
  // stub or the OOL fallback path.
  uint8_t* nextCodeRaw_;

  // The next optimized stub in this chain, or nullptr if this is the last
  // one.
  IonICStub* next_;

  // Info about this stub.
  CacheIRStubInfo* stubInfo_;

#ifndef JS_64BIT
 protected:  // Silence Clang warning about unused private fields.
  // Ensure stub data is 8-byte aligned on 32-bit.
  uintptr_t padding_ = 0;
#endif

 public:
  IonICStub(uint8_t* fallbackCode, CacheIRStubInfo* stubInfo)
      : nextCodeRaw_(fallbackCode), next_(nullptr), stubInfo_(stubInfo) {}

  uint8_t* nextCodeRaw() const { return nextCodeRaw_; }
  uint8_t** nextCodeRawPtr() { return &nextCodeRaw_; }
  CacheIRStubInfo* stubInfo() const { return stubInfo_; }
  IonICStub* next() const { return next_; }

  uint8_t* stubDataStart();

  void setNext(IonICStub* next, uint8_t* nextCodeRaw) {
    MOZ_ASSERT(!next_);
    MOZ_ASSERT(next && nextCodeRaw);
    next_ = next;
    nextCodeRaw_ = nextCodeRaw;
  }

  // Null out pointers when we unlink stubs, to ensure we never use
  // discarded stubs.
  void poison() {
    nextCodeRaw_ = nullptr;
    next_ = nullptr;
    stubInfo_ = nullptr;
  }
};

class IonGetPropertyIC;
class IonSetPropertyIC;
class IonGetPropSuperIC;
class IonGetNameIC;
class IonBindNameIC;
class IonGetIteratorIC;
class IonHasOwnIC;
class IonCheckPrivateFieldIC;
class IonInIC;
class IonInstanceOfIC;
class IonCompareIC;
class IonUnaryArithIC;
class IonBinaryArithIC;
class IonToPropertyKeyIC;
class IonOptimizeSpreadCallIC;
class IonCloseIterIC;
class IonOptimizeGetIteratorIC;

class IonIC {
  // This either points at the OOL path for the fallback path, or the code for
  // the first stub.
  uint8_t* codeRaw_;

  // The first optimized stub, or nullptr.
  IonICStub* firstStub_;

  // Location of this IC.
  JSScript* script_;
  jsbytecode* pc_;

  // The offset of the rejoin location in the IonScript's code (stubs jump to
  // this location).
  uint32_t rejoinOffset_;

  // The offset of the OOL path in the IonScript's code that calls the IC's
  // update function.
  uint32_t fallbackOffset_;

  CacheKind kind_;
  ICState state_;

 protected:
  explicit IonIC(CacheKind kind)
      : codeRaw_(nullptr),
        firstStub_(nullptr),
        script_(nullptr),
        pc_(nullptr),
        rejoinOffset_(0),
        fallbackOffset_(0),
        kind_(kind) {}

  void attachStub(IonICStub* newStub, JitCode* code);

 public:
  void setScriptedLocation(JSScript* script, jsbytecode* pc) {
    MOZ_ASSERT(!script_ && !pc_);
    MOZ_ASSERT(script && pc);
    script_ = script;
    pc_ = pc;
  }

  JSScript* script() const {
    MOZ_ASSERT(script_);
    return script_;
  }
  jsbytecode* pc() const {
    MOZ_ASSERT(pc_);
    return pc_;
  }

  // Discard all stubs.
  void discardStubs(Zone* zone, IonScript* ionScript);

  // Discard all stubs and reset the ICState.
  void reset(Zone* zone, IonScript* ionScript);

  ICState& state() { return state_; }

  CacheKind kind() const { return kind_; }
  uint8_t** codeRawPtr() { return &codeRaw_; }

  void setFallbackOffset(CodeOffset offset) {
    fallbackOffset_ = offset.offset();
  }
  void setRejoinOffset(CodeOffset offset) { rejoinOffset_ = offset.offset(); }

  void resetCodeRaw(IonScript* ionScript);

  uint8_t* fallbackAddr(IonScript* ionScript) const;
  uint8_t* rejoinAddr(IonScript* ionScript) const;

  IonGetPropertyIC* asGetPropertyIC() {
    MOZ_ASSERT(kind_ == CacheKind::GetProp || kind_ == CacheKind::GetElem);
    return (IonGetPropertyIC*)this;
  }
  IonSetPropertyIC* asSetPropertyIC() {
    MOZ_ASSERT(kind_ == CacheKind::SetProp || kind_ == CacheKind::SetElem);
    return (IonSetPropertyIC*)this;
  }
  IonGetPropSuperIC* asGetPropSuperIC() {
    MOZ_ASSERT(kind_ == CacheKind::GetPropSuper ||
               kind_ == CacheKind::GetElemSuper);
    return (IonGetPropSuperIC*)this;
  }
  IonGetNameIC* asGetNameIC() {
    MOZ_ASSERT(kind_ == CacheKind::GetName);
    return (IonGetNameIC*)this;
  }
  IonBindNameIC* asBindNameIC() {
    MOZ_ASSERT(kind_ == CacheKind::BindName);
    return (IonBindNameIC*)this;
  }
  IonGetIteratorIC* asGetIteratorIC() {
    MOZ_ASSERT(kind_ == CacheKind::GetIterator);
    return (IonGetIteratorIC*)this;
  }
  IonOptimizeSpreadCallIC* asOptimizeSpreadCallIC() {
    MOZ_ASSERT(kind_ == CacheKind::OptimizeSpreadCall);
    return (IonOptimizeSpreadCallIC*)this;
  }
  IonHasOwnIC* asHasOwnIC() {
    MOZ_ASSERT(kind_ == CacheKind::HasOwn);
    return (IonHasOwnIC*)this;
  }
  IonCheckPrivateFieldIC* asCheckPrivateFieldIC() {
    MOZ_ASSERT(kind_ == CacheKind::CheckPrivateField);
    return (IonCheckPrivateFieldIC*)this;
  }
  IonInIC* asInIC() {
    MOZ_ASSERT(kind_ == CacheKind::In);
    return (IonInIC*)this;
  }
  IonInstanceOfIC* asInstanceOfIC() {
    MOZ_ASSERT(kind_ == CacheKind::InstanceOf);
    return (IonInstanceOfIC*)this;
  }
  IonCompareIC* asCompareIC() {
    MOZ_ASSERT(kind_ == CacheKind::Compare);
    return (IonCompareIC*)this;
  }
  IonUnaryArithIC* asUnaryArithIC() {
    MOZ_ASSERT(kind_ == CacheKind::UnaryArith);
    return (IonUnaryArithIC*)this;
  }
  IonBinaryArithIC* asBinaryArithIC() {
    MOZ_ASSERT(kind_ == CacheKind::BinaryArith);
    return (IonBinaryArithIC*)this;
  }
  IonToPropertyKeyIC* asToPropertyKeyIC() {
    MOZ_ASSERT(kind_ == CacheKind::ToPropertyKey);
    return (IonToPropertyKeyIC*)this;
  }
  IonCloseIterIC* asCloseIterIC() {
    MOZ_ASSERT(kind_ == CacheKind::CloseIter);
    return (IonCloseIterIC*)this;
  }
  IonOptimizeGetIteratorIC* asOptimizeGetIteratorIC() {
    MOZ_ASSERT(kind_ == CacheKind::OptimizeGetIterator);
    return (IonOptimizeGetIteratorIC*)this;
  }

  // Returns the Register to use as scratch when entering IC stubs. This
  // should either be an output register or a temp.
  Register scratchRegisterForEntryJump();

  void trace(JSTracer* trc, IonScript* ionScript);

  void attachCacheIRStub(JSContext* cx, const CacheIRWriter& writer,
                         CacheKind kind, IonScript* ionScript, bool* attached);
};

class IonGetPropertyIC : public IonIC {
 private:
  LiveRegisterSet liveRegs_;

  TypedOrValueRegister value_;
  ConstantOrRegister id_;
  ValueOperand output_;

 public:
  IonGetPropertyIC(CacheKind kind, LiveRegisterSet liveRegs,
                   TypedOrValueRegister value, const ConstantOrRegister& id,
                   ValueOperand output)
      : IonIC(kind),
        liveRegs_(liveRegs),
        value_(value),
        id_(id),
        output_(output) {}

  TypedOrValueRegister value() const { return value_; }
  ConstantOrRegister id() const { return id_; }
  ValueOperand output() const { return output_; }
  LiveRegisterSet liveRegs() const { return liveRegs_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonGetPropertyIC* ic, HandleValue val,
                                   HandleValue idVal, MutableHandleValue res);
};

class IonGetPropSuperIC : public IonIC {
  LiveRegisterSet liveRegs_;

  Register object_;
  TypedOrValueRegister receiver_;
  ConstantOrRegister id_;
  ValueOperand output_;

 public:
  IonGetPropSuperIC(CacheKind kind, LiveRegisterSet liveRegs, Register object,
                    TypedOrValueRegister receiver, const ConstantOrRegister& id,
                    ValueOperand output)
      : IonIC(kind),
        liveRegs_(liveRegs),
        object_(object),
        receiver_(receiver),
        id_(id),
        output_(output) {}

  Register object() const { return object_; }
  TypedOrValueRegister receiver() const { return receiver_; }
  ConstantOrRegister id() const { return id_; }
  ValueOperand output() const { return output_; }
  LiveRegisterSet liveRegs() const { return liveRegs_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonGetPropSuperIC* ic, HandleObject obj,
                                   HandleValue receiver, HandleValue idVal,
                                   MutableHandleValue res);
};

class IonSetPropertyIC : public IonIC {
  LiveRegisterSet liveRegs_;

  Register object_;
  Register temp_;
  ConstantOrRegister id_;
  ConstantOrRegister rhs_;
  bool strict_ : 1;

 public:
  IonSetPropertyIC(CacheKind kind, LiveRegisterSet liveRegs, Register object,
                   Register temp, const ConstantOrRegister& id,
                   const ConstantOrRegister& rhs, bool strict)
      : IonIC(kind),
        liveRegs_(liveRegs),
        object_(object),
        temp_(temp),
        id_(id),
        rhs_(rhs),
        strict_(strict) {}

  LiveRegisterSet liveRegs() const { return liveRegs_; }
  Register object() const { return object_; }
  ConstantOrRegister id() const { return id_; }
  ConstantOrRegister rhs() const { return rhs_; }

  Register temp() const { return temp_; }

  bool strict() const { return strict_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonSetPropertyIC* ic, HandleObject obj,
                                   HandleValue idVal, HandleValue rhs);
};

class IonGetNameIC : public IonIC {
  LiveRegisterSet liveRegs_;

  Register environment_;
  ValueOperand output_;
  Register temp_;

 public:
  IonGetNameIC(LiveRegisterSet liveRegs, Register environment,
               ValueOperand output, Register temp)
      : IonIC(CacheKind::GetName),
        liveRegs_(liveRegs),
        environment_(environment),
        output_(output),
        temp_(temp) {}

  Register environment() const { return environment_; }
  ValueOperand output() const { return output_; }
  Register temp() const { return temp_; }
  LiveRegisterSet liveRegs() const { return liveRegs_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonGetNameIC* ic, HandleObject envChain,
                                   MutableHandleValue res);
};

class IonBindNameIC : public IonIC {
  LiveRegisterSet liveRegs_;

  Register environment_;
  Register output_;
  Register temp_;

 public:
  IonBindNameIC(LiveRegisterSet liveRegs, Register environment, Register output,
                Register temp)
      : IonIC(CacheKind::BindName),
        liveRegs_(liveRegs),
        environment_(environment),
        output_(output),
        temp_(temp) {}

  Register environment() const { return environment_; }
  Register output() const { return output_; }
  Register temp() const { return temp_; }
  LiveRegisterSet liveRegs() const { return liveRegs_; }

  static JSObject* update(JSContext* cx, HandleScript outerScript,
                          IonBindNameIC* ic, HandleObject envChain);
};

class IonGetIteratorIC : public IonIC {
  LiveRegisterSet liveRegs_;
  TypedOrValueRegister value_;
  Register output_;
  Register temp1_;
  Register temp2_;

 public:
  IonGetIteratorIC(LiveRegisterSet liveRegs, TypedOrValueRegister value,
                   Register output, Register temp1, Register temp2)
      : IonIC(CacheKind::GetIterator),
        liveRegs_(liveRegs),
        value_(value),
        output_(output),
        temp1_(temp1),
        temp2_(temp2) {}

  TypedOrValueRegister value() const { return value_; }
  Register output() const { return output_; }
  Register temp1() const { return temp1_; }
  Register temp2() const { return temp2_; }
  LiveRegisterSet liveRegs() const { return liveRegs_; }

  static JSObject* update(JSContext* cx, HandleScript outerScript,
                          IonGetIteratorIC* ic, HandleValue value);
};

class IonOptimizeSpreadCallIC : public IonIC {
  LiveRegisterSet liveRegs_;
  ValueOperand value_;
  ValueOperand output_;
  Register temp_;

 public:
  IonOptimizeSpreadCallIC(LiveRegisterSet liveRegs, ValueOperand value,
                          ValueOperand output, Register temp)
      : IonIC(CacheKind::OptimizeSpreadCall),
        liveRegs_(liveRegs),
        value_(value),
        output_(output),
        temp_(temp) {}

  ValueOperand value() const { return value_; }
  ValueOperand output() const { return output_; }
  Register temp() const { return temp_; }
  LiveRegisterSet liveRegs() const { return liveRegs_; }

  static bool update(JSContext* cx, HandleScript outerScript,
                     IonOptimizeSpreadCallIC* ic, HandleValue value,
                     MutableHandleValue result);
};

class IonHasOwnIC : public IonIC {
  LiveRegisterSet liveRegs_;

  TypedOrValueRegister value_;
  TypedOrValueRegister id_;
  Register output_;

 public:
  IonHasOwnIC(LiveRegisterSet liveRegs, TypedOrValueRegister value,
              TypedOrValueRegister id, Register output)
      : IonIC(CacheKind::HasOwn),
        liveRegs_(liveRegs),
        value_(value),
        id_(id),
        output_(output) {}

  TypedOrValueRegister value() const { return value_; }
  TypedOrValueRegister id() const { return id_; }
  Register output() const { return output_; }
  LiveRegisterSet liveRegs() const { return liveRegs_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonHasOwnIC* ic, HandleValue val,
                                   HandleValue idVal, int32_t* res);
};

class IonCheckPrivateFieldIC : public IonIC {
  LiveRegisterSet liveRegs_;

  TypedOrValueRegister value_;
  TypedOrValueRegister id_;
  Register output_;

 public:
  IonCheckPrivateFieldIC(LiveRegisterSet liveRegs, TypedOrValueRegister value,
                         TypedOrValueRegister id, Register output)
      : IonIC(CacheKind::CheckPrivateField),
        liveRegs_(liveRegs),
        value_(value),
        id_(id),
        output_(output) {}

  TypedOrValueRegister value() const { return value_; }
  TypedOrValueRegister id() const { return id_; }
  Register output() const { return output_; }
  LiveRegisterSet liveRegs() const { return liveRegs_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonCheckPrivateFieldIC* ic, HandleValue val,
                                   HandleValue idVal, bool* res);
};

class IonInIC : public IonIC {
  LiveRegisterSet liveRegs_;

  ConstantOrRegister key_;
  Register object_;
  Register output_;
  Register temp_;

 public:
  IonInIC(LiveRegisterSet liveRegs, const ConstantOrRegister& key,
          Register object, Register output, Register temp)
      : IonIC(CacheKind::In),
        liveRegs_(liveRegs),
        key_(key),
        object_(object),
        output_(output),
        temp_(temp) {}

  ConstantOrRegister key() const { return key_; }
  Register object() const { return object_; }
  Register output() const { return output_; }
  Register temp() const { return temp_; }
  LiveRegisterSet liveRegs() const { return liveRegs_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonInIC* ic, HandleValue key,
                                   HandleObject obj, bool* res);
};

class IonInstanceOfIC : public IonIC {
  LiveRegisterSet liveRegs_;

  TypedOrValueRegister lhs_;
  Register rhs_;
  Register output_;

 public:
  IonInstanceOfIC(LiveRegisterSet liveRegs, TypedOrValueRegister lhs,
                  Register rhs, Register output)
      : IonIC(CacheKind::InstanceOf),
        liveRegs_(liveRegs),
        lhs_(lhs),
        rhs_(rhs),
        output_(output) {}

  LiveRegisterSet liveRegs() const { return liveRegs_; }
  TypedOrValueRegister lhs() const { return lhs_; }
  Register rhs() const { return rhs_; }
  Register output() const { return output_; }

  // This signature mimics that of TryAttachInstanceOfStub in baseline
  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonInstanceOfIC* ic, HandleValue lhs,
                                   HandleObject rhs, bool* attached);
};

class IonCompareIC : public IonIC {
  LiveRegisterSet liveRegs_;

  TypedOrValueRegister lhs_;
  TypedOrValueRegister rhs_;
  Register output_;

 public:
  IonCompareIC(LiveRegisterSet liveRegs, TypedOrValueRegister lhs,
               TypedOrValueRegister rhs, Register output)
      : IonIC(CacheKind::Compare),
        liveRegs_(liveRegs),
        lhs_(lhs),
        rhs_(rhs),
        output_(output) {}

  LiveRegisterSet liveRegs() const { return liveRegs_; }
  TypedOrValueRegister lhs() const { return lhs_; }
  TypedOrValueRegister rhs() const { return rhs_; }
  Register output() const { return output_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonCompareIC* stub, HandleValue lhs,
                                   HandleValue rhs, bool* res);
};

class IonUnaryArithIC : public IonIC {
  LiveRegisterSet liveRegs_;

  TypedOrValueRegister input_;
  ValueOperand output_;

 public:
  IonUnaryArithIC(LiveRegisterSet liveRegs, TypedOrValueRegister input,
                  ValueOperand output)
      : IonIC(CacheKind::UnaryArith),
        liveRegs_(liveRegs),
        input_(input),
        output_(output) {}

  LiveRegisterSet liveRegs() const { return liveRegs_; }
  TypedOrValueRegister input() const { return input_; }
  ValueOperand output() const { return output_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonUnaryArithIC* stub, HandleValue val,
                                   MutableHandleValue res);
};

class IonToPropertyKeyIC : public IonIC {
  LiveRegisterSet liveRegs_;
  ValueOperand input_;
  ValueOperand output_;

 public:
  IonToPropertyKeyIC(LiveRegisterSet liveRegs, ValueOperand input,
                     ValueOperand output)
      : IonIC(CacheKind::ToPropertyKey),
        liveRegs_(liveRegs),
        input_(input),
        output_(output) {}

  LiveRegisterSet liveRegs() const { return liveRegs_; }
  ValueOperand input() const { return input_; }
  ValueOperand output() const { return output_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonToPropertyKeyIC* ic, HandleValue val,
                                   MutableHandleValue res);
};

class IonBinaryArithIC : public IonIC {
  LiveRegisterSet liveRegs_;

  TypedOrValueRegister lhs_;
  TypedOrValueRegister rhs_;
  ValueOperand output_;

 public:
  IonBinaryArithIC(LiveRegisterSet liveRegs, TypedOrValueRegister lhs,
                   TypedOrValueRegister rhs, ValueOperand output)
      : IonIC(CacheKind::BinaryArith),
        liveRegs_(liveRegs),
        lhs_(lhs),
        rhs_(rhs),
        output_(output) {}

  LiveRegisterSet liveRegs() const { return liveRegs_; }
  TypedOrValueRegister lhs() const { return lhs_; }
  TypedOrValueRegister rhs() const { return rhs_; }
  ValueOperand output() const { return output_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonBinaryArithIC* stub, HandleValue lhs,
                                   HandleValue rhs, MutableHandleValue res);
};

class IonCloseIterIC : public IonIC {
  LiveRegisterSet liveRegs_;

  Register iter_;
  Register temp_;
  CompletionKind completionKind_;

 public:
  IonCloseIterIC(LiveRegisterSet liveRegs, Register iter, Register temp,
                 CompletionKind completionKind)
      : IonIC(CacheKind::CloseIter),
        liveRegs_(liveRegs),
        iter_(iter),
        temp_(temp),
        completionKind_(completionKind) {}

  LiveRegisterSet liveRegs() const { return liveRegs_; }
  Register temp() const { return temp_; }
  Register iter() const { return iter_; }
  CompletionKind completionKind() const { return completionKind_; }

  [[nodiscard]] static bool update(JSContext* cx, HandleScript outerScript,
                                   IonCloseIterIC* ic, HandleObject iter);
};

class IonOptimizeGetIteratorIC : public IonIC {
  LiveRegisterSet liveRegs_;
  ValueOperand value_;
  Register output_;
  Register temp_;

 public:
  IonOptimizeGetIteratorIC(LiveRegisterSet liveRegs, ValueOperand value,
                           Register output, Register temp)
      : IonIC(CacheKind::OptimizeGetIterator),
        liveRegs_(liveRegs),
        value_(value),
        output_(output),
        temp_(temp) {}

  ValueOperand value() const { return value_; }
  Register output() const { return output_; }
  Register temp() const { return temp_; }
  LiveRegisterSet liveRegs() const { return liveRegs_; }

  static bool update(JSContext* cx, HandleScript outerScript,
                     IonOptimizeGetIteratorIC* ic, HandleValue value,
                     bool* result);
};

}  // namespace jit
}  // namespace js

#endif /* jit_IonIC_h */
