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
#include "jit/BaselineJIT.h"
#include "jit/BaselineRegisters.h"
#include "vm/ArrayObject.h"
#include "vm/TypedArrayCommon.h"

namespace js {

class TypedArrayLayout;

namespace jit {

//
// Baseline Inline Caches are polymorphic caches that aggressively
// share their stub code.
//
// Every polymorphic site contains a linked list of stubs which are
// specific to that site.  These stubs are composed of a |StubData|
// structure that stores parametrization information (e.g.
// the shape pointer for a shape-check-and-property-get stub), any
// dynamic information (e.g. warm-up counters), a pointer to the stub code,
// and a pointer to the next stub state in the linked list.
//
// Every BaselineScript keeps an table of |CacheDescriptor| data
// structures, which store the following:
//      A pointer to the first StubData in the cache.
//      The bytecode PC of the relevant IC.
//      The machine-code PC where the call to the stubcode returns.
//
// A diagram:
//
//        Control flow                  Pointers
//      =======#                     ----.     .---->
//             #                         |     |
//             #======>                  \-----/
//
//
//                                   .---------------------------------------.
//                                   |         .-------------------------.   |
//                                   |         |         .----.          |   |
//         Baseline                  |         |         |    |          |   |
//         JIT Code              0   ^     1   ^     2   ^    |          |   |
//     +--------------+    .-->+-----+   +-----+   +-----+    |          |   |
//     |              |  #=|==>|     |==>|     |==>| FB  |    |          |   |
//     |              |  # |   +-----+   +-----+   +-----+    |          |   |
//     |              |  # |      #         #         #       |          |   |
//     |==============|==# |      #         #         #       |          |   |
//     |=== IC =======|    |      #         #         #       |          |   |
//  .->|==============|<===|======#=========#=========#       |          |   |
//  |  |              |    |                                  |          |   |
//  |  |              |    |                                  |          |   |
//  |  |              |    |                                  |          |   |
//  |  |              |    |                                  v          |   |
//  |  |              |    |                              +---------+    |   |
//  |  |              |    |                              | Fallback|    |   |
//  |  |              |    |                              | Stub    |    |   |
//  |  |              |    |                              | Code    |    |   |
//  |  |              |    |                              +---------+    |   |
//  |  +--------------+    |                                             |   |
//  |         |_______     |                              +---------+    |   |
//  |                |     |                              | Stub    |<---/   |
//  |        IC      |     \--.                           | Code    |        |
//  |    Descriptor  |        |                           +---------+        |
//  |      Table     v        |                                              |
//  |  +-----------------+    |                           +---------+        |
//  \--| Ins | PC | Stub |----/                           | Stub    |<-------/
//     +-----------------+                                | Code    |
//     |       ...       |                                +---------+
//     +-----------------+
//                                                          Shared
//                                                          Stub Code
//
//
// Type ICs
// ========
//
// Type ICs are otherwise regular ICs that are actually nested within
// other IC chains.  They serve to optimize locations in the code where the
// baseline compiler would have otherwise had to perform a type Monitor operation
// (e.g. the result of GetProp, GetElem, etc.), or locations where the baseline
// compiler would have had to modify a heap typeset using the type of an input
// value (e.g. SetProp, SetElem, etc.)
//
// There are two kinds of Type ICs: Monitor and Update.
//
// Note that type stub bodies are no-ops.  The stubs only exist for their
// guards, and their existence simply signifies that the typeset (implicit)
// that is being checked already contains that type.
//
// TypeMonitor ICs
// ---------------
// Monitor ICs are shared between stubs in the general IC, and monitor the resulting
// types of getter operations (call returns, getprop outputs, etc.)
//
//        +-----------+     +-----------+     +-----------+     +-----------+
//   ---->| Stub 1    |---->| Stub 2    |---->| Stub 3    |---->| FB Stub   |
//        +-----------+     +-----------+     +-----------+     +-----------+
//             |                  |                 |                  |
//             |------------------/-----------------/                  |
//             v                                                       |
//        +-----------+     +-----------+     +-----------+            |
//        | Type 1    |---->| Type 2    |---->| Type FB   |            |
//        +-----------+     +-----------+     +-----------+            |
//             |                 |                  |                  |
//  <----------/-----------------/------------------/------------------/
//                r e t u r n    p a t h
//
// After an optimized IC stub successfully executes, it passes control to the type stub
// chain to check the resulting type.  If no type stub succeeds, and the monitor fallback
// stub is reached, the monitor fallback stub performs a manual monitor, and also adds the
// appropriate type stub to the chain.
//
// The IC's main fallback, in addition to generating new mainline stubs, also generates
// type stubs as reflected by its returned value.
//
// NOTE: The type IC chain returns directly to the mainline code, not back to the
// stub it was entered from.  Thus, entering a type IC is a matter of a |jump|, not
// a |call|.  This allows us to safely call a VM Monitor function from within the monitor IC's
// fallback chain, since the return address (needed for stack inspection) is preserved.
//
//
// TypeUpdate ICs
// --------------
// Update ICs update heap typesets and monitor the input types of setter operations
// (setelem, setprop inputs, etc.).  Unlike monitor ICs, they are not shared
// between stubs on an IC, but instead are kept track of on a per-stub basis.
//
// This is because the main stubs for the operation will each identify a potentially
// different ObjectGroup to update.  New input types must be tracked on a group-to-
// group basis.
//
// Type-update ICs cannot be called in tail position (they must return to the
// the stub that called them so that the stub may continue to perform its original
// purpose).  This means that any VMCall to perform a manual type update from C++ must be
// done from within the main IC stub.  This necessitates that the stub enter a
// "BaselineStub" frame before making the call.
//
// If the type-update IC chain could itself make the VMCall, then the BaselineStub frame
// must be entered before calling the type-update chain, and exited afterward.  This
// is very expensive for a common case where we expect the type-update fallback to not
// be called.  To avoid the cost of entering and exiting a BaselineStub frame when
// using the type-update IC chain, we design the chain to not perform any VM-calls
// in its fallback.
//
// Instead, the type-update IC chain is responsible for returning 1 or 0, depending
// on if a type is represented in the chain or not.  The fallback stub simply returns
// 0, and all other optimized stubs return 1.
// If the chain returns 1, then the IC stub goes ahead and performs its operation.
// If the chain returns 0, then the IC stub performs a call to the fallback function
// inline (doing the requisite BaselineStub frame enter/exit).
// This allows us to avoid the expensive subfram enter/exit in the common case.
//
//                                 r e t u r n    p a t h
//   <--------------.-----------------.-----------------.-----------------.
//                  |                 |                 |                 |
//        +-----------+     +-----------+     +-----------+     +-----------+
//   ---->| Stub 1    |---->| Stub 2    |---->| Stub 3    |---->| FB Stub   |
//        +-----------+     +-----------+     +-----------+     +-----------+
//          |   ^             |   ^             |   ^
//          |   |             |   |             |   |
//          |   |             |   |             |   |----------------.
//          |   |             |   |             v   |1               |0
//          |   |             |   |         +-----------+    +-----------+
//          |   |             |   |         | Type 3.1  |--->|    FB 3   |
//          |   |             |   |         +-----------+    +-----------+
//          |   |             |   |
//          |   |             |   \-------------.-----------------.
//          |   |             |   |             |                 |
//          |   |             v   |1            |1                |0
//          |   |         +-----------+     +-----------+     +-----------+
//          |   |         | Type 2.1  |---->| Type 2.2  |---->|    FB 2   |
//          |   |         +-----------+     +-----------+     +-----------+
//          |   |
//          |   \-------------.-----------------.
//          |   |             |                 |
//          v   |1            |1                |0
//     +-----------+     +-----------+     +-----------+
//     | Type 1.1  |---->| Type 1.2  |---->|   FB 1    |
//     +-----------+     +-----------+     +-----------+
//

class ICStub;
class ICFallbackStub;

//
// An entry in the Baseline IC descriptor table.
//
class ICEntry
{
  private:
    // A pointer to the baseline IC stub for this instruction.
    ICStub* firstStub_;

    // Offset from the start of the JIT code where the IC
    // load and call instructions are.
    uint32_t returnOffset_;

    // The PC of this IC's bytecode op within the JSScript.
    uint32_t pcOffset_ : 28;

  public:
    enum Kind {
        // A for-op IC entry.
        Kind_Op = 0,

        // A non-op IC entry.
        Kind_NonOp,

        // A fake IC entry for returning from a callVM for an op.
        Kind_CallVM,

        // A fake IC entry for returning from a callVM not for an op (e.g., in
        // the prologue).
        Kind_NonOpCallVM,

        // A fake IC entry for returning from a callVM to the interrupt
        // handler via the over-recursion check on function entry.
        Kind_StackCheck,

        // As above, but for the early check. See emitStackCheck.
        Kind_EarlyStackCheck,

        // A fake IC entry for returning from DebugTrapHandler.
        Kind_DebugTrap,

        // A fake IC entry for returning from a callVM to
        // Debug{Prologue,Epilogue}.
        Kind_DebugPrologue,
        Kind_DebugEpilogue,

        Kind_Invalid
    };

  private:
    // What this IC is for.
    Kind kind_ : 4;

    // Set the kind and asserts that it's sane.
    void setKind(Kind kind) {
        MOZ_ASSERT(kind < Kind_Invalid);
        kind_ = kind;
        MOZ_ASSERT(this->kind() == kind);
    }

  public:
    ICEntry(uint32_t pcOffset, Kind kind)
      : firstStub_(nullptr), returnOffset_(), pcOffset_(pcOffset)
    {
        // The offset must fit in at least 28 bits, since we shave off 4 for
        // the Kind enum.
        MOZ_ASSERT(pcOffset_ == pcOffset);
        JS_STATIC_ASSERT(BaselineScript::MAX_JSSCRIPT_LENGTH <= (1u << 28) - 1);
        MOZ_ASSERT(pcOffset <= BaselineScript::MAX_JSSCRIPT_LENGTH);
        setKind(kind);
    }

    CodeOffsetLabel returnOffset() const {
        return CodeOffsetLabel(returnOffset_);
    }

    void setReturnOffset(CodeOffsetLabel offset) {
        MOZ_ASSERT(offset.offset() <= (size_t) UINT32_MAX);
        returnOffset_ = (uint32_t) offset.offset();
    }

    void fixupReturnOffset(MacroAssembler& masm) {
        CodeOffsetLabel offset = returnOffset();
        offset.fixup(&masm);
        MOZ_ASSERT(offset.offset() <= UINT32_MAX);
        returnOffset_ = (uint32_t) offset.offset();
    }

    uint32_t pcOffset() const {
        return pcOffset_;
    }

    jsbytecode* pc(JSScript* script) const {
        return script->offsetToPC(pcOffset_);
    }

    Kind kind() const {
        // MSVC compiles enums as signed.
        return Kind(kind_ & 0xf);
    }
    bool isForOp() const {
        return kind() == Kind_Op;
    }

    void setFakeKind(Kind kind) {
        MOZ_ASSERT(kind != Kind_Op && kind != Kind_NonOp);
        setKind(kind);
    }

    bool hasStub() const {
        return firstStub_ != nullptr;
    }
    ICStub* firstStub() const {
        MOZ_ASSERT(hasStub());
        return firstStub_;
    }

    ICFallbackStub* fallbackStub() const;

    void setFirstStub(ICStub* stub) {
        firstStub_ = stub;
    }

    static inline size_t offsetOfFirstStub() {
        return offsetof(ICEntry, firstStub_);
    }

    inline ICStub** addressOfFirstStub() {
        return &firstStub_;
    }
};

// List of baseline IC stub kinds.
#define IC_STUB_KIND_LIST(_)    \
    _(WarmUpCounter_Fallback)   \
                                \
    _(TypeMonitor_Fallback)     \
    _(TypeMonitor_SingleObject) \
    _(TypeMonitor_ObjectGroup)  \
    _(TypeMonitor_PrimitiveSet) \
                                \
    _(TypeUpdate_Fallback)      \
    _(TypeUpdate_SingleObject)  \
    _(TypeUpdate_ObjectGroup)   \
    _(TypeUpdate_PrimitiveSet)  \
                                \
    _(This_Fallback)            \
                                \
    _(NewArray_Fallback)        \
    _(NewObject_Fallback)       \
                                \
    _(Compare_Fallback)         \
    _(Compare_Int32)            \
    _(Compare_Double)           \
    _(Compare_NumberWithUndefined) \
    _(Compare_String)           \
    _(Compare_Boolean)          \
    _(Compare_Object)           \
    _(Compare_ObjectWithUndefined) \
    _(Compare_Int32WithBoolean) \
                                \
    _(ToBool_Fallback)          \
    _(ToBool_Int32)             \
    _(ToBool_String)            \
    _(ToBool_NullUndefined)     \
    _(ToBool_Double)            \
    _(ToBool_Object)            \
                                \
    _(ToNumber_Fallback)        \
                                \
    _(BinaryArith_Fallback)     \
    _(BinaryArith_Int32)        \
    _(BinaryArith_Double)       \
    _(BinaryArith_StringConcat) \
    _(BinaryArith_StringObjectConcat) \
    _(BinaryArith_BooleanWithInt32) \
    _(BinaryArith_DoubleWithInt32) \
                                \
    _(UnaryArith_Fallback)      \
    _(UnaryArith_Int32)         \
    _(UnaryArith_Double)        \
                                \
    _(Call_Fallback)            \
    _(Call_Scripted)            \
    _(Call_AnyScripted)         \
    _(Call_Native)              \
    _(Call_ClassHook)           \
    _(Call_ScriptedApplyArray)  \
    _(Call_ScriptedApplyArguments) \
    _(Call_ScriptedFunCall)     \
    _(Call_StringSplit)         \
    _(Call_IsSuspendedStarGenerator) \
                                \
    _(GetElem_Fallback)         \
    _(GetElem_NativeSlot)       \
    _(GetElem_NativePrototypeSlot) \
    _(GetElem_NativePrototypeCallNative) \
    _(GetElem_NativePrototypeCallScripted) \
    _(GetElem_String)           \
    _(GetElem_Dense)            \
    _(GetElem_TypedArray)       \
    _(GetElem_Arguments)        \
                                \
    _(SetElem_Fallback)         \
    _(SetElem_Dense)            \
    _(SetElem_DenseAdd)         \
    _(SetElem_TypedArray)       \
                                \
    _(In_Fallback)              \
                                \
    _(GetName_Fallback)         \
    _(GetName_Global)           \
    _(GetName_Scope0)           \
    _(GetName_Scope1)           \
    _(GetName_Scope2)           \
    _(GetName_Scope3)           \
    _(GetName_Scope4)           \
    _(GetName_Scope5)           \
    _(GetName_Scope6)           \
                                \
    _(BindName_Fallback)        \
                                \
    _(GetIntrinsic_Fallback)    \
    _(GetIntrinsic_Constant)    \
                                \
    _(GetProp_Fallback)         \
    _(GetProp_ArrayLength)      \
    _(GetProp_Primitive)        \
    _(GetProp_StringLength)     \
    _(GetProp_Native)           \
    _(GetProp_NativeDoesNotExist) \
    _(GetProp_NativePrototype)  \
    _(GetProp_UnboxedPrototype) \
    _(GetProp_Unboxed)          \
    _(GetProp_TypedObject)      \
    _(GetProp_CallScripted)     \
    _(GetProp_CallNative)       \
    _(GetProp_CallNativePrototype)\
    _(GetProp_CallDOMProxyNative)\
    _(GetProp_CallDOMProxyWithGenerationNative)\
    _(GetProp_DOMProxyShadowed) \
    _(GetProp_ArgumentsLength)  \
    _(GetProp_ArgumentsCallee)  \
    _(GetProp_Generic)          \
                                \
    _(SetProp_Fallback)         \
    _(SetProp_Native)           \
    _(SetProp_NativeAdd)        \
    _(SetProp_Unboxed)          \
    _(SetProp_TypedObject)      \
    _(SetProp_CallScripted)     \
    _(SetProp_CallNative)       \
                                \
    _(TableSwitch)              \
                                \
    _(IteratorNew_Fallback)     \
    _(IteratorMore_Fallback)    \
    _(IteratorMore_Native)      \
    _(IteratorClose_Fallback)   \
                                \
    _(InstanceOf_Fallback)      \
    _(InstanceOf_Function)      \
                                \
    _(TypeOf_Fallback)          \
    _(TypeOf_Typed)             \
                                \
    _(Rest_Fallback)            \
                                \
    _(RetSub_Fallback)          \
    _(RetSub_Resume)

#define FORWARD_DECLARE_STUBS(kindName) class IC##kindName;
    IC_STUB_KIND_LIST(FORWARD_DECLARE_STUBS)
#undef FORWARD_DECLARE_STUBS

class ICMonitoredStub;
class ICMonitoredFallbackStub;
class ICUpdatedStub;

// Constant iterator that traverses arbitrary chains of ICStubs.
// No requirements are made of the ICStub used to construct this
// iterator, aside from that the stub be part of a nullptr-terminated
// chain.
// The iterator is considered to be at its end once it has been
// incremented _past_ the last stub.  Thus, if 'atEnd()' returns
// true, the '*' and '->' operations are not valid.
class ICStubConstIterator
{
    friend class ICStub;
    friend class ICFallbackStub;

