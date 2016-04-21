/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonCaches_h
#define jit_IonCaches_h

#if defined(JS_CODEGEN_ARM)
# include "jit/arm/Assembler-arm.h"
#elif defined(JS_CODEGEN_ARM64)
# include "jit/arm64/Assembler-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
# include "jit/mips32/Assembler-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
# include "jit/mips64/Assembler-mips64.h"
#endif
#include "jit/JitCompartment.h"
#include "jit/Registers.h"
#include "jit/shared/Assembler-shared.h"
#include "js/TrackedOptimizationInfo.h"

#include "vm/TypedArrayCommon.h"

namespace js {
namespace jit {

class LInstruction;

#define IONCACHE_KIND_LIST(_)                                   \
    _(GetProperty)                                              \
    _(SetProperty)                                              \
    _(BindName)                                                 \
    _(Name)

// Forward declarations of Cache kinds.
#define FORWARD_DECLARE(kind) class kind##IC;
IONCACHE_KIND_LIST(FORWARD_DECLARE)
#undef FORWARD_DECLARE

class IonCacheVisitor
{
  public:
#define VISIT_INS(op)                                               \
    virtual void visit##op##IC(CodeGenerator* codegen) {            \
        MOZ_CRASH("NYI: " #op "IC");                                \
    }

    IONCACHE_KIND_LIST(VISIT_INS)
#undef VISIT_INS
};

// Common structure encoding the state of a polymorphic inline cache contained
// in the code for an IonScript. IonCaches are used for polymorphic operations
// where multiple implementations may be required.
//
// Roughly speaking, the cache initially jumps to an out of line fragment
// which invokes a cache function to perform the operation. The cache function
// may generate a stub to perform the operation in certain cases (e.g. a
// particular shape for an input object) and attach the stub to existing
// stubs, forming a daisy chain of tests for how to perform the operation in
// different circumstances.
//
// Eventually, if too many stubs are generated the cache function may disable
// the cache, by generating a stub to make a call and perform the operation
// within the VM.
//
// The caches initially generate a patchable jump to an out of line call
// to the cache function. Stubs are attached by appending: when attaching a
// new stub, we patch the any failure conditions in last generated stub to
// jump to the new stub. Failure conditions in the new stub jump to the cache
// function which may generate new stubs.
//
//        Control flow               Pointers
//      =======#                 ----.     .---->
//             #                     |     |
//             #======>              \-----/
//
// Initial state:
//
//  JIT Code
// +--------+   .---------------.
// |        |   |               |
// |========|   v +----------+  |
// |== IC ==|====>| Cache Fn |  |
// |========|     +----------+  |
// |        |<=#       #        |
// |        |  #=======#        |
// +--------+  Rejoin path      |
//     |________                |
//             |                |
//     IC      |                |
//   Entry     |                |
// +------------+               |
// | lastJump_  |---------------/
// +------------+
// |    ...     |
// +------------+
//
// Attaching stubs:
//
//   Patch the jump pointed to by lastJump_ to jump to the new stub. Update
//   lastJump_ to be the new stub's failure jump. The failure jump of the new
//   stub goes to the fallback label, which is the cache function. In this
//   fashion, new stubs are _appended_ to the chain of stubs, as lastJump_
//   points to the _tail_ of the stub chain.
//
//  JIT Code
// +--------+ #=======================#
// |        | #                       v
// |========| #   +----------+     +------+
// |== IC ==|=#   | Cache Fn |<====| Stub |
// |========|     +----------+  ^  +------+
// |        |<=#      #         |     #
// |        |  #======#=========|=====#
// +--------+      Rejoin path  |
//     |________                |
//             |                |
//     IC      |                |
//   Entry     |                |
// +------------+               |
// | lastJump_  |---------------/
// +------------+
// |    ...     |
// +------------+
//
// While calls may be made to the cache function and other VM functions, the
// cache may still be treated as pure during optimization passes, such that
// LICM and GVN may be performed on operations around the cache as if the
// operation cannot reenter scripted code through an Invoke() or otherwise have
// unexpected behavior. This restricts the sorts of stubs which the cache can
// generate or the behaviors which called functions can have, and if a called
// function performs a possibly impure operation then the operation will be
// marked as such and the calling script will be recompiled.
//
// Similarly, despite the presence of functions and multiple stubs generated
// for a cache, the cache itself may be marked as idempotent and become hoisted
// or coalesced by LICM or GVN. This also constrains the stubs which can be
// generated for the cache.
//
// * IonCache usage
//
// IonCache is the base structure of an inline cache, which generates code stubs
// dynamically and attaches them to an IonScript.
//
// A cache must at least provide a static update function which will usualy have
// a JSContext*, followed by the cache index. The rest of the arguments of the
// update function are usualy corresponding to the register inputs of the cache,
// as it must perform the same operation as any of the stubs that it might
// produce. The update function call is handled by the visit function of
// CodeGenerator corresponding to this IC.
//
// The CodeGenerator visit function, as opposed to other visit functions, has
// two arguments. The first one is the OutOfLineUpdateCache which stores the LIR
// instruction. The second one is the IC object.  This function would be called
// once the IC is registered with the addCache function of CodeGeneratorShared.
//
// To register a cache, you must call the addCache function as follow:
//
//     MyCodeIC cache(inputReg1, inputValueReg2, outputReg);
//     if (!addCache(lir, allocateCache(cache)))
//         return false;
//
// Once the cache is allocated with the allocateCache function, any modification
// made to the cache would be ignored.
//
// The addCache function will produce a patchable jump at the location where
// it is called. This jump will execute generated stubs and fallback on the code
// of the visitMyCodeIC function if no stub match.
//
//   Warning: As the addCache function fallback on a VMCall, calls to
// addCache should not be in the same path as another VMCall or in the same
// path of another addCache as this is not supported by the invalidation
// procedure.
class IonCache
{
  public:
    class StubAttacher;