  private:
    ICStub* currentStub_;

  public:
    explicit ICStubConstIterator(ICStub* currentStub) : currentStub_(currentStub) {}

    static ICStubConstIterator StartingAt(ICStub* stub) {
        return ICStubConstIterator(stub);
    }
    static ICStubConstIterator End(ICStub* stub) {
        return ICStubConstIterator(nullptr);
    }

    bool operator ==(const ICStubConstIterator& other) const {
        return currentStub_ == other.currentStub_;
    }
    bool operator !=(const ICStubConstIterator& other) const {
        return !(*this == other);
    }

    ICStubConstIterator& operator++();

    ICStubConstIterator operator++(int) {
        ICStubConstIterator oldThis(*this);
        ++(*this);
        return oldThis;
    }

    ICStub* operator*() const {
        MOZ_ASSERT(currentStub_);
        return currentStub_;
    }

    ICStub* operator ->() const {
        MOZ_ASSERT(currentStub_);
        return currentStub_;
    }

    bool atEnd() const {
        return currentStub_ == nullptr;
    }
};

// Iterator that traverses "regular" IC chains that start at an ICEntry
// and are terminated with an ICFallbackStub.
//
// The iterator is considered to be at its end once it is _at_ the
// fallback stub.  Thus, unlike the ICStubConstIterator, operators
// '*' and '->' are valid even if 'atEnd()' returns true - they
// will act on the fallback stub.
//
// This iterator also allows unlinking of stubs being traversed.
// Note that 'unlink' does not implicitly advance the iterator -
// it must be advanced explicitly using '++'.
class ICStubIterator
{
    friend class ICFallbackStub;

  private:
    ICEntry* icEntry_;
    ICFallbackStub* fallbackStub_;
    ICStub* previousStub_;
    ICStub* currentStub_;
    bool unlinked_;

    explicit ICStubIterator(ICFallbackStub* fallbackStub, bool end=false);
  public:

    bool operator ==(const ICStubIterator& other) const {
        // == should only ever be called on stubs from the same chain.
        MOZ_ASSERT(icEntry_ == other.icEntry_);
        MOZ_ASSERT(fallbackStub_ == other.fallbackStub_);
        return currentStub_ == other.currentStub_;
    }
    bool operator !=(const ICStubIterator& other) const {
        return !(*this == other);
    }

    ICStubIterator& operator++();

    ICStubIterator operator++(int) {
        ICStubIterator oldThis(*this);
        ++(*this);
        return oldThis;
    }

    ICStub* operator*() const {
        return currentStub_;
    }

    ICStub* operator ->() const {
        return currentStub_;
    }

    bool atEnd() const {
        return currentStub_ == (ICStub*) fallbackStub_;
    }

    void unlink(JSContext* cx);
};

//
// Base class for all IC stubs.
//
class ICStub
{
    friend class ICFallbackStub;

  public:
    enum Kind {
        INVALID = 0,
#define DEF_ENUM_KIND(kindName) kindName,
        IC_STUB_KIND_LIST(DEF_ENUM_KIND)
#undef DEF_ENUM_KIND
        LIMIT
    };

    static inline bool IsValidKind(Kind k) {
        return (k > INVALID) && (k < LIMIT);
    }

    static const char* KindString(Kind k) {
        switch(k) {
#define DEF_KIND_STR(kindName) case kindName: return #kindName;
            IC_STUB_KIND_LIST(DEF_KIND_STR)
#undef DEF_KIND_STR
          default:
            MOZ_CRASH("Invalid kind.");
        }
    }

    enum Trait {
        Regular             = 0x0,
        Fallback            = 0x1,
        Monitored           = 0x2,
        MonitoredFallback   = 0x3,
        Updated             = 0x4
    };

    void markCode(JSTracer* trc, const char* name);
    void updateCode(JitCode* stubCode);
    void trace(JSTracer* trc);

    template <typename T, typename... Args>
    static T* New(ICStubSpace* space, JitCode* code, Args&&... args) {
        if (!code)
            return nullptr;
        return space->allocate<T>(code, mozilla::Forward<Args>(args)...);
    }

  protected:
    // The raw jitcode to call for this stub.
    uint8_t* stubCode_;

    // Pointer to next IC stub.  This is null for the last IC stub, which should
    // either be a fallback or inert IC stub.
    ICStub* next_;

    // A 16-bit field usable by subtypes of ICStub for subtype-specific small-info
    uint16_t extra_;

    // The kind of the stub.
    //  High bit is 'isFallback' flag.
    //  Second high bit is 'isMonitored' flag.
    Trait trait_ : 3;
    Kind kind_ : 13;

    inline ICStub(Kind kind, JitCode* stubCode)
      : stubCode_(stubCode->raw()),
        next_(nullptr),
        extra_(0),
        trait_(Regular),
        kind_(kind)
    {
        MOZ_ASSERT(stubCode != nullptr);
    }

    inline ICStub(Kind kind, Trait trait, JitCode* stubCode)
      : stubCode_(stubCode->raw()),
        next_(nullptr),
        extra_(0),
        trait_(trait),
        kind_(kind)
    {
        MOZ_ASSERT(stubCode != nullptr);
    }

    inline Trait trait() const {
        // Workaround for MSVC reading trait_ as signed value.
        return (Trait)(trait_ & 0x7);
    }

  public:

    inline Kind kind() const {
        return static_cast<Kind>(kind_);
    }

    inline bool isFallback() const {
        return trait() == Fallback || trait() == MonitoredFallback;
    }

    inline bool isMonitored() const {
        return trait() == Monitored;
    }

    inline bool isUpdated() const {
        return trait() == Updated;
    }

    inline bool isMonitoredFallback() const {
        return trait() == MonitoredFallback;
    }

    inline const ICFallbackStub* toFallbackStub() const {
        MOZ_ASSERT(isFallback());
        return reinterpret_cast<const ICFallbackStub*>(this);
    }

    inline ICFallbackStub* toFallbackStub() {
        MOZ_ASSERT(isFallback());
        return reinterpret_cast<ICFallbackStub*>(this);
    }

    inline const ICMonitoredStub* toMonitoredStub() const {
        MOZ_ASSERT(isMonitored());
        return reinterpret_cast<const ICMonitoredStub*>(this);
    }

    inline ICMonitoredStub* toMonitoredStub() {
        MOZ_ASSERT(isMonitored());
        return reinterpret_cast<ICMonitoredStub*>(this);
    }

    inline const ICMonitoredFallbackStub* toMonitoredFallbackStub() const {
        MOZ_ASSERT(isMonitoredFallback());
        return reinterpret_cast<const ICMonitoredFallbackStub*>(this);
    }

    inline ICMonitoredFallbackStub* toMonitoredFallbackStub() {
        MOZ_ASSERT(isMonitoredFallback());
        return reinterpret_cast<ICMonitoredFallbackStub*>(this);
    }

    inline const ICUpdatedStub* toUpdatedStub() const {
        MOZ_ASSERT(isUpdated());
        return reinterpret_cast<const ICUpdatedStub*>(this);
    }

    inline ICUpdatedStub* toUpdatedStub() {
        MOZ_ASSERT(isUpdated());
        return reinterpret_cast<ICUpdatedStub*>(this);
    }

#define KIND_METHODS(kindName)   \
    inline bool is##kindName() const { return kind() == kindName; } \
    inline const IC##kindName* to##kindName() const { \
        MOZ_ASSERT(is##kindName()); \
        return reinterpret_cast<const IC##kindName*>(this); \
    } \
    inline IC##kindName* to##kindName() { \
        MOZ_ASSERT(is##kindName()); \
        return reinterpret_cast<IC##kindName*>(this); \
    }
    IC_STUB_KIND_LIST(KIND_METHODS)
#undef KIND_METHODS

    inline ICStub* next() const {
        return next_;
    }

    inline bool hasNext() const {
        return next_ != nullptr;
    }

    inline void setNext(ICStub* stub) {
        // Note: next_ only needs to be changed under the compilation lock for
        // non-type-monitor/update ICs.
        next_ = stub;
    }

    inline ICStub** addressOfNext() {
        return &next_;
    }

    inline JitCode* jitCode() {
        return JitCode::FromExecutable(stubCode_);
    }

    inline uint8_t* rawStubCode() const {
        return stubCode_;
    }

    // This method is not valid on TypeUpdate stub chains!
    inline ICFallbackStub* getChainFallback() {
        ICStub* lastStub = this;
        while (lastStub->next_)
            lastStub = lastStub->next_;
        MOZ_ASSERT(lastStub->isFallback());
        return lastStub->toFallbackStub();
    }

    inline ICStubConstIterator beginHere() {
        return ICStubConstIterator::StartingAt(this);
    }

    static inline size_t offsetOfNext() {
        return offsetof(ICStub, next_);
    }

    static inline size_t offsetOfStubCode() {
        return offsetof(ICStub, stubCode_);
    }

    static inline size_t offsetOfExtra() {
        return offsetof(ICStub, extra_);
    }

    static bool CanMakeCalls(ICStub::Kind kind) {
        MOZ_ASSERT(IsValidKind(kind));
        switch (kind) {
          case Call_Fallback:
          case Call_Scripted:
          case Call_AnyScripted:
          case Call_Native:
          case Call_ClassHook:
          case Call_ScriptedApplyArray:
          case Call_ScriptedApplyArguments:
          case Call_ScriptedFunCall:
          case Call_StringSplit:
          case WarmUpCounter_Fallback:
          case GetElem_NativeSlot:
          case GetElem_NativePrototypeSlot:
          case GetElem_NativePrototypeCallNative:
          case GetElem_NativePrototypeCallScripted:
          case GetProp_CallScripted:
          case GetProp_CallNative:
          case GetProp_CallNativePrototype:
          case GetProp_CallDOMProxyNative:
          case GetProp_CallDOMProxyWithGenerationNative:
          case GetProp_DOMProxyShadowed:
          case GetProp_Generic:
          case SetProp_CallScripted:
          case SetProp_CallNative:
          case RetSub_Fallback:
          // These two fallback stubs don't actually make non-tail calls,
          // but the fallback code for the bailout path needs to pop the stub frame
          // pushed during the bailout.
          case GetProp_Fallback:
          case SetProp_Fallback:
#if JS_HAS_NO_SUCH_METHOD
          case GetElem_Dense:
          case GetElem_Arguments:
          case GetProp_NativePrototype:
          case GetProp_UnboxedPrototype:
          case GetProp_Native:
#endif
            return true;
          default:
            return false;
        }
    }

    // Optimized stubs get purged on GC.  But some stubs can be active on the
    // stack during GC - specifically the ones that can make calls.  To ensure
    // that these do not get purged, all stubs that can make calls are allocated
    // in the fallback stub space.
    bool allocatedInFallbackSpace() const {
        MOZ_ASSERT(next());
        return CanMakeCalls(kind());
    }
};

class ICFallbackStub : public ICStub
{
    friend class ICStubConstIterator;
  protected:
    // Fallback stubs need these fields to easily add new stubs to
    // the linked list of stubs for an IC.

    // The IC entry for this linked list of stubs.
    ICEntry* icEntry_;

    // The number of stubs kept in the IC entry.
    uint32_t numOptimizedStubs_;

    // A pointer to the location stub pointer that needs to be
    // changed to add a new "last" stub immediately before the fallback
    // stub.  This'll start out pointing to the icEntry's "firstStub_"
    // field, and as new stubs are addd, it'll point to the current
    // last stub's "next_" field.
    ICStub** lastStubPtrAddr_;

    ICFallbackStub(Kind kind, JitCode* stubCode)
      : ICStub(kind, ICStub::Fallback, stubCode),
        icEntry_(nullptr),
        numOptimizedStubs_(0),
        lastStubPtrAddr_(nullptr) {}

    ICFallbackStub(Kind kind, Trait trait, JitCode* stubCode)
      : ICStub(kind, trait, stubCode),
        icEntry_(nullptr),
        numOptimizedStubs_(0),
        lastStubPtrAddr_(nullptr)
    {
        MOZ_ASSERT(trait == ICStub::Fallback ||
                   trait == ICStub::MonitoredFallback);
    }

  public:
    inline ICEntry* icEntry() const {
        return icEntry_;
    }

    inline size_t numOptimizedStubs() const {
        return (size_t) numOptimizedStubs_;
    }

    // The icEntry and lastStubPtrAddr_ fields can't be initialized when the stub is
    // created since the stub is created at compile time, and we won't know the IC entry
    // address until after compile when the BaselineScript is created.  This method
    // allows these fields to be fixed up at that point.
    void fixupICEntry(ICEntry* icEntry) {
        MOZ_ASSERT(icEntry_ == nullptr);
        MOZ_ASSERT(lastStubPtrAddr_ == nullptr);
        icEntry_ = icEntry;
        lastStubPtrAddr_ = icEntry_->addressOfFirstStub();
    }

    // Add a new stub to the IC chain terminated by this fallback stub.
    void addNewStub(ICStub* stub) {
        MOZ_ASSERT(*lastStubPtrAddr_ == this);
        MOZ_ASSERT(stub->next() == nullptr);
        stub->setNext(this);
        *lastStubPtrAddr_ = stub;
        lastStubPtrAddr_ = stub->addressOfNext();
        numOptimizedStubs_++;
    }

    ICStubConstIterator beginChainConst() const {
        return ICStubConstIterator(icEntry_->firstStub());
    }

    ICStubIterator beginChain() {
        return ICStubIterator(this);
    }

    bool hasStub(ICStub::Kind kind) const {
        for (ICStubConstIterator iter = beginChainConst(); !iter.atEnd(); iter++) {
            if (iter->kind() == kind)
                return true;
        }
        return false;
    }

    unsigned numStubsWithKind(ICStub::Kind kind) const {
        unsigned count = 0;
        for (ICStubConstIterator iter = beginChainConst(); !iter.atEnd(); iter++) {
            if (iter->kind() == kind)
                count++;
        }
        return count;
    }

    void unlinkStub(Zone* zone, ICStub* prev, ICStub* stub);
    void unlinkStubsWithKind(JSContext* cx, ICStub::Kind kind);
};

// Monitored stubs are IC stubs that feed a single resulting value out to a
// type monitor operation.
class ICMonitoredStub : public ICStub
{
  protected:
    // Pointer to the start of the type monitoring stub chain.
    ICStub* firstMonitorStub_;

    ICMonitoredStub(Kind kind, JitCode* stubCode, ICStub* firstMonitorStub);

  public:
    inline void updateFirstMonitorStub(ICStub* monitorStub) {
        // This should only be called once: when the first optimized monitor stub
        // is added to the type monitor IC chain.
        MOZ_ASSERT(firstMonitorStub_ && firstMonitorStub_->isTypeMonitor_Fallback());
        firstMonitorStub_ = monitorStub;
    }
    inline void resetFirstMonitorStub(ICStub* monitorFallback) {
        MOZ_ASSERT(monitorFallback->isTypeMonitor_Fallback());
        firstMonitorStub_ = monitorFallback;
    }
    inline ICStub* firstMonitorStub() const {
        return firstMonitorStub_;
    }

    static inline size_t offsetOfFirstMonitorStub() {
        return offsetof(ICMonitoredStub, firstMonitorStub_);
    }
};

// Monitored fallback stubs - as the name implies.
class ICMonitoredFallbackStub : public ICFallbackStub
{
  protected:
    // Pointer to the fallback monitor stub.
    ICTypeMonitor_Fallback* fallbackMonitorStub_;

    ICMonitoredFallbackStub(Kind kind, JitCode* stubCode)
      : ICFallbackStub(kind, ICStub::MonitoredFallback, stubCode),
        fallbackMonitorStub_(nullptr) {}

  public:
    bool initMonitoringChain(JSContext* cx, ICStubSpace* space);
    bool addMonitorStubForValue(JSContext* cx, JSScript* script, HandleValue val);

    inline ICTypeMonitor_Fallback* fallbackMonitorStub() const {
        return fallbackMonitorStub_;
    }

    static inline size_t offsetOfFallbackMonitorStub() {
        return offsetof(ICMonitoredFallbackStub, fallbackMonitorStub_);
    }
};

// Updated stubs are IC stubs that use a TypeUpdate IC to track
// the status of heap typesets that need to be updated.
class ICUpdatedStub : public ICStub
{
  protected:
    // Pointer to the start of the type updating stub chain.
    ICStub* firstUpdateStub_;

    static const uint32_t MAX_OPTIMIZED_STUBS = 8;
    uint32_t numOptimizedStubs_;

    ICUpdatedStub(Kind kind, JitCode* stubCode)
      : ICStub(kind, ICStub::Updated, stubCode),
        firstUpdateStub_(nullptr),
        numOptimizedStubs_(0)
    {}

  public:
    bool initUpdatingChain(JSContext* cx, ICStubSpace* space);

    bool addUpdateStubForValue(JSContext* cx, HandleScript script, HandleObject obj, HandleId id,
                               HandleValue val);

    void addOptimizedUpdateStub(ICStub* stub) {
        if (firstUpdateStub_->isTypeUpdate_Fallback()) {
            stub->setNext(firstUpdateStub_);
            firstUpdateStub_ = stub;
        } else {
            ICStub* iter = firstUpdateStub_;
            MOZ_ASSERT(iter->next() != nullptr);
            while (!iter->next()->isTypeUpdate_Fallback())
                iter = iter->next();
            MOZ_ASSERT(iter->next()->next() == nullptr);
            stub->setNext(iter->next());
            iter->setNext(stub);
        }

        numOptimizedStubs_++;
    }

    inline ICStub* firstUpdateStub() const {
        return firstUpdateStub_;
    }

    bool hasTypeUpdateStub(ICStub::Kind kind) {
        ICStub* stub = firstUpdateStub_;
        do {
            if (stub->kind() == kind)
                return true;

            stub = stub->next();
        } while (stub);

        return false;
    }

    inline uint32_t numOptimizedStubs() const {
        return numOptimizedStubs_;
    }

    static inline size_t offsetOfFirstUpdateStub() {
        return offsetof(ICUpdatedStub, firstUpdateStub_);
    }
};

// Base class for stubcode compilers.
class ICStubCompiler
{
    // Prevent GC in the middle of stub compilation.
    js::gc::AutoSuppressGC suppressGC;


  protected:
    JSContext* cx;
    ICStub::Kind kind;
#ifdef DEBUG
    bool entersStubFrame_;
#endif

    // By default the stubcode key is just the kind.
    virtual int32_t getKey() const {
        return static_cast<int32_t>(kind);
    }

    virtual bool generateStubCode(MacroAssembler& masm) = 0;
    virtual bool postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> genCode) {
        return true;
    }
    JitCode* getStubCode();

    ICStubCompiler(JSContext* cx, ICStub::Kind kind)
      : suppressGC(cx), cx(cx), kind(kind)
#ifdef DEBUG
      , entersStubFrame_(false)
#endif
    {}

    // Emits a tail call to a VMFunction wrapper.
    bool tailCallVM(const VMFunction& fun, MacroAssembler& masm);

    // Emits a normal (non-tail) call to a VMFunction wrapper.
    bool callVM(const VMFunction& fun, MacroAssembler& masm);

    // Emits a call to a type-update IC, assuming that the value to be
    // checked is already in R0.
    bool callTypeUpdateIC(MacroAssembler& masm, uint32_t objectOffset);

    // A stub frame is used when a stub wants to call into the VM without
    // performing a tail call. This is required for the return address
    // to pc mapping to work.
    void enterStubFrame(MacroAssembler& masm, Register scratch);
    void leaveStubFrame(MacroAssembler& masm, bool calledIntoIon = false);

    // Some stubs need to emit SPS profiler updates.  This emits the guarding
    // jitcode for those stubs.  If profiling is not enabled, jumps to the
    // given label.
    void guardProfilingEnabled(MacroAssembler& masm, Register scratch, Label* skip);

    inline GeneralRegisterSet availableGeneralRegs(size_t numInputs) const {
        GeneralRegisterSet regs(GeneralRegisterSet::All());
        MOZ_ASSERT(!regs.has(BaselineStackReg));
#if defined(JS_CODEGEN_ARM)
        MOZ_ASSERT(!regs.has(BaselineTailCallReg));
        regs.take(BaselineSecondScratchReg);
#elif defined(JS_CODEGEN_MIPS)
        MOZ_ASSERT(!regs.has(BaselineTailCallReg));
        MOZ_ASSERT(!regs.has(BaselineSecondScratchReg));
#endif
        regs.take(BaselineFrameReg);
        regs.take(BaselineStubReg);
#ifdef JS_CODEGEN_X64
        regs.take(ExtractTemp0);
        regs.take(ExtractTemp1);
#endif

        switch (numInputs) {
          case 0:
            break;
          case 1:
            regs.take(R0);
            break;
          case 2:
            regs.take(R0);
            regs.take(R1);
            break;
          default:
            MOZ_CRASH("Invalid numInputs");
        }

        return regs;
    }

    inline bool emitPostWriteBarrierSlot(MacroAssembler& masm, Register obj, ValueOperand val,
                                         Register scratch, GeneralRegisterSet saveRegs);

  public:
    virtual ICStub* getStub(ICStubSpace* space) = 0;

    static ICStubSpace* StubSpaceForKind(ICStub::Kind kind, JSScript* script) {
        if (ICStub::CanMakeCalls(kind))
            return script->baselineScript()->fallbackStubSpace();
        return script->zone()->jitZone()->optimizedStubSpace();
    }

    ICStubSpace* getStubSpace(JSScript* script) {
        return StubSpaceForKind(kind, script);
    }
};

// Base class for stub compilers that can generate multiple stubcodes.
// These compilers need access to the JSOp they are compiling for.
class ICMultiStubCompiler : public ICStubCompiler
{
  protected:
    JSOp op;

    // Stub keys for multi-stub kinds are composed of both the kind
    // and the op they are compiled for.
    virtual int32_t getKey() const {
        return static_cast<int32_t>(kind) | (static_cast<int32_t>(op) << 16);
    }

    ICMultiStubCompiler(JSContext* cx, ICStub::Kind kind, JSOp op)
      : ICStubCompiler(cx, kind), op(op) {}
};

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
          : ICStubCompiler(cx, ICStub::WarmUpCounter_Fallback)
        { }

        ICWarmUpCounter_Fallback* getStub(ICStubSpace* space) {
            return ICStub::New<ICWarmUpCounter_Fallback>(space, getStubCode());
        }
    };
};


// TypeCheckPrimitiveSetStub
//   Base class for IC stubs (TypeUpdate or TypeMonitor) that check that a given
//   value's type falls within a set of primitive types.

class TypeCheckPrimitiveSetStub : public ICStub
{
    friend class ICStubSpace;
  protected:
    inline static uint16_t TypeToFlag(JSValueType type) {
        return 1u << static_cast<unsigned>(type);
    }

    inline static uint16_t ValidFlags() {
        return ((TypeToFlag(JSVAL_TYPE_OBJECT) << 1) - 1) & ~TypeToFlag(JSVAL_TYPE_MAGIC);
    }

    TypeCheckPrimitiveSetStub(Kind kind, JitCode* stubCode, uint16_t flags)
        : ICStub(kind, stubCode)
    {
        MOZ_ASSERT(kind == TypeMonitor_PrimitiveSet || kind == TypeUpdate_PrimitiveSet);
        MOZ_ASSERT(flags && !(flags & ~ValidFlags()));
        extra_ = flags;
    }

    TypeCheckPrimitiveSetStub* updateTypesAndCode(uint16_t flags, JitCode* code) {
        MOZ_ASSERT(flags && !(flags & ~ValidFlags()));
        if (!code)
            return nullptr;
        extra_ = flags;
        updateCode(code);
        return this;
    }

  public:
    uint16_t typeFlags() const {
        return extra_;
    }

    bool containsType(JSValueType type) const {
        MOZ_ASSERT(type <= JSVAL_TYPE_OBJECT);
        MOZ_ASSERT(type != JSVAL_TYPE_MAGIC);
        return extra_ & TypeToFlag(type);
    }

    ICTypeMonitor_PrimitiveSet* toMonitorStub() {
        return toTypeMonitor_PrimitiveSet();
    }

    ICTypeUpdate_PrimitiveSet* toUpdateStub() {
        return toTypeUpdate_PrimitiveSet();
    }

    class Compiler : public ICStubCompiler {
      protected:
        TypeCheckPrimitiveSetStub* existingStub_;
        uint16_t flags_;

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(flags_) << 16);
        }

      public:
        Compiler(JSContext* cx, Kind kind, TypeCheckPrimitiveSetStub* existingStub,
                 JSValueType type)
          : ICStubCompiler(cx, kind),
            existingStub_(existingStub),
            flags_((existingStub ? existingStub->typeFlags() : 0) | TypeToFlag(type))
        {
            MOZ_ASSERT_IF(existingStub_, flags_ != existingStub_->typeFlags());
        }

        TypeCheckPrimitiveSetStub* updateStub() {
            MOZ_ASSERT(existingStub_);
            return existingStub_->updateTypesAndCode(flags_, getStubCode());
        }
    };
};

// TypeMonitor

// The TypeMonitor fallback stub is not always a regular fallback stub. When
// used for monitoring the values pushed by a bytecode it doesn't hold a
// pointer to the IC entry, but rather back to the main fallback stub for the
// IC (from which a pointer to the IC entry can be retrieved). When monitoring
// the types of 'this', arguments or other values with no associated IC, there
// is no main fallback stub, and the IC entry is referenced directly.
class ICTypeMonitor_Fallback : public ICStub
{
    friend class ICStubSpace;

    static const uint32_t MAX_OPTIMIZED_STUBS = 8;

    // Pointer to the main fallback stub for the IC or to the main IC entry,
    // depending on hasFallbackStub.
    union {
        ICMonitoredFallbackStub* mainFallbackStub_;
        ICEntry* icEntry_;
    };

    // Pointer to the first monitor stub.
    ICStub* firstMonitorStub_;

    // Address of the last monitor stub's field pointing to this
    // fallback monitor stub.  This will get updated when new
    // monitor stubs are created and added.
    ICStub** lastMonitorStubPtrAddr_;

    // Count of optimized type monitor stubs in this chain.
    uint32_t numOptimizedMonitorStubs_ : 8;

    // Whether this has a fallback stub referring to the IC entry.
    bool hasFallbackStub_ : 1;

    // Index of 'this' or argument which is being monitored, or BYTECODE_INDEX
    // if this is monitoring the types of values pushed at some bytecode.
    uint32_t argumentIndex_ : 23;

    static const uint32_t BYTECODE_INDEX = (1 << 23) - 1;

    ICTypeMonitor_Fallback(JitCode* stubCode, ICMonitoredFallbackStub* mainFallbackStub,
                           uint32_t argumentIndex)
      : ICStub(ICStub::TypeMonitor_Fallback, stubCode),
        mainFallbackStub_(mainFallbackStub),
        firstMonitorStub_(thisFromCtor()),
        lastMonitorStubPtrAddr_(nullptr),
        numOptimizedMonitorStubs_(0),
        hasFallbackStub_(mainFallbackStub != nullptr),
        argumentIndex_(argumentIndex)
    { }

    ICTypeMonitor_Fallback* thisFromCtor() {
        return this;
    }

    void addOptimizedMonitorStub(ICStub* stub) {
        stub->setNext(this);

        MOZ_ASSERT((lastMonitorStubPtrAddr_ != nullptr) ==
                   (numOptimizedMonitorStubs_ || !hasFallbackStub_));

        if (lastMonitorStubPtrAddr_)
            *lastMonitorStubPtrAddr_ = stub;

        if (numOptimizedMonitorStubs_ == 0) {
            MOZ_ASSERT(firstMonitorStub_ == this);
            firstMonitorStub_ = stub;
        } else {
            MOZ_ASSERT(firstMonitorStub_ != nullptr);
        }

        lastMonitorStubPtrAddr_ = stub->addressOfNext();
        numOptimizedMonitorStubs_++;
    }

  public:
    bool hasStub(ICStub::Kind kind) {
        ICStub* stub = firstMonitorStub_;
        do {
            if (stub->kind() == kind)
                return true;

            stub = stub->next();
        } while (stub);

        return false;
    }

    inline ICFallbackStub* mainFallbackStub() const {
        MOZ_ASSERT(hasFallbackStub_);
        return mainFallbackStub_;
    }

    inline ICEntry* icEntry() const {
        return hasFallbackStub_ ? mainFallbackStub()->icEntry() : icEntry_;
    }

    inline ICStub* firstMonitorStub() const {
        return firstMonitorStub_;
    }

    static inline size_t offsetOfFirstMonitorStub() {
        return offsetof(ICTypeMonitor_Fallback, firstMonitorStub_);
    }

    inline uint32_t numOptimizedMonitorStubs() const {
        return numOptimizedMonitorStubs_;
    }

    inline bool monitorsThis() const {
        return argumentIndex_ == 0;
    }

    inline bool monitorsArgument(uint32_t* pargument) const {
        if (argumentIndex_ > 0 && argumentIndex_ < BYTECODE_INDEX) {
            *pargument = argumentIndex_ - 1;
            return true;
        }
        return false;
    }

    inline bool monitorsBytecode() const {
        return argumentIndex_ == BYTECODE_INDEX;
    }

    // Fixup the IC entry as for a normal fallback stub, for this/arguments.
    void fixupICEntry(ICEntry* icEntry) {
        MOZ_ASSERT(!hasFallbackStub_);
        MOZ_ASSERT(icEntry_ == nullptr);
        MOZ_ASSERT(lastMonitorStubPtrAddr_ == nullptr);
        icEntry_ = icEntry;
        lastMonitorStubPtrAddr_ = icEntry_->addressOfFirstStub();
    }

    // Create a new monitor stub for the type of the given value, and
    // add it to this chain.
    bool addMonitorStubForValue(JSContext* cx, JSScript* script, HandleValue val);

    void resetMonitorStubChain(Zone* zone);

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
        ICMonitoredFallbackStub* mainFallbackStub_;
        uint32_t argumentIndex_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ICMonitoredFallbackStub* mainFallbackStub)
          : ICStubCompiler(cx, ICStub::TypeMonitor_Fallback),
            mainFallbackStub_(mainFallbackStub),
            argumentIndex_(BYTECODE_INDEX)
        { }

        Compiler(JSContext* cx, uint32_t argumentIndex)
          : ICStubCompiler(cx, ICStub::TypeMonitor_Fallback),
            mainFallbackStub_(nullptr),
            argumentIndex_(argumentIndex)
        { }

        ICTypeMonitor_Fallback* getStub(ICStubSpace* space) {
            return ICStub::New<ICTypeMonitor_Fallback>(space, getStubCode(), mainFallbackStub_,
                                                       argumentIndex_);
        }
    };
};

class ICTypeMonitor_PrimitiveSet : public TypeCheckPrimitiveSetStub
{
    friend class ICStubSpace;

    ICTypeMonitor_PrimitiveSet(JitCode* stubCode, uint16_t flags)
        : TypeCheckPrimitiveSetStub(TypeMonitor_PrimitiveSet, stubCode, flags)
    {}

  public:
    class Compiler : public TypeCheckPrimitiveSetStub::Compiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ICTypeMonitor_PrimitiveSet* existingStub, JSValueType type)
          : TypeCheckPrimitiveSetStub::Compiler(cx, TypeMonitor_PrimitiveSet, existingStub, type)
        {}

        ICTypeMonitor_PrimitiveSet* updateStub() {
            TypeCheckPrimitiveSetStub* stub =
                this->TypeCheckPrimitiveSetStub::Compiler::updateStub();
            if (!stub)
                return nullptr;
            return stub->toMonitorStub();
        }

        ICTypeMonitor_PrimitiveSet* getStub(ICStubSpace* space) {
            MOZ_ASSERT(!existingStub_);
            return ICStub::New<ICTypeMonitor_PrimitiveSet>(space, getStubCode(), flags_);
        }
    };
};

class ICTypeMonitor_SingleObject : public ICStub
{
    friend class ICStubSpace;

    HeapPtrObject obj_;

    ICTypeMonitor_SingleObject(JitCode* stubCode, JSObject* obj);

  public:
    HeapPtrObject& object() {
        return obj_;
    }

    static size_t offsetOfObject() {
        return offsetof(ICTypeMonitor_SingleObject, obj_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        HandleObject obj_;
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, HandleObject obj)
          : ICStubCompiler(cx, TypeMonitor_SingleObject),
            obj_(obj)
        { }

        ICTypeMonitor_SingleObject* getStub(ICStubSpace* space) {
            return ICStub::New<ICTypeMonitor_SingleObject>(space, getStubCode(), obj_);
        }
    };
};

class ICTypeMonitor_ObjectGroup : public ICStub
{
    friend class ICStubSpace;

    HeapPtrObjectGroup group_;

    ICTypeMonitor_ObjectGroup(JitCode* stubCode, ObjectGroup* group);

  public:
    HeapPtrObjectGroup& group() {
        return group_;
    }