    enum Kind {
#   define DEFINE_CACHEKINDS(ickind) Cache_##ickind,
        IONCACHE_KIND_LIST(DEFINE_CACHEKINDS)
#   undef DEFINE_CACHEKINDS
        Cache_Invalid
    };

    // Cache testing and cast.
#   define CACHEKIND_CASTS(ickind)                                      \
    bool is##ickind() const {                                           \
        return kind() == Cache_##ickind;                                \
    }                                                                   \
    inline ickind##IC& to##ickind();                                    \
    inline const ickind##IC& to##ickind() const;
    IONCACHE_KIND_LIST(CACHEKIND_CASTS)
#   undef CACHEKIND_CASTS

    virtual Kind kind() const = 0;

    virtual void accept(CodeGenerator* codegen, IonCacheVisitor* visitor) = 0;

  public:

    static const char* CacheName(Kind kind);

  protected:
    bool pure_ : 1;
    bool idempotent_ : 1;
    bool disabled_ : 1;
    size_t stubCount_ : 5;

    CodeLocationLabel fallbackLabel_;

    // Location of this operation, nullptr for idempotent caches.
    JSScript* script_;
    jsbytecode* pc_;

    // Location to use when updating profiler pseudostack when leaving this
    // IC code to enter a callee.
    jsbytecode* profilerLeavePc_;

    CodeLocationJump initialJump_;
    CodeLocationJump lastJump_;
    CodeLocationLabel rejoinLabel_;

  private:
    static const size_t MAX_STUBS;
    void incrementStubCount() {
        // The IC should stop generating stubs before wrapping stubCount.
        stubCount_++;
        MOZ_ASSERT(stubCount_);
    }

  public:

    IonCache()
      : pure_(false),
        idempotent_(false),
        disabled_(false),
        stubCount_(0),
        fallbackLabel_(),
        script_(nullptr),
        pc_(nullptr),
        profilerLeavePc_(nullptr),
        initialJump_(),
        lastJump_(),
        rejoinLabel_()
    {
    }

    void disable();
    inline bool isDisabled() const {
        return disabled_;
    }

    // Set the initial 'out-of-line' jump state of the cache. The fallbackLabel is
    // the location of the out-of-line update (slow) path.  This location will
    // be set to the exitJump of the last generated stub.
    void setFallbackLabel(CodeOffset fallbackLabel) {
        fallbackLabel_ = fallbackLabel;
    }

    void setProfilerLeavePC(jsbytecode* pc) {
        MOZ_ASSERT(pc != nullptr);
        profilerLeavePc_ = pc;
    }

    // Get the address at which IC rejoins the mainline jitcode.
    void* rejoinAddress() const {
        return rejoinLabel_.raw();
    }

    void emitInitialJump(MacroAssembler& masm, RepatchLabel& entry);
    void updateBaseAddress(JitCode* code, MacroAssembler& masm);

    // Reset the cache around garbage collection.
    virtual void reset(ReprotectCode reprotect);

    bool canAttachStub() const {
        return stubCount_ < MAX_STUBS;
    }
    bool empty() const {
        return stubCount_ == 0;
    }

    enum LinkStatus {
        LINK_ERROR,
        CACHE_FLUSHED,
        LINK_GOOD
    };

    // Use the Linker to link the generated code and check if any
    // monitoring/allocation caused an invalidation of the running ion script,
    // this function returns CACHE_FLUSHED. In case of allocation issue this
    // function returns LINK_ERROR.
    LinkStatus linkCode(JSContext* cx, MacroAssembler& masm, IonScript* ion, JitCode** code);
    // Fixup variables and update jumps in the list of stubs.  Increment the
    // number of attached stubs accordingly.
    void attachStub(MacroAssembler& masm, StubAttacher& attacher, Handle<JitCode*> code);

    // Combine both linkStub and attachStub into one function. In addition, it
    // produces a spew augmented with the attachKind string.
    bool linkAndAttachStub(JSContext* cx, MacroAssembler& masm, StubAttacher& attacher,
                           IonScript* ion, const char* attachKind,
                           JS::TrackedOutcome = JS::TrackedOutcome::ICOptStub_GenericSuccess);

#ifdef DEBUG
    bool isAllocated() {
        return fallbackLabel_.isSet();
    }
#endif

    bool pure() const {
        return pure_;
    }
    bool idempotent() const {
        return idempotent_;
    }
    void setIdempotent() {
        MOZ_ASSERT(!idempotent_);
        MOZ_ASSERT(!script_);
        MOZ_ASSERT(!pc_);
        idempotent_ = true;
    }

    void setScriptedLocation(JSScript* script, jsbytecode* pc) {
        MOZ_ASSERT(!idempotent_);
        script_ = script;
        pc_ = pc;
    }

    void getScriptedLocation(MutableHandleScript pscript, jsbytecode** ppc) const {
        pscript.set(script_);
        *ppc = pc_;
    }

    jsbytecode* pc() const {
        MOZ_ASSERT(pc_);
        return pc_;
    }
};

// Define the cache kind and pre-declare data structures used for calling inline
// caches.
#define CACHE_HEADER(ickind)                                        \
    Kind kind() const {                                             \
        return IonCache::Cache_##ickind;                            \
    }                                                               \
                                                                    \
    void accept(CodeGenerator* codegen, IonCacheVisitor* visitor) { \
        visitor->visit##ickind##IC(codegen);                        \
    }                                                               \
                                                                    \
    static const VMFunction UpdateInfo;

// Subclasses of IonCache for the various kinds of caches. These do not define
// new data members; all caches must be of the same size.

// Helper for idempotent GetPropertyIC location tracking. Declared externally
// to be forward declarable.
//
// Since all the scripts stored in CacheLocations are guaranteed to have been
// Ion compiled, and are kept alive by function objects in jitcode, and since
// the CacheLocations only have the lifespan of the jitcode, there is no need
// to trace or mark any of the scripts. Since JSScripts are always allocated
// tenured, and never moved, we can keep raw pointers, and there is no need
// for HeapPtrScripts here.
struct CacheLocation {
    jsbytecode* pc;
    JSScript* script;