    static size_t offsetOfGroup() {
        return offsetof(ICTypeMonitor_ObjectGroup, group_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        HandleObjectGroup group_;
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, HandleObjectGroup group)
          : ICStubCompiler(cx, TypeMonitor_ObjectGroup),
            group_(group)
        { }

        ICTypeMonitor_ObjectGroup* getStub(ICStubSpace* space) {
            return ICStub::New<ICTypeMonitor_ObjectGroup>(space, getStubCode(), group_);
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
          : ICStubCompiler(cx, ICStub::TypeUpdate_Fallback)
        { }

        ICTypeUpdate_Fallback* getStub(ICStubSpace* space) {
            return ICStub::New<ICTypeUpdate_Fallback>(space, getStubCode());
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
          : TypeCheckPrimitiveSetStub::Compiler(cx, TypeUpdate_PrimitiveSet, existingStub, type)
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
            return ICStub::New<ICTypeUpdate_PrimitiveSet>(space, getStubCode(), flags_);
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
          : ICStubCompiler(cx, TypeUpdate_SingleObject),
            obj_(obj)
        { }

        ICTypeUpdate_SingleObject* getStub(ICStubSpace* space) {
            return ICStub::New<ICTypeUpdate_SingleObject>(space, getStubCode(), obj_);
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
          : ICStubCompiler(cx, TypeUpdate_ObjectGroup),
            group_(group)
        { }

        ICTypeUpdate_ObjectGroup* getStub(ICStubSpace* space) {
            return ICStub::New<ICTypeUpdate_ObjectGroup>(space, getStubCode(), group_);
        }
    };
};

// This
//      JSOP_THIS

class ICThis_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICThis_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::This_Fallback, stubCode) {}

  public:
    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::This_Fallback) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICThis_Fallback>(space, getStubCode());
        }
    };
};

class ICNewArray_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    HeapPtrArrayObject templateObject_;

    ICNewArray_Fallback(JitCode* stubCode, ArrayObject* templateObject)
      : ICFallbackStub(ICStub::NewArray_Fallback, stubCode), templateObject_(templateObject)
    {}

  public:
    class Compiler : public ICStubCompiler {
        RootedArrayObject templateObject;
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ArrayObject* templateObject)
          : ICStubCompiler(cx, ICStub::NewArray_Fallback),
            templateObject(cx, templateObject)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICNewArray_Fallback>(space, getStubCode(), templateObject);
        }
    };

    HeapPtrArrayObject& templateObject() {
        return templateObject_;
    }
};

class ICNewObject_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    HeapPtrPlainObject templateObject_;

    ICNewObject_Fallback(JitCode* stubCode, PlainObject* templateObject)
      : ICFallbackStub(ICStub::NewObject_Fallback, stubCode), templateObject_(templateObject)
    {}

  public:
    class Compiler : public ICStubCompiler {
        RootedPlainObject templateObject;
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, PlainObject* templateObject)
          : ICStubCompiler(cx, ICStub::NewObject_Fallback),
            templateObject(cx, templateObject)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICNewObject_Fallback>(space, getStubCode(), templateObject);
        }
    };

    HeapPtrPlainObject& templateObject() {
        return templateObject_;
    }
};

// Compare
//      JSOP_LT
//      JSOP_GT

class ICCompare_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICCompare_Fallback(JitCode* stubCode)
      : ICFallbackStub(ICStub::Compare_Fallback, stubCode) {}

  public:
    static const uint32_t MAX_OPTIMIZED_STUBS = 8;

    static const size_t UNOPTIMIZABLE_ACCESS_BIT = 0;
    void noteUnoptimizableAccess() {
        extra_ |= (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }
    bool hadUnoptimizableAccess() const {
        return extra_ & (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::Compare_Fallback) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCompare_Fallback>(space, getStubCode());
        }
    };
};

class ICCompare_Int32 : public ICStub
{
    friend class ICStubSpace;

    explicit ICCompare_Int32(JitCode* stubCode)
      : ICStub(ICStub::Compare_Int32, stubCode) {}

  public:
    // Compiler for this stub kind.
    class Compiler : public ICMultiStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, JSOp op)
          : ICMultiStubCompiler(cx, ICStub::Compare_Int32, op) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCompare_Int32>(space, getStubCode());
        }
    };
};

class ICCompare_Double : public ICStub
{
    friend class ICStubSpace;

    explicit ICCompare_Double(JitCode* stubCode)
      : ICStub(ICStub::Compare_Double, stubCode)
    {}

  public:
    class Compiler : public ICMultiStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, JSOp op)
          : ICMultiStubCompiler(cx, ICStub::Compare_Double, op)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCompare_Double>(space, getStubCode());
        }
    };
};

class ICCompare_NumberWithUndefined : public ICStub
{
    friend class ICStubSpace;

    ICCompare_NumberWithUndefined(JitCode* stubCode, bool lhsIsUndefined)
      : ICStub(ICStub::Compare_NumberWithUndefined, stubCode)
    {
        extra_ = lhsIsUndefined;
    }

  public:
    bool lhsIsUndefined() {
        return extra_;
    }

    class Compiler : public ICMultiStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

        bool lhsIsUndefined;

      public:
        Compiler(JSContext* cx, JSOp op, bool lhsIsUndefined)
          : ICMultiStubCompiler(cx, ICStub::Compare_NumberWithUndefined, op),
            lhsIsUndefined(lhsIsUndefined)
        {}

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind)
                 | (static_cast<int32_t>(op) << 16)
                 | (static_cast<int32_t>(lhsIsUndefined) << 24);
        }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCompare_NumberWithUndefined>(space, getStubCode(), lhsIsUndefined);
        }
    };
};

class ICCompare_String : public ICStub
{
    friend class ICStubSpace;

    explicit ICCompare_String(JitCode* stubCode)
      : ICStub(ICStub::Compare_String, stubCode)
    {}

  public:
    class Compiler : public ICMultiStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, JSOp op)
          : ICMultiStubCompiler(cx, ICStub::Compare_String, op)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCompare_String>(space, getStubCode());
        }
    };
};

class ICCompare_Boolean : public ICStub
{
    friend class ICStubSpace;

    explicit ICCompare_Boolean(JitCode* stubCode)
      : ICStub(ICStub::Compare_Boolean, stubCode)
    {}

  public:
    class Compiler : public ICMultiStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, JSOp op)
          : ICMultiStubCompiler(cx, ICStub::Compare_Boolean, op)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCompare_Boolean>(space, getStubCode());
        }
    };
};

class ICCompare_Object : public ICStub
{
    friend class ICStubSpace;

    explicit ICCompare_Object(JitCode* stubCode)
      : ICStub(ICStub::Compare_Object, stubCode)
    {}

  public:
    class Compiler : public ICMultiStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, JSOp op)
          : ICMultiStubCompiler(cx, ICStub::Compare_Object, op)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCompare_Object>(space, getStubCode());
        }
    };
};

class ICCompare_ObjectWithUndefined : public ICStub
{
    friend class ICStubSpace;

    explicit ICCompare_ObjectWithUndefined(JitCode* stubCode)
      : ICStub(ICStub::Compare_ObjectWithUndefined, stubCode)
    {}

  public:
    class Compiler : public ICMultiStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

        bool lhsIsUndefined;
        bool compareWithNull;

      public:
        Compiler(JSContext* cx, JSOp op, bool lhsIsUndefined, bool compareWithNull)
          : ICMultiStubCompiler(cx, ICStub::Compare_ObjectWithUndefined, op),
            lhsIsUndefined(lhsIsUndefined),
            compareWithNull(compareWithNull)
        {}

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind)
                 | (static_cast<int32_t>(op) << 16)
                 | (static_cast<int32_t>(lhsIsUndefined) << 24)
                 | (static_cast<int32_t>(compareWithNull) << 25);
        }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCompare_ObjectWithUndefined>(space, getStubCode());
        }
    };
};

class ICCompare_Int32WithBoolean : public ICStub
{
    friend class ICStubSpace;

    ICCompare_Int32WithBoolean(JitCode* stubCode, bool lhsIsInt32)
      : ICStub(ICStub::Compare_Int32WithBoolean, stubCode)
    {
        extra_ = lhsIsInt32;
    }

  public:
    bool lhsIsInt32() const {
        return extra_;
    }

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        JSOp op_;
        bool lhsIsInt32_;
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(op_) << 16) |
                   (static_cast<int32_t>(lhsIsInt32_) << 24);
        }

      public:
        Compiler(JSContext* cx, JSOp op, bool lhsIsInt32)
          : ICStubCompiler(cx, ICStub::Compare_Int32WithBoolean),
            op_(op),
            lhsIsInt32_(lhsIsInt32)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCompare_Int32WithBoolean>(space, getStubCode(), lhsIsInt32_);
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
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::ToBool_Fallback) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICToBool_Fallback>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::ToBool_Int32) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICToBool_Int32>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::ToBool_String) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICToBool_String>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::ToBool_NullUndefined) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICToBool_NullUndefined>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::ToBool_Double) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICToBool_Double>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::ToBool_Object) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICToBool_Object>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::ToNumber_Fallback) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICToNumber_Fallback>(space, getStubCode());
        }
    };
};

// BinaryArith
//      JSOP_ADD
//      JSOP_BITAND, JSOP_BITXOR, JSOP_BITOR
//      JSOP_LSH, JSOP_RSH, JSOP_URSH

class ICBinaryArith_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICBinaryArith_Fallback(JitCode* stubCode)
      : ICFallbackStub(BinaryArith_Fallback, stubCode)
    {
        extra_ = 0;
    }

    static const uint16_t SAW_DOUBLE_RESULT_BIT = 0x1;
    static const uint16_t UNOPTIMIZABLE_OPERANDS_BIT = 0x2;

  public:
    static const uint32_t MAX_OPTIMIZED_STUBS = 8;

    bool sawDoubleResult() const {
        return extra_ & SAW_DOUBLE_RESULT_BIT;
    }
    void setSawDoubleResult() {
        extra_ |= SAW_DOUBLE_RESULT_BIT;
    }
    bool hadUnoptimizableOperands() const {
        return extra_ & UNOPTIMIZABLE_OPERANDS_BIT;
    }
    void noteUnoptimizableOperands() {
        extra_ |= UNOPTIMIZABLE_OPERANDS_BIT;
    }

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::BinaryArith_Fallback) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICBinaryArith_Fallback>(space, getStubCode());
        }
    };
};

class ICBinaryArith_Int32 : public ICStub
{
    friend class ICStubSpace;

    ICBinaryArith_Int32(JitCode* stubCode, bool allowDouble)
      : ICStub(BinaryArith_Int32, stubCode)
    {
        extra_ = allowDouble;
    }

  public:
    bool allowDouble() const {
        return extra_;
    }

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        JSOp op_;
        bool allowDouble_;

        bool generateStubCode(MacroAssembler& masm);

        // Stub keys shift-stubs need to encode the kind, the JSOp and if we allow doubles.
        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(op_) << 16) |
                   (static_cast<int32_t>(allowDouble_) << 24);
        }

      public:
        Compiler(JSContext* cx, JSOp op, bool allowDouble)
          : ICStubCompiler(cx, ICStub::BinaryArith_Int32),
            op_(op), allowDouble_(allowDouble) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICBinaryArith_Int32>(space, getStubCode(), allowDouble_);
        }
    };
};

class ICBinaryArith_StringConcat : public ICStub
{
    friend class ICStubSpace;

    explicit ICBinaryArith_StringConcat(JitCode* stubCode)
      : ICStub(BinaryArith_StringConcat, stubCode)
    {}

  public:
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::BinaryArith_StringConcat)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICBinaryArith_StringConcat>(space, getStubCode());
        }
    };
};

class ICBinaryArith_StringObjectConcat : public ICStub
{
    friend class ICStubSpace;

    ICBinaryArith_StringObjectConcat(JitCode* stubCode, bool lhsIsString)
      : ICStub(BinaryArith_StringObjectConcat, stubCode)
    {
        extra_ = lhsIsString;
    }

  public:
    bool lhsIsString() const {
        return extra_;
    }

    class Compiler : public ICStubCompiler {
      protected:
        bool lhsIsString_;
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(lhsIsString_) << 16);
        }

      public:
        Compiler(JSContext* cx, bool lhsIsString)
          : ICStubCompiler(cx, ICStub::BinaryArith_StringObjectConcat),
            lhsIsString_(lhsIsString)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICBinaryArith_StringObjectConcat>(space, getStubCode(), lhsIsString_);
        }
    };
};

class ICBinaryArith_Double : public ICStub
{
    friend class ICStubSpace;

    explicit ICBinaryArith_Double(JitCode* stubCode)
      : ICStub(BinaryArith_Double, stubCode)
    {}

  public:
    class Compiler : public ICMultiStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, JSOp op)
          : ICMultiStubCompiler(cx, ICStub::BinaryArith_Double, op)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICBinaryArith_Double>(space, getStubCode());
        }
    };
};

class ICBinaryArith_BooleanWithInt32 : public ICStub
{
    friend class ICStubSpace;

    ICBinaryArith_BooleanWithInt32(JitCode* stubCode, bool lhsIsBool, bool rhsIsBool)
      : ICStub(BinaryArith_BooleanWithInt32, stubCode)
    {
        MOZ_ASSERT(lhsIsBool || rhsIsBool);
        extra_ = 0;
        if (lhsIsBool)
            extra_ |= 1;
        if (rhsIsBool)
            extra_ |= 2;
    }

  public:
    bool lhsIsBoolean() const {
        return extra_ & 1;
    }

    bool rhsIsBoolean() const {
        return extra_ & 2;
    }

    class Compiler : public ICStubCompiler {
      protected:
        JSOp op_;
        bool lhsIsBool_;
        bool rhsIsBool_;
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(op_) << 16) |
                   (static_cast<int32_t>(lhsIsBool_) << 24) |
                   (static_cast<int32_t>(rhsIsBool_) << 25);
        }

      public:
        Compiler(JSContext* cx, JSOp op, bool lhsIsBool, bool rhsIsBool)
          : ICStubCompiler(cx, ICStub::BinaryArith_BooleanWithInt32),
            op_(op), lhsIsBool_(lhsIsBool), rhsIsBool_(rhsIsBool)
        {
            MOZ_ASSERT(op_ == JSOP_ADD || op_ == JSOP_SUB || op_ == JSOP_BITOR ||
                       op_ == JSOP_BITAND || op_ == JSOP_BITXOR);
            MOZ_ASSERT(lhsIsBool_ || rhsIsBool_);
        }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICBinaryArith_BooleanWithInt32>(space, getStubCode(),
                                                               lhsIsBool_, rhsIsBool_);
        }
    };
};

class ICBinaryArith_DoubleWithInt32 : public ICStub
{
    friend class ICStubSpace;

    ICBinaryArith_DoubleWithInt32(JitCode* stubCode, bool lhsIsDouble)
      : ICStub(BinaryArith_DoubleWithInt32, stubCode)
    {
        extra_ = lhsIsDouble;
    }

  public:
    bool lhsIsDouble() const {
        return extra_;
    }

    class Compiler : public ICMultiStubCompiler {
      protected:
        bool lhsIsDouble_;
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(op) << 16) |
                   (static_cast<int32_t>(lhsIsDouble_) << 24);
        }

      public:
        Compiler(JSContext* cx, JSOp op, bool lhsIsDouble)
          : ICMultiStubCompiler(cx, ICStub::BinaryArith_DoubleWithInt32, op),
            lhsIsDouble_(lhsIsDouble)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICBinaryArith_DoubleWithInt32>(space, getStubCode(), lhsIsDouble_);
        }
    };
};

// UnaryArith
//     JSOP_BITNOT
//     JSOP_NEG

class ICUnaryArith_Fallback : public ICFallbackStub
{
    friend class ICStubSpace;

    explicit ICUnaryArith_Fallback(JitCode* stubCode)
      : ICFallbackStub(UnaryArith_Fallback, stubCode)
    {
        extra_ = 0;
    }

  public:
    static const uint32_t MAX_OPTIMIZED_STUBS = 8;

    bool sawDoubleResult() {
        return extra_;
    }
    void setSawDoubleResult() {
        extra_ = 1;
    }

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::UnaryArith_Fallback)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICUnaryArith_Fallback>(space, getStubCode());
        }
    };
};

class ICUnaryArith_Int32 : public ICStub
{
    friend class ICStubSpace;

    explicit ICUnaryArith_Int32(JitCode* stubCode)
      : ICStub(UnaryArith_Int32, stubCode)
    {}

  public:
    class Compiler : public ICMultiStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, JSOp op)
          : ICMultiStubCompiler(cx, ICStub::UnaryArith_Int32, op)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICUnaryArith_Int32>(space, getStubCode());
        }
    };
};

class ICUnaryArith_Double : public ICStub
{
    friend class ICStubSpace;

    explicit ICUnaryArith_Double(JitCode* stubCode)
      : ICStub(UnaryArith_Double, stubCode)
    {}

  public:
    class Compiler : public ICMultiStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, JSOp op)
          : ICMultiStubCompiler(cx, ICStub::UnaryArith_Double, op)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICUnaryArith_Double>(space, getStubCode());
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

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::GetElem_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            ICGetElem_Fallback* stub = ICStub::New<ICGetElem_Fallback>(space, getStubCode());
            if (!stub)
                return nullptr;
            if (!stub->initMonitoringChain(cx, space))
                return nullptr;
            return stub;
        }
    };
};

class ICGetElemNativeStub : public ICMonitoredStub
{
  public:
    enum AccessType { FixedSlot = 0, DynamicSlot, NativeGetter, ScriptedGetter };

  protected:
    HeapPtrShape shape_;
    HeapPtrPropertyName name_;