    CacheLocation(jsbytecode* pcin, JSScript* scriptin)
        : pc(pcin), script(scriptin)
    { }
};

class GetPropertyIC : public IonCache
{
  protected:
    // Registers live after the cache, excluding output registers. The initial
    // value of these registers must be preserved by the cache.
    LiveRegisterSet liveRegs_;

    Register object_;
    ConstantOrRegister id_;
    TypedOrValueRegister output_;

    // Only valid if idempotent
    size_t locationsIndex_;
    size_t numLocations_;

    static const size_t MAX_FAILED_UPDATES = 16;
    uint16_t failedUpdates_;

    bool monitoredResult_ : 1;
    bool allowDoubleResult_ : 1;
    bool hasTypedArrayLengthStub_ : 1;
    bool hasMappedArgumentsLengthStub_ : 1;
    bool hasUnmappedArgumentsLengthStub_ : 1;
    bool hasMappedArgumentsElementStub_ : 1;
    bool hasUnmappedArgumentsElementStub_ : 1;
    bool hasGenericProxyStub_ : 1;
    bool hasDenseStub_ : 1;

    void emitIdGuard(MacroAssembler& masm, jsid id, Label* fail);

  public:
    GetPropertyIC(LiveRegisterSet liveRegs,
                  Register object, ConstantOrRegister id,
                  TypedOrValueRegister output,
                  bool monitoredResult, bool allowDoubleResult)
      : liveRegs_(liveRegs),
        object_(object),
        id_(id),
        output_(output),
        locationsIndex_(0),
        numLocations_(0),
        failedUpdates_(0),
        monitoredResult_(monitoredResult),
        allowDoubleResult_(allowDoubleResult),
        hasTypedArrayLengthStub_(false),
        hasMappedArgumentsLengthStub_(false),
        hasUnmappedArgumentsLengthStub_(false),
        hasMappedArgumentsElementStub_(false),
        hasUnmappedArgumentsElementStub_(false),
        hasGenericProxyStub_(false),
        hasDenseStub_(false)
    {
    }

    CACHE_HEADER(GetProperty)

    void reset(ReprotectCode reprotect);

    Register object() const {
        return object_;
    }
    ConstantOrRegister id() const {
        return id_;
    }
    TypedOrValueRegister output() const {
        return output_;
    }
    bool monitoredResult() const {
        return monitoredResult_;
    }
    bool hasTypedArrayLengthStub(HandleObject obj) const {
        return hasTypedArrayLengthStub_;
    }
    bool hasArgumentsLengthStub(bool mapped) const {
        return mapped ? hasMappedArgumentsLengthStub_ : hasUnmappedArgumentsLengthStub_;
    }
    bool hasArgumentsElementStub(bool mapped) const {
        return mapped ? hasMappedArgumentsElementStub_ : hasUnmappedArgumentsElementStub_;
    }
    bool hasGenericProxyStub() const {
        return hasGenericProxyStub_;
    }

    bool hasDenseStub() const {
        return hasDenseStub_;
    }
    void setHasDenseStub() {
        MOZ_ASSERT(!hasDenseStub());
        hasDenseStub_ = true;
    }

    void setHasTypedArrayLengthStub(HandleObject obj) {
        MOZ_ASSERT(obj->is<TypedArrayObject>());
        MOZ_ASSERT(!hasTypedArrayLengthStub_);
        hasTypedArrayLengthStub_ = true;
    }

    void setLocationInfo(size_t locationsIndex, size_t numLocations) {
        MOZ_ASSERT(idempotent());
        MOZ_ASSERT(!numLocations_);
        MOZ_ASSERT(numLocations);
        locationsIndex_ = locationsIndex;
        numLocations_ = numLocations;
    }
    void getLocationInfo(uint32_t* index, uint32_t* num) const {
        MOZ_ASSERT(idempotent());
        *index = locationsIndex_;
        *num = numLocations_;
    }