    static const unsigned NEEDS_ATOMIZE_SHIFT = 0;
    static const uint16_t NEEDS_ATOMIZE_MASK = 0x1;

    static const unsigned ACCESSTYPE_SHIFT = 1;
    static const uint16_t ACCESSTYPE_MASK = 0x3;

    ICGetElemNativeStub(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                        Shape* shape, PropertyName* name, AccessType acctype,
                        bool needsAtomize);

    ~ICGetElemNativeStub();

  public:
    HeapPtrShape& shape() {
        return shape_;
    }
    static size_t offsetOfShape() {
        return offsetof(ICGetElemNativeStub, shape_);
    }

    HeapPtrPropertyName& name() {
        return name_;
    }
    static size_t offsetOfName() {
        return offsetof(ICGetElemNativeStub, name_);
    }

    AccessType accessType() const {
        return static_cast<AccessType>((extra_ >> ACCESSTYPE_SHIFT) & ACCESSTYPE_MASK);
    }

    bool needsAtomize() const {
        return (extra_ >> NEEDS_ATOMIZE_SHIFT) & NEEDS_ATOMIZE_MASK;
    }
};

class ICGetElemNativeSlotStub : public ICGetElemNativeStub
{
  protected:
    uint32_t offset_;

    ICGetElemNativeSlotStub(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                            Shape* shape, PropertyName* name,
                            AccessType acctype, bool needsAtomize, uint32_t offset)
      : ICGetElemNativeStub(kind, stubCode, firstMonitorStub, shape, name, acctype, needsAtomize),
        offset_(offset)
    {
        MOZ_ASSERT(kind == GetElem_NativeSlot || kind == GetElem_NativePrototypeSlot);
        MOZ_ASSERT(acctype == FixedSlot || acctype == DynamicSlot);
    }

  public:
    uint32_t offset() const {
        return offset_;
    }

    static size_t offsetOfOffset() {
        return offsetof(ICGetElemNativeSlotStub, offset_);
    }
};

class ICGetElemNativeGetterStub : public ICGetElemNativeStub
{
  protected:
    HeapPtrFunction getter_;
    uint32_t pcOffset_;

    ICGetElemNativeGetterStub(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                              Shape* shape, PropertyName* name, AccessType acctype,
                              bool needsAtomize, JSFunction* getter, uint32_t pcOffset);

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

class ICGetElem_NativeSlot : public ICGetElemNativeSlotStub
{
    friend class ICStubSpace;
    ICGetElem_NativeSlot(JitCode* stubCode, ICStub* firstMonitorStub,
                         Shape* shape, PropertyName* name,
                         AccessType acctype, bool needsAtomize, uint32_t offset)
      : ICGetElemNativeSlotStub(ICStub::GetElem_NativeSlot, stubCode, firstMonitorStub, shape,
                                name, acctype, needsAtomize, offset)
    {}
};

class ICGetElem_NativePrototypeSlot : public ICGetElemNativeSlotStub
{
    friend class ICStubSpace;
    HeapPtrObject holder_;
    HeapPtrShape holderShape_;

    ICGetElem_NativePrototypeSlot(JitCode* stubCode, ICStub* firstMonitorStub,
                                  Shape* shape, PropertyName* name,
                                  AccessType acctype, bool needsAtomize, uint32_t offset,
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

class ICGetElemNativePrototypeCallStub : public ICGetElemNativeGetterStub
{
    friend class ICStubSpace;
    HeapPtrObject holder_;
    HeapPtrShape holderShape_;

  protected:
    ICGetElemNativePrototypeCallStub(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                                     Shape* shape, PropertyName* name,
                                     AccessType acctype, bool needsAtomize, JSFunction* getter,
                                     uint32_t pcOffset, JSObject* holder,
                                     Shape* holderShape);

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

class ICGetElem_NativePrototypeCallNative : public ICGetElemNativePrototypeCallStub
{
    friend class ICStubSpace;

    ICGetElem_NativePrototypeCallNative(JitCode* stubCode, ICStub* firstMonitorStub,
                                        Shape* shape, PropertyName* name,
                                        AccessType acctype, bool needsAtomize,
                                        JSFunction* getter, uint32_t pcOffset,
                                        JSObject* holder, Shape* holderShape)
      : ICGetElemNativePrototypeCallStub(GetElem_NativePrototypeCallNative,
                                         stubCode, firstMonitorStub, shape, name,
                                         acctype, needsAtomize, getter, pcOffset, holder,
                                         holderShape)
    {}

  public:
    static ICGetElem_NativePrototypeCallNative* Clone(ICStubSpace* space,
                                                      ICStub* firstMonitorStub,
                                                      ICGetElem_NativePrototypeCallNative& other);
};

class ICGetElem_NativePrototypeCallScripted : public ICGetElemNativePrototypeCallStub
{
    friend class ICStubSpace;

    ICGetElem_NativePrototypeCallScripted(JitCode* stubCode, ICStub* firstMonitorStub,
                                          Shape* shape, PropertyName* name,
                                          AccessType acctype, bool needsAtomize,
                                          JSFunction* getter, uint32_t pcOffset,
                                          JSObject* holder, Shape* holderShape)
      : ICGetElemNativePrototypeCallStub(GetElem_NativePrototypeCallScripted,
                                         stubCode, firstMonitorStub, shape, name,
                                         acctype, needsAtomize, getter, pcOffset, holder,
                                         holderShape)
    {}

  public:
    static ICGetElem_NativePrototypeCallScripted*
    Clone(ICStubSpace* space,
          ICStub* firstMonitorStub,
          ICGetElem_NativePrototypeCallScripted& other);
};

// Compiler for GetElem_NativeSlot and GetElem_NativePrototypeSlot stubs.
class ICGetElemNativeCompiler : public ICStubCompiler
{
    bool isCallElem_;
    ICStub* firstMonitorStub_;
    HandleObject obj_;
    HandleObject holder_;
    HandlePropertyName name_;
    ICGetElemNativeStub::AccessType acctype_;
    bool needsAtomize_;
    uint32_t offset_;
    HandleFunction getter_;
    uint32_t pcOffset_;

    bool emitCallNative(MacroAssembler& masm, Register objReg);
    bool emitCallScripted(MacroAssembler& masm, Register objReg);
    bool generateStubCode(MacroAssembler& masm);

  protected:
    virtual int32_t getKey() const {
#if JS_HAS_NO_SUCH_METHOD
        return static_cast<int32_t>(kind) |
               (static_cast<int32_t>(isCallElem_) << 16) |
               (static_cast<int32_t>(needsAtomize_) << 17) |
               (static_cast<int32_t>(acctype_) << 18);
#else
        return static_cast<int32_t>(kind) | (static_cast<int32_t>(needsAtomize_) << 16) |
               (static_cast<int32_t>(acctype_) << 17);
#endif
    }

  public:
    ICGetElemNativeCompiler(JSContext* cx, ICStub::Kind kind, bool isCallElem,
                            ICStub* firstMonitorStub, HandleObject obj, HandleObject holder,
                            HandlePropertyName name, ICGetElemNativeStub::AccessType acctype,
                            bool needsAtomize, uint32_t offset)
      : ICStubCompiler(cx, kind),
        isCallElem_(isCallElem),
        firstMonitorStub_(firstMonitorStub),
        obj_(obj),
        holder_(holder),
        name_(name),
        acctype_(acctype),
        needsAtomize_(needsAtomize),
        offset_(offset),
        getter_(js::NullPtr()),
        pcOffset_(0)
    {}

    ICGetElemNativeCompiler(JSContext* cx, ICStub::Kind kind, ICStub* firstMonitorStub,
                            HandleObject obj, HandleObject holder, HandlePropertyName name,
                            ICGetElemNativeStub::AccessType acctype, bool needsAtomize,
                            HandleFunction getter, uint32_t pcOffset, bool isCallElem)
      : ICStubCompiler(cx, kind),
        isCallElem_(false),
        firstMonitorStub_(firstMonitorStub),
        obj_(obj),
        holder_(holder),
        name_(name),
        acctype_(acctype),
        needsAtomize_(needsAtomize),
        offset_(0),
        getter_(getter),
        pcOffset_(pcOffset)
    {}

    ICStub* getStub(ICStubSpace* space) {
        RootedShape shape(cx, obj_->lastProperty());
        if (kind == ICStub::GetElem_NativeSlot) {
            MOZ_ASSERT(obj_ == holder_);
            return ICStub::New<ICGetElem_NativeSlot>(
                    space, getStubCode(), firstMonitorStub_, shape, name_, acctype_, needsAtomize_,
                    offset_);
        }

        MOZ_ASSERT(obj_ != holder_);
        RootedShape holderShape(cx, holder_->lastProperty());
        if (kind == ICStub::GetElem_NativePrototypeSlot) {
            return ICStub::New<ICGetElem_NativePrototypeSlot>(
                    space, getStubCode(), firstMonitorStub_, shape, name_, acctype_, needsAtomize_,
                    offset_, holder_, holderShape);
        }

        if (kind == ICStub::GetElem_NativePrototypeCallNative) {
            return ICStub::New<ICGetElem_NativePrototypeCallNative>(
                    space, getStubCode(), firstMonitorStub_, shape, name_, acctype_, needsAtomize_,
                    getter_, pcOffset_, holder_, holderShape);
        }

        MOZ_ASSERT(kind == ICStub::GetElem_NativePrototypeCallScripted);
        if (kind == ICStub::GetElem_NativePrototypeCallScripted) {
            return ICStub::New<ICGetElem_NativePrototypeCallScripted>(
                    space, getStubCode(), firstMonitorStub_, shape, name_, acctype_, needsAtomize_,
                    getter_, pcOffset_, holder_, holderShape);
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
          : ICStubCompiler(cx, ICStub::GetElem_String) {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetElem_String>(space, getStubCode());
        }
    };
};

class ICGetElem_Dense : public ICMonitoredStub
{
    friend class ICStubSpace;

    HeapPtrShape shape_;

    ICGetElem_Dense(JitCode* stubCode, ICStub* firstMonitorStub, Shape* shape);

  public:
    static ICGetElem_Dense* Clone(ICStubSpace* space, ICStub* firstMonitorStub,
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
      bool isCallElem_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
#if JS_HAS_NO_SUCH_METHOD
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(isCallElem_) << 16);
#else
            return static_cast<int32_t>(kind);
#endif
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, Shape* shape, bool isCallElem)
          : ICStubCompiler(cx, ICStub::GetElem_Dense),
            firstMonitorStub_(firstMonitorStub),
            shape_(cx, shape),
            isCallElem_(isCallElem)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetElem_Dense>(space, getStubCode(), firstMonitorStub_, shape_);
        }
    };
};

// Enum for stubs handling a combination of typed arrays and typed objects.
enum TypedThingLayout {
    Layout_TypedArray,
    Layout_OutlineTypedObject,
    Layout_InlineTypedObject
};

static inline TypedThingLayout
GetTypedThingLayout(const Class* clasp)
{
    if (IsAnyTypedArrayClass(clasp))
        return Layout_TypedArray;
    if (IsOutlineTypedObjectClass(clasp))
        return Layout_OutlineTypedObject;
    if (IsInlineTypedObjectClass(clasp))
        return Layout_InlineTypedObject;
    MOZ_CRASH("Bad object class");
}

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
            return static_cast<int32_t>(kind) |
                   (static_cast<int32_t>(type_) << 16) |
                   (static_cast<int32_t>(layout_) << 24);
        }

      public:
        Compiler(JSContext* cx, Shape* shape, Scalar::Type type)
          : ICStubCompiler(cx, ICStub::GetElem_TypedArray),
            shape_(cx, shape),
            type_(type),
            layout_(GetTypedThingLayout(shape->getObjectClass()))
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetElem_TypedArray>(space, getStubCode(), shape_, type_);
        }
    };
};

class ICGetElem_Arguments : public ICMonitoredStub
{
    friend class ICStubSpace;
  public:
    enum Which { Normal, Strict, Magic };

  private:
    ICGetElem_Arguments(JitCode* stubCode, ICStub* firstMonitorStub, Which which)
      : ICMonitoredStub(ICStub::GetElem_Arguments, stubCode, firstMonitorStub)
    {
        extra_ = static_cast<uint16_t>(which);
    }

  public:
    static ICGetElem_Arguments* Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                                      ICGetElem_Arguments& other);

    Which which() const {
        return static_cast<Which>(extra_);
    }

    class Compiler : public ICStubCompiler {
      ICStub* firstMonitorStub_;
      Which which_;
      bool isCallElem_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
#if JS_HAS_NO_SUCH_METHOD
            return static_cast<int32_t>(kind) |
                   static_cast<int32_t>(isCallElem_ << 16) |
                   (static_cast<int32_t>(which_) << 17);
#else
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(which_) << 16);
#endif
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, Which which, bool isCallElem)
          : ICStubCompiler(cx, ICStub::GetElem_Arguments),
            firstMonitorStub_(firstMonitorStub),
            which_(which),
            isCallElem_(isCallElem)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetElem_Arguments>(space, getStubCode(), firstMonitorStub_, which_);
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
          : ICStubCompiler(cx, ICStub::SetElem_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICSetElem_Fallback>(space, getStubCode());
        }
    };
};

class ICSetElem_Dense : public ICUpdatedStub
{
    friend class ICStubSpace;

    HeapPtrShape shape_;
    HeapPtrObjectGroup group_;

    ICSetElem_Dense(JitCode* stubCode, Shape* shape, ObjectGroup* group);

  public:
    static size_t offsetOfShape() {
        return offsetof(ICSetElem_Dense, shape_);
    }
    static size_t offsetOfGroup() {
        return offsetof(ICSetElem_Dense, group_);
    }

    HeapPtrShape& shape() {
        return shape_;
    }
    HeapPtrObjectGroup& group() {
        return group_;
    }

    class Compiler : public ICStubCompiler {
        RootedShape shape_;

        // Compiler is only live on stack during compilation, it should
        // outlive any RootedObjectGroup it's passed.  So it can just
        // use the handle.
        HandleObjectGroup group_;

        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, Shape* shape, HandleObjectGroup group)
          : ICStubCompiler(cx, ICStub::SetElem_Dense),
            shape_(cx, shape),
            group_(group)
        {}

        ICUpdatedStub* getStub(ICStubSpace* space) {
            ICSetElem_Dense* stub = ICStub::New<ICSetElem_Dense>(space, getStubCode(), shape_, group_);
            if (!stub || !stub->initUpdatingChain(cx, space))
                return nullptr;
            return stub;
        }
    };
};

template <size_t ProtoChainDepth> class ICSetElem_DenseAddImpl;

class ICSetElem_DenseAdd : public ICUpdatedStub
{
    friend class ICStubSpace;

  public:
    static const size_t MAX_PROTO_CHAIN_DEPTH = 4;

  protected:
    HeapPtrObjectGroup group_;

    ICSetElem_DenseAdd(JitCode* stubCode, ObjectGroup* group, size_t protoChainDepth);

  public:
    static size_t offsetOfGroup() {
        return offsetof(ICSetElem_DenseAdd, group_);
    }

    HeapPtrObjectGroup& group() {
        return group_;
    }
    size_t protoChainDepth() const {
        MOZ_ASSERT(extra_ <= MAX_PROTO_CHAIN_DEPTH);
        return extra_;
    }

    template <size_t ProtoChainDepth>
    ICSetElem_DenseAddImpl<ProtoChainDepth>* toImplUnchecked() {
        return static_cast<ICSetElem_DenseAddImpl<ProtoChainDepth>*>(this);
    }

    template <size_t ProtoChainDepth>
    ICSetElem_DenseAddImpl<ProtoChainDepth>* toImpl() {
        MOZ_ASSERT(ProtoChainDepth == protoChainDepth());
        return toImplUnchecked<ProtoChainDepth>();
    }
};

template <size_t ProtoChainDepth>
class ICSetElem_DenseAddImpl : public ICSetElem_DenseAdd
{
    friend class ICStubSpace;

    static const size_t NumShapes = ProtoChainDepth + 1;
    mozilla::Array<HeapPtrShape, NumShapes> shapes_;

    ICSetElem_DenseAddImpl(JitCode* stubCode, ObjectGroup* group,
                           const AutoShapeVector* shapes)
      : ICSetElem_DenseAdd(stubCode, group, ProtoChainDepth)
    {
        MOZ_ASSERT(shapes->length() == NumShapes);
        for (size_t i = 0; i < NumShapes; i++)
            shapes_[i].init((*shapes)[i]);
    }

  public:
    void traceShapes(JSTracer* trc) {
        for (size_t i = 0; i < NumShapes; i++)
            MarkShape(trc, &shapes_[i], "baseline-setelem-denseadd-stub-shape");
    }
    Shape* shape(size_t i) const {
        MOZ_ASSERT(i < NumShapes);
        return shapes_[i];
    }
    static size_t offsetOfShape(size_t idx) {
        return offsetof(ICSetElem_DenseAddImpl, shapes_) + idx * sizeof(HeapPtrShape);
    }
};

class ICSetElemDenseAddCompiler : public ICStubCompiler {
    RootedObject obj_;
    size_t protoChainDepth_;

    bool generateStubCode(MacroAssembler& masm);

  protected:
    virtual int32_t getKey() const {
        return static_cast<int32_t>(kind) | (static_cast<int32_t>(protoChainDepth_) << 16);
    }

  public:
    ICSetElemDenseAddCompiler(JSContext* cx, HandleObject obj, size_t protoChainDepth)
        : ICStubCompiler(cx, ICStub::SetElem_DenseAdd),
          obj_(cx, obj),
          protoChainDepth_(protoChainDepth)
    {}

    template <size_t ProtoChainDepth>
    ICUpdatedStub* getStubSpecific(ICStubSpace* space, const AutoShapeVector* shapes);

    ICUpdatedStub* getStub(ICStubSpace* space);
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
            return static_cast<int32_t>(kind) |
                   (static_cast<int32_t>(type_) << 16) |
                   (static_cast<int32_t>(layout_) << 24) |
                   (static_cast<int32_t>(expectOutOfBounds_) << 28);
        }

      public:
        Compiler(JSContext* cx, Shape* shape, Scalar::Type type, bool expectOutOfBounds)
          : ICStubCompiler(cx, ICStub::SetElem_TypedArray),
            shape_(cx, shape),
            type_(type),
            layout_(GetTypedThingLayout(shape->getObjectClass())),
            expectOutOfBounds_(expectOutOfBounds)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICSetElem_TypedArray>(space, getStubCode(), shape_, type_,
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
    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::In_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICIn_Fallback>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::GetName_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            ICGetName_Fallback* stub = ICStub::New<ICGetName_Fallback>(space, getStubCode());
            if (!stub || !stub->initMonitoringChain(cx, space))
                return nullptr;
            return stub;
        }
    };
};

// Optimized GETGNAME/CALLGNAME stub.
class ICGetName_Global : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected: // Protected to silence Clang warning.
    HeapPtrShape shape_;
    uint32_t slot_;

    ICGetName_Global(JitCode* stubCode, ICStub* firstMonitorStub, Shape* shape, uint32_t slot);

  public:
    HeapPtrShape& shape() {
        return shape_;
    }
    static size_t offsetOfShape() {
        return offsetof(ICGetName_Global, shape_);
    }
    static size_t offsetOfSlot() {
        return offsetof(ICGetName_Global, slot_);
    }

    class Compiler : public ICStubCompiler {
        ICStub* firstMonitorStub_;
        RootedShape shape_;
        uint32_t slot_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, Shape* shape, uint32_t slot)
          : ICStubCompiler(cx, ICStub::GetName_Global),
            firstMonitorStub_(firstMonitorStub),
            shape_(cx, shape),
            slot_(slot)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetName_Global>(space, getStubCode(), firstMonitorStub_, shape_,
                                                 slot_);
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
                    AutoShapeVector* shapes, uint32_t offset);

    static Kind GetStubKind() {
        return (Kind) (GetName_Scope0 + NumHops);
    }

  public:
    void traceScopes(JSTracer* trc) {
        for (size_t i = 0; i < NumHops + 1; i++)
            MarkShape(trc, &shapes_[i], "baseline-scope-stub-shape");
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
        AutoShapeVector* shapes_;
        bool isFixedSlot_;
        uint32_t offset_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

      protected:
        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(isFixedSlot_) << 16);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub,
                 AutoShapeVector* shapes, bool isFixedSlot, uint32_t offset)
          : ICStubCompiler(cx, GetStubKind()),
            firstMonitorStub_(firstMonitorStub),
            shapes_(shapes),
            isFixedSlot_(isFixedSlot),
            offset_(offset)
        {
        }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetName_Scope>(space, getStubCode(), firstMonitorStub_, shapes_, offset_);
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
          : ICStubCompiler(cx, ICStub::BindName_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICBindName_Fallback>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::GetIntrinsic_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            ICGetIntrinsic_Fallback* stub =
                ICStub::New<ICGetIntrinsic_Fallback>(space, getStubCode());
            if (!stub || !stub->initMonitoringChain(cx, space))
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
          : ICStubCompiler(cx, ICStub::GetIntrinsic_Constant),
            value_(value)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetIntrinsic_Constant>(space, getStubCode(), value_);
        }
    };
};

class ICGetProp_Fallback : public ICMonitoredFallbackStub
{
    friend class ICStubSpace;

    explicit ICGetProp_Fallback(JitCode* stubCode)
      : ICMonitoredFallbackStub(ICStub::GetProp_Fallback, stubCode)
    { }

  public:
    static const uint32_t MAX_OPTIMIZED_STUBS = 16;
    static const size_t UNOPTIMIZABLE_ACCESS_BIT = 0;
    static const size_t ACCESSED_GETTER_BIT = 1;

    void noteUnoptimizableAccess() {
        extra_ |= (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }
    bool hadUnoptimizableAccess() const {
        return extra_ & (1u << UNOPTIMIZABLE_ACCESS_BIT);
    }

    void noteAccessedGetter() {
        extra_ |= (1u << ACCESSED_GETTER_BIT);
    }
    bool hasAccessedGetter() const {
        return extra_ & (1u << ACCESSED_GETTER_BIT);
    }

    class Compiler : public ICStubCompiler {
      protected:
        uint32_t returnOffset_;
        bool generateStubCode(MacroAssembler& masm);
        bool postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::GetProp_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            ICGetProp_Fallback* stub = ICStub::New<ICGetProp_Fallback>(space, getStubCode());
            if (!stub || !stub->initMonitoringChain(cx, space))
                return nullptr;
            return stub;
        }
    };
};

// Stub for sites, which are too polymorphic (i.e. MAX_OPTIMIZED_STUBS was reached)
class ICGetProp_Generic : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    explicit ICGetProp_Generic(JitCode* stubCode, ICStub* firstMonitorStub)
      : ICMonitoredStub(ICStub::GetProp_Generic, stubCode, firstMonitorStub) {}

  public:
    static ICGetProp_Generic* Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                                    ICGetProp_Generic& other);

    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);
        ICStub* firstMonitorStub_;
      public:
        explicit Compiler(JSContext* cx, ICStub* firstMonitorStub)
          : ICStubCompiler(cx, ICStub::GetProp_Generic),
            firstMonitorStub_(firstMonitorStub)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetProp_Generic>(space, getStubCode(), firstMonitorStub_);
        }
    };
};

// Stub for accessing a dense array's length.
class ICGetProp_ArrayLength : public ICStub
{
    friend class ICStubSpace;

    explicit ICGetProp_ArrayLength(JitCode* stubCode)
      : ICStub(GetProp_ArrayLength, stubCode)
    {}

  public:
    class Compiler : public ICStubCompiler {
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::GetProp_ArrayLength)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetProp_ArrayLength>(space, getStubCode());
        }
    };
};

// Stub for accessing a property on a primitive's prototype.
class ICGetProp_Primitive : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected: // Protected to silence Clang warning.
    // Shape of String.prototype/Number.prototype to check for.
    HeapPtrShape protoShape_;

    // Fixed or dynamic slot offset.
    uint32_t offset_;

    ICGetProp_Primitive(JitCode* stubCode, ICStub* firstMonitorStub,
                        Shape* protoShape, uint32_t offset);

  public:
    HeapPtrShape& protoShape() {
        return protoShape_;
    }
    static size_t offsetOfProtoShape() {
        return offsetof(ICGetProp_Primitive, protoShape_);
    }

    static size_t offsetOfOffset() {
        return offsetof(ICGetProp_Primitive, offset_);
    }

    class Compiler : public ICStubCompiler {
        ICStub* firstMonitorStub_;
        JSValueType primitiveType_;
        RootedObject prototype_;
        bool isFixedSlot_;
        uint32_t offset_;

        bool generateStubCode(MacroAssembler& masm);

      protected:
        virtual int32_t getKey() const {
            static_assert(sizeof(JSValueType) == 1, "JSValueType should fit in one byte");
            return static_cast<int32_t>(kind)
                | (static_cast<int32_t>(isFixedSlot_) << 16)
                | (static_cast<int32_t>(primitiveType_) << 24);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, JSValueType primitiveType,
                 HandleObject prototype, bool isFixedSlot, uint32_t offset)
          : ICStubCompiler(cx, ICStub::GetProp_Primitive),
            firstMonitorStub_(firstMonitorStub),
            primitiveType_(primitiveType),
            prototype_(cx, prototype),
            isFixedSlot_(isFixedSlot),
            offset_(offset)
        {}

        ICStub* getStub(ICStubSpace* space) {
            RootedShape protoShape(cx, prototype_->lastProperty());
            return ICStub::New<ICGetProp_Primitive>(space, getStubCode(), firstMonitorStub_,
                                                    protoShape, offset_);
        }
    };
};

// Stub for accessing a string's length.
class ICGetProp_StringLength : public ICStub
{
    friend class ICStubSpace;

    explicit ICGetProp_StringLength(JitCode* stubCode)
      : ICStub(GetProp_StringLength, stubCode)
    {}

  public:
    class Compiler : public ICStubCompiler {
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::GetProp_StringLength)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetProp_StringLength>(space, getStubCode());
        }
    };
};

// Base class for native GetProp stubs.
class ICGetPropNativeStub : public ICMonitoredStub
{
    // Fixed or dynamic slot offset.
    uint32_t offset_;

  protected:
    ICGetPropNativeStub(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                        uint32_t offset);

  public:
    uint32_t offset() const {
        return offset_;
    }
    void notePreliminaryObject() {
        extra_ = 1;
    }
    bool hasPreliminaryObject() const {
        return extra_;
    }
    static size_t offsetOfOffset() {
        return offsetof(ICGetPropNativeStub, offset_);
    }
};

// Stub for accessing an own property on a native object.
class ICGetProp_Native : public ICGetPropNativeStub
{
    friend class ICStubSpace;

    // Object shape (lastProperty).
    HeapPtrShape shape_;

    ICGetProp_Native(JitCode* stubCode, ICStub* firstMonitorStub, Shape* shape,
                     uint32_t offset)
      : ICGetPropNativeStub(GetProp_Native, stubCode, firstMonitorStub, offset),
        shape_(shape)
    {}

  public:
    HeapPtrShape& shape() {
        return shape_;
    }
    static size_t offsetOfShape() {
        return offsetof(ICGetProp_Native, shape_);
    }

    static ICGetProp_Native* Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                                   ICGetProp_Native& other);
};

// Stub for accessing a property on a native object's prototype. Note that due to
// the shape teleporting optimization, we only have to guard on the object's shape
// and the holder's shape.
class ICGetProp_NativePrototype : public ICGetPropNativeStub
{
    friend class ICStubSpace;

  protected:
    // Object shape (lastProperty).
    HeapPtrShape shape_;

    // Holder and its shape.
    HeapPtrObject holder_;
    HeapPtrShape holderShape_;

    ICGetProp_NativePrototype(JitCode* stubCode, ICStub* firstMonitorStub, Shape* shape,
                              uint32_t offset, JSObject* holder, Shape* holderShape);

  public:
    static ICGetProp_NativePrototype* Clone(ICStubSpace* space,
                                            ICStub* firstMonitorStub,
                                            ICGetProp_NativePrototype& other);

  public:
    HeapPtrShape& shape() {
        return shape_;
    }
    HeapPtrObject& holder() {
        return holder_;
    }
    HeapPtrShape& holderShape() {
        return holderShape_;
    }
    static size_t offsetOfShape() {
        return offsetof(ICGetProp_NativePrototype, shape_);
    }
    static size_t offsetOfHolder() {
        return offsetof(ICGetProp_NativePrototype, holder_);
    }
    static size_t offsetOfHolderShape() {
        return offsetof(ICGetProp_NativePrototype, holderShape_);
    }
};

// Stub for accessing a property on an unboxed object's native prototype. Note
// that due to the shape teleporting optimization, we only have to guard on the
// object's type and the holder's shape.
class ICGetProp_UnboxedPrototype : public ICGetPropNativeStub
{
    friend class ICStubSpace;

  protected:
    // Object group.
    HeapPtrObjectGroup group_;

    // Holder and its shape.
    HeapPtrObject holder_;
    HeapPtrShape holderShape_;

    ICGetProp_UnboxedPrototype(JitCode* stubCode, ICStub* firstMonitorStub,
                               ObjectGroup* group,
                               uint32_t offset, JSObject* holder, Shape* holderShape);

  public:
    static ICGetProp_UnboxedPrototype* Clone(ICStubSpace* space,
                                             ICStub* firstMonitorStub,
                                             ICGetProp_UnboxedPrototype& other);

  public:
    HeapPtrObjectGroup& group() {
        return group_;
    }
    HeapPtrObject& holder() {
        return holder_;
    }
    HeapPtrShape& holderShape() {
        return holderShape_;
    }
    static size_t offsetOfGroup() {
        return offsetof(ICGetProp_UnboxedPrototype, group_);
    }
    static size_t offsetOfHolder() {
        return offsetof(ICGetProp_UnboxedPrototype, holder_);
    }
    static size_t offsetOfHolderShape() {
        return offsetof(ICGetProp_UnboxedPrototype, holderShape_);
    }
};

// Compiler for native GetProp stubs.
class ICGetPropNativeCompiler : public ICStubCompiler
{
    bool isCallProp_;
    ICStub* firstMonitorStub_;
    HandleObject obj_;
    HandleObject holder_;
    HandlePropertyName propName_;
    bool isFixedSlot_;
    uint32_t offset_;
    bool inputDefinitelyObject_;

    bool generateStubCode(MacroAssembler& masm);

  protected:
    virtual int32_t getKey() const {
#if JS_HAS_NO_SUCH_METHOD
        return static_cast<int32_t>(kind) |
               (static_cast<int32_t>(isCallProp_) << 16) |
               (static_cast<int32_t>(isFixedSlot_) << 17) |
               (static_cast<int32_t>(inputDefinitelyObject_) << 18);
#else
        return static_cast<int32_t>(kind) |
               (static_cast<int32_t>(isFixedSlot_) << 16) |
               (static_cast<int32_t>(inputDefinitelyObject_) << 17);
#endif
    }

  public:
    ICGetPropNativeCompiler(JSContext* cx, ICStub::Kind kind, bool isCallProp,
                            ICStub* firstMonitorStub, HandleObject obj, HandleObject holder,
                            HandlePropertyName propName, bool isFixedSlot, uint32_t offset,
                            bool inputDefinitelyObject = false)
      : ICStubCompiler(cx, kind),
        isCallProp_(isCallProp),
        firstMonitorStub_(firstMonitorStub),
        obj_(obj),
        holder_(holder),
        propName_(propName),
        isFixedSlot_(isFixedSlot),
        offset_(offset),
        inputDefinitelyObject_(inputDefinitelyObject)
    {}

    ICGetPropNativeStub* getStub(ICStubSpace* space);
};

template <size_t ProtoChainDepth> class ICGetProp_NativeDoesNotExistImpl;

class ICGetProp_NativeDoesNotExist : public ICMonitoredStub
{
    friend class ICStubSpace;
  public:
    static const size_t MAX_PROTO_CHAIN_DEPTH = 8;

  protected:
    ICGetProp_NativeDoesNotExist(JitCode* stubCode, ICStub* firstMonitorStub,
                                 size_t protoChainDepth);

  public:
    size_t protoChainDepth() const {
        MOZ_ASSERT(extra_ <= MAX_PROTO_CHAIN_DEPTH);
        return extra_;
    }

    template <size_t ProtoChainDepth>
    ICGetProp_NativeDoesNotExistImpl<ProtoChainDepth>* toImpl() {
        MOZ_ASSERT(ProtoChainDepth == protoChainDepth());
        return static_cast<ICGetProp_NativeDoesNotExistImpl<ProtoChainDepth>*>(this);
    }

    static size_t offsetOfShape(size_t idx);
};

template <size_t ProtoChainDepth>
class ICGetProp_NativeDoesNotExistImpl : public ICGetProp_NativeDoesNotExist
{
    friend class ICStubSpace;
  public:
    static const size_t MAX_PROTO_CHAIN_DEPTH = 8;
    static const size_t NumShapes = ProtoChainDepth + 1;

  private:
    mozilla::Array<HeapPtrShape, NumShapes> shapes_;

    ICGetProp_NativeDoesNotExistImpl(JitCode* stubCode, ICStub* firstMonitorStub,
                                     const AutoShapeVector* shapes);

  public:
    void traceShapes(JSTracer* trc) {
        for (size_t i = 0; i < NumShapes; i++)
            MarkShape(trc, &shapes_[i], "baseline-getpropnativedoesnotexist-stub-shape");
    }

    static size_t offsetOfShape(size_t idx) {
        return offsetof(ICGetProp_NativeDoesNotExistImpl, shapes_) + (idx * sizeof(HeapPtrShape));
    }
};

class ICGetPropNativeDoesNotExistCompiler : public ICStubCompiler
{
    ICStub* firstMonitorStub_;
    RootedObject obj_;
    size_t protoChainDepth_;