    enum NativeGetPropCacheability {
        CanAttachNone,
        CanAttachReadSlot,
        CanAttachArrayLength,
        CanAttachCallGetter
    };

    // Helpers for CanAttachNativeGetProp
    bool allowArrayLength(JSContext* cx) const;
    bool allowGetters() const {
        return monitoredResult() && !idempotent();
    }

    void maybeDisable(bool emitted);

    // Attach the proper stub, if possible
    bool tryAttachStub(JSContext* cx, HandleScript outerScript, IonScript* ion,
                       HandleObject obj, HandleValue idval, bool* emitted);

    bool tryAttachProxy(JSContext* cx, HandleScript outerScript, IonScript* ion,
                        HandleObject obj, HandleId id, void* returnAddr, bool* emitted);

    bool tryAttachGenericProxy(JSContext* cx, HandleScript outerScript, IonScript* ion,
                               HandleObject obj, HandleId id, void* returnAddr, bool* emitted);

    bool tryAttachDOMProxyShadowed(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                   HandleObject obj, HandleId id, void* returnAddr, bool* emitted);

    bool tryAttachDOMProxyUnshadowed(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                     HandleObject obj, HandleId id, bool resetNeeded,
                                     void* returnAddr, bool* emitted);

    bool tryAttachNative(JSContext* cx, HandleScript outerScript, IonScript* ion,
                         HandleObject obj, HandleId id, void* returnAddr, bool* emitted);

    bool tryAttachUnboxed(JSContext* cx, HandleScript outerScript, IonScript* ion,
                          HandleObject obj, HandleId id, void* returnAddr, bool* emitted);

    bool tryAttachUnboxedExpando(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                 HandleObject obj, HandleId id, void* returnAddr, bool* emitted);

    bool tryAttachUnboxedArrayLength(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                     HandleObject obj, HandleId id, void* returnAddr,
                                     bool* emitted);

    bool tryAttachTypedArrayLength(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                   HandleObject obj, HandleId id, bool* emitted);

    bool tryAttachArgumentsLength(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                  HandleObject obj, HandleId id, bool* emitted);

    bool tryAttachArgumentsElement(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                   HandleObject obj, HandleValue idval, bool* emitted);

    bool tryAttachDenseElement(JSContext* cx, HandleScript outerScript, IonScript* ion,
                               HandleObject obj, HandleValue idval, bool* emitted);

    static bool canAttachDenseElementHole(JSObject* obj, HandleValue idval,
                                          TypedOrValueRegister output);
    bool tryAttachDenseElementHole(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                   HandleObject obj, HandleValue idval, bool* emitted);

    static bool canAttachTypedOrUnboxedArrayElement(JSObject* obj, const Value& idval,
                                                    TypedOrValueRegister output);

    bool tryAttachTypedOrUnboxedArrayElement(JSContext* cx, HandleScript outerScript,
                                             IonScript* ion, HandleObject obj,
                                             HandleValue idval, bool* emitted);

    bool tryAttachModuleNamespace(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                  HandleObject obj, HandleId id, void* returnAddr, bool* emitted);

    static bool update(JSContext* cx, HandleScript outerScript, size_t cacheIndex,
                       HandleObject obj, HandleValue id, MutableHandleValue vp);
};

class SetPropertyIC : public IonCache
{
  protected:
    // Registers live after the cache, excluding output registers. The initial
    // value of these registers must be preserved by the cache.
    LiveRegisterSet liveRegs_;

    Register object_;
    Register temp_;
    Register tempToUnboxIndex_;
    FloatRegister tempDouble_;
    FloatRegister tempFloat32_;
    ConstantOrRegister id_;
    ConstantOrRegister value_;
    bool strict_ : 1;
    bool needsTypeBarrier_ : 1;
    bool guardHoles_ : 1;

    bool hasGenericProxyStub_ : 1;
    bool hasDenseStub_ : 1;