  protected:
    virtual int32_t getKey() const {
        return static_cast<int32_t>(kind) | (static_cast<int32_t>(protoChainDepth_) << 16);
    }

    bool generateStubCode(MacroAssembler& masm);

  public:
    ICGetPropNativeDoesNotExistCompiler(JSContext* cx, ICStub* firstMonitorStub,
                                        HandleObject obj, size_t protoChainDepth);

    template <size_t ProtoChainDepth>
    ICStub* getStubSpecific(ICStubSpace* space, const AutoShapeVector* shapes) {
        return ICStub::New<ICGetProp_NativeDoesNotExistImpl<ProtoChainDepth>>(space, getStubCode(),
                                                                              firstMonitorStub_, shapes);
    }

    ICStub* getStub(ICStubSpace* space);
};

class ICGetProp_Unboxed : public ICMonitoredStub
{
    friend class ICStubSpace;

    HeapPtrObjectGroup group_;
    uint32_t fieldOffset_;

    ICGetProp_Unboxed(JitCode* stubCode, ICStub* firstMonitorStub, ObjectGroup* group,
                      uint32_t fieldOffset)
      : ICMonitoredStub(ICStub::GetProp_Unboxed, stubCode, firstMonitorStub),
        group_(group), fieldOffset_(fieldOffset)
    {
        (void) fieldOffset_; // Silence clang warning
    }

  public:
    HeapPtrObjectGroup& group() {
        return group_;
    }

    static size_t offsetOfGroup() {
        return offsetof(ICGetProp_Unboxed, group_);
    }
    static size_t offsetOfFieldOffset() {
        return offsetof(ICGetProp_Unboxed, fieldOffset_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        RootedObjectGroup group_;
        uint32_t fieldOffset_;
        JSValueType fieldType_;

        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(fieldType_)) << 16;
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub,
                 ObjectGroup* group, uint32_t fieldOffset, JSValueType fieldType)
          : ICStubCompiler(cx, ICStub::GetProp_Unboxed),
            firstMonitorStub_(firstMonitorStub),
            group_(cx, group),
            fieldOffset_(fieldOffset),
            fieldType_(fieldType)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetProp_Unboxed>(space, getStubCode(), firstMonitorStub_,
                                                  group_, fieldOffset_);
        }
    };
};

static uint32_t
SimpleTypeDescrKey(SimpleTypeDescr* descr)
{
    if (descr->is<ScalarTypeDescr>())
        return uint32_t(descr->as<ScalarTypeDescr>().type()) << 1;
    return (uint32_t(descr->as<ReferenceTypeDescr>().type()) << 1) | 1;
}

class ICGetProp_TypedObject : public ICMonitoredStub
{
    friend class ICStubSpace;

    HeapPtrShape shape_;
    uint32_t fieldOffset_;

    ICGetProp_TypedObject(JitCode* stubCode, ICStub* firstMonitorStub, Shape* shape,
                          uint32_t fieldOffset)
      : ICMonitoredStub(ICStub::GetProp_TypedObject, stubCode, firstMonitorStub),
        shape_(shape), fieldOffset_(fieldOffset)
    {
        (void) fieldOffset_; // Silence clang warning
    }

  public:
    HeapPtrShape& shape() {
        return shape_;
    }

    static size_t offsetOfShape() {
        return offsetof(ICGetProp_TypedObject, shape_);
    }
    static size_t offsetOfFieldOffset() {
        return offsetof(ICGetProp_TypedObject, fieldOffset_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        RootedShape shape_;
        uint32_t fieldOffset_;
        TypedThingLayout layout_;
        Rooted<SimpleTypeDescr*> fieldDescr_;

        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) |
                   (static_cast<int32_t>(SimpleTypeDescrKey(fieldDescr_)) << 16) |
                   (static_cast<int32_t>(layout_) << 24);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub,
                 Shape* shape, uint32_t fieldOffset, SimpleTypeDescr* fieldDescr)
          : ICStubCompiler(cx, ICStub::GetProp_TypedObject),
            firstMonitorStub_(firstMonitorStub),
            shape_(cx, shape),
            fieldOffset_(fieldOffset),
            layout_(GetTypedThingLayout(shape->getObjectClass())),
            fieldDescr_(cx, fieldDescr)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetProp_TypedObject>(space, getStubCode(), firstMonitorStub_,
                                                      shape_, fieldOffset_);
        }
    };
};

class ICGetPropCallGetter : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    // We don't strictly need this for own property getters, but we need it to do
    // Ion optimizations, so we should keep it around.
    HeapPtrObject holder_;

    HeapPtrShape holderShape_;

    // Function to call.
    HeapPtrFunction getter_;

    // PC offset of call
    uint32_t pcOffset_;

    ICGetPropCallGetter(Kind kind, JitCode* stubCode, ICStub* firstMonitorStub, JSObject* holder,
                        Shape* holderShape, JSFunction* getter, uint32_t pcOffset);

  public:
    HeapPtrObject& holder() {
        return holder_;
    }
    HeapPtrShape& holderShape() {
        return holderShape_;
    }
    HeapPtrFunction& getter() {
        return getter_;
    }

    static size_t offsetOfHolder() {
        return offsetof(ICGetPropCallGetter, holder_);
    }
    static size_t offsetOfHolderShape() {
        return offsetof(ICGetPropCallGetter, holderShape_);
    }
    static size_t offsetOfGetter() {
        return offsetof(ICGetPropCallGetter, getter_);
    }
    static size_t offsetOfPCOffset() {
        return offsetof(ICGetPropCallGetter, pcOffset_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        RootedObject holder_;
        RootedFunction getter_;
        uint32_t pcOffset_;
        const Class* outerClass_;

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) |
                   (static_cast<int32_t>(!!outerClass_) << 16);
        }

      public:
        Compiler(JSContext* cx, ICStub::Kind kind, ICStub* firstMonitorStub,
                 HandleObject holder, HandleFunction getter, uint32_t pcOffset,
                 const Class* outerClass)
          : ICStubCompiler(cx, kind),
            firstMonitorStub_(firstMonitorStub),
            holder_(cx, holder),
            getter_(cx, getter),
            pcOffset_(pcOffset),
            outerClass_(outerClass)
        {
            MOZ_ASSERT(kind == ICStub::GetProp_CallScripted ||
                       kind == ICStub::GetProp_CallNative ||
                       kind == ICStub::GetProp_CallNativePrototype);
        }
    };
};

// Stub for calling a getter (native or scripted) on a native object when the getter is kept on
// the proto-chain.
class ICGetPropCallPrototypeGetter : public ICGetPropCallGetter
{
    friend class ICStubSpace;

  protected:
    // shape of receiver object.
    HeapPtrShape receiverShape_;

    ICGetPropCallPrototypeGetter(Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                                 Shape* receiverShape,
                                 JSObject* holder, Shape* holderShape,
                                 JSFunction* getter, uint32_t pcOffset);

  public:
    HeapPtrShape& receiverShape() {
        return receiverShape_;
    }

    static size_t offsetOfReceiverShape() {
        return offsetof(ICGetPropCallPrototypeGetter, receiverShape_);
    }

    class Compiler : public ICGetPropCallGetter::Compiler {
      protected:
        RootedObject receiver_;

      public:
        Compiler(JSContext* cx, ICStub::Kind kind, ICStub* firstMonitorStub,
                 HandleObject obj, HandleObject holder, HandleFunction getter, uint32_t pcOffset,
                 const Class* outerClass)
          : ICGetPropCallGetter::Compiler(cx, kind, firstMonitorStub, holder, getter, pcOffset,
                                          outerClass),
            receiver_(cx, obj)
        {
            MOZ_ASSERT(kind == ICStub::GetProp_CallScripted ||
                       kind == ICStub::GetProp_CallNativePrototype);
        }
    };
};

// Stub for calling a scripted getter on a native object when the getter is kept on the
// proto-chain.
class ICGetProp_CallScripted : public ICGetPropCallPrototypeGetter
{
    friend class ICStubSpace;

  protected:
    ICGetProp_CallScripted(JitCode* stubCode, ICStub* firstMonitorStub,
                           Shape* receiverShape, JSObject* holder, Shape* holderShape,
                           JSFunction* getter, uint32_t pcOffset)
      : ICGetPropCallPrototypeGetter(GetProp_CallScripted, stubCode, firstMonitorStub,
                                     receiverShape, holder, holderShape, getter, pcOffset)
    {}

  public:
    static ICGetProp_CallScripted* Clone(ICStubSpace* space,
                                         ICStub* firstMonitorStub, ICGetProp_CallScripted& other);

    class Compiler : public ICGetPropCallPrototypeGetter::Compiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, HandleObject obj,
                 HandleObject holder, HandleFunction getter, uint32_t pcOffset)
          : ICGetPropCallPrototypeGetter::Compiler(cx, ICStub::GetProp_CallScripted,
                                                   firstMonitorStub, obj, holder,
                                                   getter, pcOffset, /* outerClass = */ nullptr)
        {}

        ICStub* getStub(ICStubSpace* space) {
            RootedShape receiverShape(cx, receiver_->lastProperty());
            RootedShape holderShape(cx, holder_->lastProperty());
            return ICStub::New<ICGetProp_CallScripted>(space, getStubCode(), firstMonitorStub_,
                                                       receiverShape, holder_, holderShape, getter_,
                                                       pcOffset_);
        }
    };
};

// Stub for calling an own native getter on a native object.
class ICGetProp_CallNative : public ICGetPropCallGetter
{
    friend class ICStubSpace;

  protected:

    ICGetProp_CallNative(JitCode* stubCode, ICStub* firstMonitorStub, JSObject* obj,
                         Shape* shape, JSFunction* getter, uint32_t pcOffset)
      : ICGetPropCallGetter(GetProp_CallNative, stubCode, firstMonitorStub, obj, shape,
                            getter, pcOffset)
    { }

  public:
    static ICGetProp_CallNative* Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                                       ICGetProp_CallNative& other);

    class Compiler : public ICGetPropCallGetter::Compiler
    {
        bool inputDefinitelyObject_;
      protected:
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) |
                   (static_cast<int32_t>(inputDefinitelyObject_) << 16);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, HandleObject obj,
                 HandleFunction getter, uint32_t pcOffset, const Class* outerClass,
                 bool inputDefinitelyObject = false)
          : ICGetPropCallGetter::Compiler(cx, ICStub::GetProp_CallNative, firstMonitorStub,
                                          obj, getter, pcOffset, outerClass),
            inputDefinitelyObject_(inputDefinitelyObject)
        {}

        ICStub* getStub(ICStubSpace* space) {
            RootedShape shape(cx, holder_->lastProperty());
            return ICStub::New<ICGetProp_CallNative>(space, getStubCode(), firstMonitorStub_,
                                                     holder_, shape, getter_, pcOffset_);
        }
    };
};

// Stub for calling an native getter on a native object when the getter is kept on the proto-chain.
class ICGetProp_CallNativePrototype : public ICGetPropCallPrototypeGetter
{
    friend class ICStubSpace;

  protected:
    ICGetProp_CallNativePrototype(JitCode* stubCode, ICStub* firstMonitorStub,
                         Shape* receiverShape, JSObject* holder, Shape* holderShape,
                         JSFunction* getter, uint32_t pcOffset)
      : ICGetPropCallPrototypeGetter(GetProp_CallNativePrototype, stubCode, firstMonitorStub,
                                     receiverShape, holder, holderShape, getter, pcOffset)
    {}

  public:
    static ICGetProp_CallNativePrototype* Clone(ICStubSpace* space,
                                                ICStub* firstMonitorStub,
                                                ICGetProp_CallNativePrototype& other);

    class Compiler : public ICGetPropCallPrototypeGetter::Compiler {
        bool inputDefinitelyObject_;
      protected:
        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) |
                   (static_cast<int32_t>(inputDefinitelyObject_) << 16);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, HandleObject obj,
                 HandleObject holder, HandleFunction getter, uint32_t pcOffset,
                 const Class* outerClass, bool inputDefinitelyObject = false)
          : ICGetPropCallPrototypeGetter::Compiler(cx, ICStub::GetProp_CallNativePrototype,
                                                   firstMonitorStub, obj, holder,
                                                   getter, pcOffset, outerClass),
            inputDefinitelyObject_(inputDefinitelyObject)
        {}

        ICStub* getStub(ICStubSpace* space) {
            RootedShape receiverShape(cx, receiver_->lastProperty());
            RootedShape holderShape(cx, holder_->lastProperty());
            return ICStub::New<ICGetProp_CallNativePrototype>(space, getStubCode(), firstMonitorStub_,
                                                              receiverShape, holder_, holderShape,
                                                              getter_, pcOffset_);
        }
    };
};

class ICGetPropCallDOMProxyNativeStub : public ICGetPropCallGetter
{
  friend class ICStubSpace;
  protected:
    // Shape of the DOMProxy.  This enforces its class and hence its proxy handler.
    HeapPtrShape shape_;

    // Object shape of expected expando object. (nullptr if no expando object should be there)
    HeapPtrShape expandoShape_;

    ICGetPropCallDOMProxyNativeStub(ICStub::Kind kind, JitCode* stubCode,
                                    ICStub* firstMonitorStub, Shape* shape,
                                    Shape* expandoShape,
                                    JSObject* holder, Shape* holderShape,
                                    JSFunction* getter, uint32_t pcOffset);

  public:
    HeapPtrShape& shape() {
        return shape_;
    }
    HeapPtrShape& expandoShape() {
        return expandoShape_;
    }
    static size_t offsetOfShape() {
        return offsetof(ICGetPropCallDOMProxyNativeStub, shape_);
    }
    static size_t offsetOfExpandoShape() {
        return offsetof(ICGetPropCallDOMProxyNativeStub, expandoShape_);
    }
};

class ICGetProp_CallDOMProxyNative : public ICGetPropCallDOMProxyNativeStub
{
    friend class ICStubSpace;
    ICGetProp_CallDOMProxyNative(JitCode* stubCode, ICStub* firstMonitorStub, Shape* shape,
                                 Shape* expandoShape,
                                 JSObject* holder, Shape* holderShape,
                                 JSFunction* getter, uint32_t pcOffset)
      : ICGetPropCallDOMProxyNativeStub(ICStub::GetProp_CallDOMProxyNative, stubCode,
                                        firstMonitorStub, shape, expandoShape,
                                        holder, holderShape, getter, pcOffset)
    {}

  public:
    static ICGetProp_CallDOMProxyNative* Clone(ICStubSpace* space,
                                               ICStub* firstMonitorStub,
                                               ICGetProp_CallDOMProxyNative& other);
};

class ICGetProp_CallDOMProxyWithGenerationNative : public ICGetPropCallDOMProxyNativeStub
{
  protected:
    ExpandoAndGeneration* expandoAndGeneration_;
    uint32_t generation_;

  public:
    ICGetProp_CallDOMProxyWithGenerationNative(JitCode* stubCode, ICStub* firstMonitorStub,
                                               Shape* shape,
                                               ExpandoAndGeneration* expandoAndGeneration,
                                               uint32_t generation, Shape* expandoShape,
                                               JSObject* holder, Shape* holderShape,
                                               JSFunction* getter, uint32_t pcOffset)
      : ICGetPropCallDOMProxyNativeStub(ICStub::GetProp_CallDOMProxyWithGenerationNative,
                                        stubCode, firstMonitorStub, shape,
                                        expandoShape, holder, holderShape, getter, pcOffset),
        expandoAndGeneration_(expandoAndGeneration),
        generation_(generation)
    {
    }

    static ICGetProp_CallDOMProxyWithGenerationNative*
    Clone(ICStubSpace* space, ICStub* firstMonitorStub,
          ICGetProp_CallDOMProxyWithGenerationNative& other);

    void* expandoAndGeneration() const {
        return expandoAndGeneration_;
    }
    uint32_t generation() const {
        return generation_;
    }

    void setGeneration(uint32_t value) {
        generation_ = value;
    }

    static size_t offsetOfInternalStruct() {
        return offsetof(ICGetProp_CallDOMProxyWithGenerationNative, expandoAndGeneration_);
    }
    static size_t offsetOfGeneration() {
        return offsetof(ICGetProp_CallDOMProxyWithGenerationNative, generation_);
    }
};

class ICGetPropCallDOMProxyNativeCompiler : public ICStubCompiler {
    ICStub* firstMonitorStub_;
    Rooted<ProxyObject*> proxy_;
    RootedObject holder_;
    RootedFunction getter_;
    uint32_t pcOffset_;

    bool generateStubCode(MacroAssembler& masm, Address* internalStructAddr,
                          Address* generationAddr);
    bool generateStubCode(MacroAssembler& masm);

  public:
    ICGetPropCallDOMProxyNativeCompiler(JSContext* cx, ICStub::Kind kind,
                                        ICStub* firstMonitorStub, Handle<ProxyObject*> proxy,
                                        HandleObject holder, HandleFunction getter,
                                        uint32_t pcOffset);

    ICStub* getStub(ICStubSpace* space);
};