    void emitIdGuard(MacroAssembler& masm, jsid id, Label* fail);

  public:
    SetPropertyIC(LiveRegisterSet liveRegs, Register object, Register temp, Register tempToUnboxIndex,
                  FloatRegister tempDouble, FloatRegister tempFloat32, ConstantOrRegister id,
                  ConstantOrRegister value, bool strict, bool needsTypeBarrier, bool guardHoles)
      : liveRegs_(liveRegs),
        object_(object),
        temp_(temp),
        tempToUnboxIndex_(tempToUnboxIndex),
        tempDouble_(tempDouble),
        tempFloat32_(tempFloat32),
        id_(id),
        value_(value),
        strict_(strict),
        needsTypeBarrier_(needsTypeBarrier),
        guardHoles_(guardHoles),
        hasGenericProxyStub_(false),
        hasDenseStub_(false)
    {
    }

    CACHE_HEADER(SetProperty)

    void reset(ReprotectCode reprotect);

    Register object() const {
        return object_;
    }
    Register temp() const {
        return temp_;
    }
    Register tempToUnboxIndex() const {
        return tempToUnboxIndex_;
    }
    FloatRegister tempDouble() const {
        return tempDouble_;
    }
    FloatRegister tempFloat32() const {
        return tempFloat32_;
    }
    ConstantOrRegister id() const {
        return id_;
    }
    ConstantOrRegister value() const {
        return value_;
    }
    bool strict() const {
        return strict_;
    }
    bool needsTypeBarrier() const {
        return needsTypeBarrier_;
    }
    bool guardHoles() const {
        return guardHoles_;
    }
    bool hasGenericProxyStub() const {
        return hasGenericProxyStub_;
    }

    bool hasDenseStub() const {
        return hasDenseStub_;
    }
    void setHasDenseStub() {
        MOZ_ASSERT(!hasDenseStub());
        hasDenseStub_ = true;
    }

    enum NativeSetPropCacheability {
        CanAttachNone,
        CanAttachSetSlot,
        MaybeCanAttachAddSlot,
        CanAttachCallSetter
    };

    bool attachSetSlot(JSContext* cx, HandleScript outerScript, IonScript* ion,
                       HandleObject obj, HandleShape shape, bool checkTypeset);

    bool attachCallSetter(JSContext* cx, HandleScript outerScript, IonScript* ion,
                          HandleObject obj, HandleObject holder, HandleShape shape,
                          void* returnAddr);

    bool attachAddSlot(JSContext* cx, HandleScript outerScript, IonScript* ion,
                       HandleObject obj, HandleId id, HandleShape oldShape,
                       HandleObjectGroup oldGroup, bool checkTypeset);

    bool attachGenericProxy(JSContext* cx, HandleScript outerScript, IonScript* ion,
                            HandleId id, void* returnAddr);

    bool attachDOMProxyShadowed(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                HandleObject obj, HandleId id, void* returnAddr);

    bool attachDOMProxyUnshadowed(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                  HandleObject obj, HandleId id, void* returnAddr);

    static bool update(JSContext* cx, HandleScript outerScript, size_t cacheIndex,
                       HandleObject obj, HandleValue idval, HandleValue value);

    bool tryAttachNative(JSContext* cx, HandleScript outerScript, IonScript* ion,
                         HandleObject obj, HandleId id, bool* emitted, bool* tryNativeAddSlot);

    bool tryAttachUnboxed(JSContext* cx, HandleScript outerScript, IonScript* ion,
                          HandleObject obj, HandleId id, bool* emitted);

    bool tryAttachUnboxedExpando(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                 HandleObject obj, HandleId id, bool* emitted);

    bool tryAttachProxy(JSContext* cx, HandleScript outerScript, IonScript* ion,
                        HandleObject obj, HandleId id, bool* emitted);

    bool tryAttachStub(JSContext* cx, HandleScript outerScript, IonScript* ion,
                       HandleObject obj, HandleValue idval, HandleValue value,
                       MutableHandleId id, bool* emitted, bool* tryNativeAddSlot);