class ICGetProp_DOMProxyShadowed : public ICMonitoredStub
{
  friend class ICStubSpace;
  protected:
    HeapPtrShape shape_;
    const BaseProxyHandler* proxyHandler_;
    HeapPtrPropertyName name_;
    uint32_t pcOffset_;

    ICGetProp_DOMProxyShadowed(JitCode* stubCode, ICStub* firstMonitorStub, Shape* shape,
                               const BaseProxyHandler* proxyHandler, PropertyName* name,
                               uint32_t pcOffset);

  public:
    static ICGetProp_DOMProxyShadowed* Clone(ICStubSpace* space,
                                             ICStub* firstMonitorStub,
                                             ICGetProp_DOMProxyShadowed& other);

    HeapPtrShape& shape() {
        return shape_;
    }
    HeapPtrPropertyName& name() {
        return name_;
    }

    static size_t offsetOfShape() {
        return offsetof(ICGetProp_DOMProxyShadowed, shape_);
    }
    static size_t offsetOfProxyHandler() {
        return offsetof(ICGetProp_DOMProxyShadowed, proxyHandler_);
    }
    static size_t offsetOfName() {
        return offsetof(ICGetProp_DOMProxyShadowed, name_);
    }
    static size_t offsetOfPCOffset() {
        return offsetof(ICGetProp_DOMProxyShadowed, pcOffset_);
    }

    class Compiler : public ICStubCompiler {
        ICStub* firstMonitorStub_;
        Rooted<ProxyObject*> proxy_;
        RootedPropertyName name_;
        uint32_t pcOffset_;

        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, Handle<ProxyObject*> proxy,
                 HandlePropertyName name, uint32_t pcOffset)
          : ICStubCompiler(cx, ICStub::GetProp_CallNative),
            firstMonitorStub_(firstMonitorStub),
            proxy_(cx, proxy),
            name_(cx, name),
            pcOffset_(pcOffset)
        {}

        ICStub* getStub(ICStubSpace* space);
    };
};

class ICGetProp_ArgumentsLength : public ICStub
{
  friend class ICStubSpace;
  public:
    enum Which { Normal, Strict, Magic };

  protected:
    explicit ICGetProp_ArgumentsLength(JitCode* stubCode)
      : ICStub(ICStub::GetProp_ArgumentsLength, stubCode)
    { }

  public:
    class Compiler : public ICStubCompiler {
      protected:
        Which which_;

        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(which_) << 16);
        }

      public:
        Compiler(JSContext* cx, Which which)
          : ICStubCompiler(cx, ICStub::GetProp_ArgumentsLength),
            which_(which)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetProp_ArgumentsLength>(space, getStubCode());
        }
    };
};

class ICGetProp_ArgumentsCallee : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    ICGetProp_ArgumentsCallee(JitCode* stubCode, ICStub* firstMonitorStub);

  public:
    class Compiler : public ICStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub)
          : ICStubCompiler(cx, ICStub::GetProp_ArgumentsCallee),
            firstMonitorStub_(firstMonitorStub)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICGetProp_ArgumentsCallee>(space, getStubCode(), firstMonitorStub_);
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
      protected:
        uint32_t returnOffset_;
        bool generateStubCode(MacroAssembler& masm);
        bool postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code);

      public:
        explicit Compiler(JSContext* cx)
          : ICStubCompiler(cx, ICStub::SetProp_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICSetProp_Fallback>(space, getStubCode());
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
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(isFixedSlot_) << 16);
        }

        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, HandleObject obj, bool isFixedSlot, uint32_t offset)
          : ICStubCompiler(cx, ICStub::SetProp_Native),
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
                            const AutoShapeVector* shapes,
                            Shape* newShape, ObjectGroup* newGroup, uint32_t offset);

  public:
    void traceShapes(JSTracer* trc) {
        for (size_t i = 0; i < NumShapes; i++)
            MarkShape(trc, &shapes_[i], "baseline-setpropnativeadd-stub-shape");
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
        return static_cast<int32_t>(kind) | (static_cast<int32_t>(isFixedSlot_) << 16) |
               (static_cast<int32_t>(protoChainDepth_) << 20);
    }

    bool generateStubCode(MacroAssembler& masm);

  public:
    ICSetPropNativeAddCompiler(JSContext* cx, HandleObject obj,
                               HandleShape oldShape, HandleObjectGroup oldGroup,
                               size_t protoChainDepth, bool isFixedSlot, uint32_t offset);

    template <size_t ProtoChainDepth>
    ICUpdatedStub* getStubSpecific(ICStubSpace* space, const AutoShapeVector* shapes)
    {
        RootedObjectGroup newGroup(cx, obj_->getGroup(cx));
        if (!newGroup)
            return nullptr;

        // Only specify newGroup when the object's group changes due to the
        // object becoming fully initialized per the acquired properties
        // analysis.
        if (newGroup == oldGroup_)
            newGroup = nullptr;

        RootedShape newShape(cx, obj_->lastProperty());

        return ICStub::New<ICSetProp_NativeAddImpl<ProtoChainDepth>>(
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
            return static_cast<int32_t>(kind) |
                   (static_cast<int32_t>(fieldType_) << 16);
        }

      public:
        Compiler(JSContext* cx, ObjectGroup* group, uint32_t fieldOffset,
                 JSValueType fieldType)
          : ICStubCompiler(cx, ICStub::SetProp_Unboxed),
            group_(cx, group),
            fieldOffset_(fieldOffset),
            fieldType_(fieldType)
        {}

        ICUpdatedStub* getStub(ICStubSpace* space) {
            ICUpdatedStub* stub = ICStub::New<ICSetProp_Unboxed>(space, getStubCode(),
                                                                 group_, fieldOffset_);
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
            return static_cast<int32_t>(kind) |
                   (static_cast<int32_t>(SimpleTypeDescrKey(fieldDescr_)) << 16) |
                   (static_cast<int32_t>(layout_) << 24);
        }

      public:
        Compiler(JSContext* cx, Shape* shape, ObjectGroup* group, uint32_t fieldOffset,
                 SimpleTypeDescr* fieldDescr)
          : ICStubCompiler(cx, ICStub::SetProp_TypedObject),
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
            ICUpdatedStub* stub = ICStub::New<ICSetProp_TypedObject>(space, getStubCode(), shape_,
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

// Base stub for calling a setters on a native object.
class ICSetPropCallSetter : public ICStub
{
    friend class ICStubSpace;

  protected:
    // Object shape (lastProperty).
    HeapPtrShape shape_;

    // Holder and shape.
    HeapPtrObject holder_;
    HeapPtrShape holderShape_;

    // Function to call.
    HeapPtrFunction setter_;

    // PC of call, for profiler
    uint32_t pcOffset_;

    ICSetPropCallSetter(Kind kind, JitCode* stubCode, Shape* shape, JSObject* holder,
                        Shape* holderShape, JSFunction* setter, uint32_t pcOffset);

  public:
    HeapPtrShape& shape() {
        return shape_;
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

    static size_t offsetOfShape() {
        return offsetof(ICSetPropCallSetter, shape_);
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
        RootedObject obj_;
        RootedObject holder_;
        RootedFunction setter_;
        uint32_t pcOffset_;

      public:
        Compiler(JSContext* cx, ICStub::Kind kind, HandleObject obj, HandleObject holder,
                 HandleFunction setter, uint32_t pcOffset)
          : ICStubCompiler(cx, kind),
            obj_(cx, obj),
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
    ICSetProp_CallScripted(JitCode* stubCode, Shape* shape, JSObject* holder,
                           Shape* holderShape, JSFunction* setter, uint32_t pcOffset)
      : ICSetPropCallSetter(SetProp_CallScripted, stubCode, shape, holder, holderShape,
                            setter, pcOffset)
    {}

  public:
    static ICSetProp_CallScripted* Clone(ICStubSpace* space, ICStub*,
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
            RootedShape shape(cx, obj_->lastProperty());
            RootedShape holderShape(cx, holder_->lastProperty());
            return ICStub::New<ICSetProp_CallScripted>(space, getStubCode(), shape, holder_,
                                                       holderShape, setter_, pcOffset_);
        }
    };
};

// Stub for calling a native setter on a native object.
class ICSetProp_CallNative : public ICSetPropCallSetter
{
    friend class ICStubSpace;

  protected:
    ICSetProp_CallNative(JitCode* stubCode, Shape* shape, JSObject* holder,
                         Shape* holderShape, JSFunction* setter, uint32_t pcOffset)
      : ICSetPropCallSetter(SetProp_CallNative, stubCode, shape, holder, holderShape,
                            setter, pcOffset)
    {}

  public:
    static ICSetProp_CallNative* Clone(ICStubSpace* space, ICStub*,
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
            RootedShape shape(cx, obj_->lastProperty());
            RootedShape holderShape(cx, holder_->lastProperty());
            return ICStub::New<ICSetProp_CallNative>(space, getStubCode(), shape, holder_,
                                                     holderShape, setter_, pcOffset_);
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
      : ICStubCompiler(cx, kind)
    { }

    enum FunApplyThing {
        FunApply_MagicArgs,
        FunApply_Array
    };

    void pushCallArguments(MacroAssembler& masm, GeneralRegisterSet regs, Register argcReg,
                           bool isJitCall);
    void pushSpreadCallArguments(MacroAssembler& masm, GeneralRegisterSet regs, Register argcReg,
                                 bool isJitCall);
    void guardSpreadCall(MacroAssembler& masm, Register argcReg, Label* failure);
    Register guardFunApply(MacroAssembler& masm, GeneralRegisterSet regs, Register argcReg,
                           bool checkNative, FunApplyThing applyThing, Label* failure);
    void pushCallerArguments(MacroAssembler& masm, GeneralRegisterSet regs);
    void pushArrayArguments(MacroAssembler& masm, Address arrayVal, GeneralRegisterSet regs);
};

class ICCall_Fallback : public ICMonitoredFallbackStub
{
    friend class ICStubSpace;
  public:
    static const unsigned CONSTRUCTING_FLAG = 0x1;
    static const unsigned UNOPTIMIZABLE_CALL_FLAG = 0x2;

    static const uint32_t MAX_OPTIMIZED_STUBS = 16;
    static const uint32_t MAX_SCRIPTED_STUBS = 7;
    static const uint32_t MAX_NATIVE_STUBS = 7;
  private:

    ICCall_Fallback(JitCode* stubCode, bool isConstructing)
      : ICMonitoredFallbackStub(ICStub::Call_Fallback, stubCode)
    {
        extra_ = 0;
        if (isConstructing)
            extra_ |= CONSTRUCTING_FLAG;
    }

  public:
    bool isConstructing() const {
        return extra_ & CONSTRUCTING_FLAG;
    }

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
      protected:
        bool isConstructing_;
        bool isSpread_;
        uint32_t returnOffset_;
        bool generateStubCode(MacroAssembler& masm);
        bool postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(isSpread_) << 16);
        }

      public:
        Compiler(JSContext* cx, bool isConstructing, bool isSpread)
          : ICCallStubCompiler(cx, ICStub::Call_Fallback),
            isConstructing_(isConstructing),
            isSpread_(isSpread)
        { }

        ICStub* getStub(ICStubSpace* space) {
            ICCall_Fallback* stub = ICStub::New<ICCall_Fallback>(space, getStubCode(), isConstructing_);
            if (!stub || !stub->initMonitoringChain(cx, space))
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
    static ICCall_Scripted* Clone(ICStubSpace* space, ICStub* firstMonitorStub,
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
    static ICCall_AnyScripted* Clone(ICStubSpace* space, ICStub* firstMonitorStub,
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
        return static_cast<int32_t>(kind) | (static_cast<int32_t>(isConstructing_) << 16) |
               (static_cast<int32_t>(isSpread_) << 17);
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
            return ICStub::New<ICCall_Scripted>(space, getStubCode(), firstMonitorStub_,
                                                callee_, templateObject_,
                                                pcOffset_);
        }
        return ICStub::New<ICCall_AnyScripted>(space, getStubCode(), firstMonitorStub_, pcOffset_);
    }
};

class ICCall_Native : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    HeapPtrFunction callee_;
    HeapPtrObject templateObject_;
    uint32_t pcOffset_;

#if defined(JS_ARM_SIMULATOR) || defined(JS_MIPS_SIMULATOR)
    void* native_;
#endif

    ICCall_Native(JitCode* stubCode, ICStub* firstMonitorStub,
                  JSFunction* callee, JSObject* templateObject,
                  uint32_t pcOffset);

  public:
    static ICCall_Native* Clone(ICStubSpace* space, ICStub* firstMonitorStub,
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

#if defined(JS_ARM_SIMULATOR) || defined(JS_MIPS_SIMULATOR)
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
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(isConstructing_) << 16) |
                   (static_cast<int32_t>(isSpread_) << 17);
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
            return ICStub::New<ICCall_Native>(space, getStubCode(), firstMonitorStub_,
                                              callee_, templateObject_, pcOffset_);
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
    static ICCall_ClassHook* Clone(ICStubSpace* space, ICStub* firstMonitorStub,
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
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(isConstructing_) << 16);
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
            return ICStub::New<ICCall_ClassHook>(space, getStubCode(), firstMonitorStub_,
                                                 clasp_, native_, templateObject_, pcOffset_);
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
    static ICCall_ScriptedApplyArray* Clone(ICStubSpace* space,
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
            return static_cast<int32_t>(kind);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset)
          : ICCallStubCompiler(cx, ICStub::Call_ScriptedApplyArray),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCall_ScriptedApplyArray>(space, getStubCode(), firstMonitorStub_,
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
    static ICCall_ScriptedApplyArguments* Clone(ICStubSpace* space,
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
            return static_cast<int32_t>(kind);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset)
          : ICCallStubCompiler(cx, ICStub::Call_ScriptedApplyArguments),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCall_ScriptedApplyArguments>(space, getStubCode(), firstMonitorStub_,
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
    static ICCall_ScriptedFunCall* Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                                         ICCall_ScriptedFunCall& other);

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
            return static_cast<int32_t>(kind);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset)
          : ICCallStubCompiler(cx, ICStub::Call_ScriptedFunCall),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCall_ScriptedFunCall>(space, getStubCode(), firstMonitorStub_,
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
    HeapPtrArrayObject templateObject_;

    ICCall_StringSplit(JitCode* stubCode, ICStub* firstMonitorStub, uint32_t pcOffset, JSString* thisString,
                       JSString* argString, ArrayObject* templateObject)
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

    HeapPtrArrayObject& templateObject() {
        return templateObject_;
    }

    class Compiler : public ICCallStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        uint32_t pcOffset_;
        RootedString expectedThis_;
        RootedString expectedArg_;
        RootedArrayObject templateObject_;

        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(kind);
        }

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, uint32_t pcOffset, HandleString thisString,
                 HandleString argString, HandleValue templateObject)
          : ICCallStubCompiler(cx, ICStub::Call_StringSplit),
            firstMonitorStub_(firstMonitorStub),
            pcOffset_(pcOffset),
            expectedThis_(cx, thisString),
            expectedArg_(cx, argString),
            templateObject_(cx, &templateObject.toObject().as<ArrayObject>())
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCall_StringSplit>(space, getStubCode(), firstMonitorStub_,
                                                   pcOffset_, expectedThis_, expectedArg_,
                                                   templateObject_);
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
          : ICStubCompiler(cx, ICStub::Call_IsSuspendedStarGenerator)
        {}
        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICCall_IsSuspendedStarGenerator>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::TableSwitch), pc_(pc)
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
          : ICStubCompiler(cx, ICStub::IteratorNew_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICIteratorNew_Fallback>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::IteratorMore_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICIteratorMore_Fallback>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::IteratorMore_Native)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICIteratorMore_Native>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::IteratorClose_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICIteratorClose_Fallback>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::InstanceOf_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICInstanceOf_Fallback>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::InstanceOf_Function),
            shape_(cx, shape),
            prototypeObj_(cx, prototypeObj),
            slot_(slot)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICInstanceOf_Function>(space, getStubCode(), shape_, prototypeObj_, slot_);
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
          : ICStubCompiler(cx, ICStub::TypeOf_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICTypeOf_Fallback>(space, getStubCode());
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
            return static_cast<int32_t>(kind) | (static_cast<int32_t>(type_) << 16);
        }

      public:
        Compiler(JSContext* cx, JSType type, HandleString string)
          : ICStubCompiler(cx, ICStub::TypeOf_Typed),
            type_(type),
            typeString_(cx, string)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICTypeOf_Typed>(space, getStubCode(), type_);
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
          : ICStubCompiler(cx, ICStub::Rest_Fallback),
            templateObject(cx, templateObject)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICRest_Fallback>(space, getStubCode(), templateObject);
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
          : ICStubCompiler(cx, ICStub::RetSub_Fallback)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICRetSub_Fallback>(space, getStubCode());
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
          : ICStubCompiler(cx, ICStub::RetSub_Resume),
            pcOffset_(pcOffset),
            addr_(addr)
        { }

        ICStub* getStub(ICStubSpace* space) {
            return ICStub::New<ICRetSub_Resume>(space, getStubCode(), pcOffset_, addr_);
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