    bool tryAttachAddSlot(JSContext* cx, HandleScript outerScript, IonScript* ion,
                          HandleObject obj, HandleId id, HandleObjectGroup oldGroup,
                          HandleShape oldShape, bool tryNativeAddSlot, bool* emitted);

    bool tryAttachDenseElement(JSContext* cx, HandleScript outerScript, IonScript* ion,
                               HandleObject obj, const Value& idval, bool* emitted);

    bool tryAttachTypedArrayElement(JSContext* cx, HandleScript outerScript, IonScript* ion,
                                    HandleObject obj, HandleValue idval, HandleValue val,
                                    bool* emitted);
};

class BindNameIC : public IonCache
{
  protected:
    Register scopeChain_;
    PropertyName* name_;
    Register output_;

  public:
    BindNameIC(Register scopeChain, PropertyName* name, Register output)
      : scopeChain_(scopeChain),
        name_(name),
        output_(output)
    {
    }

    CACHE_HEADER(BindName)

    Register scopeChainReg() const {
        return scopeChain_;
    }
    HandlePropertyName name() const {
        return HandlePropertyName::fromMarkedLocation(&name_);
    }
    Register outputReg() const {
        return output_;
    }

    bool attachGlobal(JSContext* cx, HandleScript outerScript, IonScript* ion,
                      HandleObject scopeChain);

    bool attachNonGlobal(JSContext* cx, HandleScript outerScript, IonScript* ion,
                         HandleObject scopeChain, HandleObject holder);

    static JSObject*
    update(JSContext* cx, HandleScript outerScript, size_t cacheIndex, HandleObject scopeChain);
};

class NameIC : public IonCache
{
  protected:
    // Registers live after the cache, excluding output registers. The initial
    // value of these registers must be preserved by the cache.
    LiveRegisterSet liveRegs_;

    bool typeOf_;
    Register scopeChain_;
    PropertyName* name_;
    TypedOrValueRegister output_;

  public:
    NameIC(LiveRegisterSet liveRegs, bool typeOf,
           Register scopeChain, PropertyName* name,
           TypedOrValueRegister output)
      : liveRegs_(liveRegs),
        typeOf_(typeOf),
        scopeChain_(scopeChain),
        name_(name),
        output_(output)
    {
    }

    CACHE_HEADER(Name)

    Register scopeChainReg() const {
        return scopeChain_;
    }
    HandlePropertyName name() const {
        return HandlePropertyName::fromMarkedLocation(&name_);
    }
    TypedOrValueRegister outputReg() const {
        return output_;
    }
    bool isTypeOf() const {
        return typeOf_;
    }

    bool attachReadSlot(JSContext* cx, HandleScript outerScript, IonScript* ion,
                        HandleObject scopeChain, HandleObject holderBase,
                        HandleNativeObject holder, HandleShape shape);

    bool attachCallGetter(JSContext* cx, HandleScript outerScript, IonScript* ion,
                          HandleObject scopeChain, HandleObject obj, HandleObject holder,
                          HandleShape shape, void* returnAddr);

    static bool
    update(JSContext* cx, HandleScript outerScript, size_t cacheIndex, HandleObject scopeChain,
           MutableHandleValue vp);
};

#undef CACHE_HEADER

// Implement cache casts now that the compiler can see the inheritance.
#define CACHE_CASTS(ickind)                                             \
    ickind##IC& IonCache::to##ickind()                                  \
    {                                                                   \
        MOZ_ASSERT(is##ickind());                                       \
        return *static_cast<ickind##IC*>(this);                        \
    }                                                                   \
    const ickind##IC& IonCache::to##ickind() const                      \
    {                                                                   \
        MOZ_ASSERT(is##ickind());                                       \
        return *static_cast<const ickind##IC*>(this);                  \
    }
IONCACHE_KIND_LIST(CACHE_CASTS)
#undef OPCODE_CASTS

} // namespace jit
} // namespace js

#endif /* jit_IonCaches_h */
