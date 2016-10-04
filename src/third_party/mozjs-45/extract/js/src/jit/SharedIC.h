/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_SharedIC_h
#define jit_SharedIC_h

#include "jscntxt.h"
#include "jscompartment.h"
#include "jsgc.h"

#include "jit/BaselineICList.h"
#include "jit/BaselineJIT.h"
#include "jit/MacroAssembler.h"
#include "jit/SharedICList.h"
#include "jit/SharedICRegisters.h"
#include "vm/ReceiverGuard.h"
#include "vm/TypedArrayCommon.h"

namespace js {
namespace jit {

class AutoShapeVector;

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

#define FORWARD_DECLARE_STUBS(kindName) class IC##kindName;
    IC_BASELINE_STUB_KIND_LIST(FORWARD_DECLARE_STUBS)
    IC_SHARED_STUB_KIND_LIST(FORWARD_DECLARE_STUBS)
#undef FORWARD_DECLARE_STUBS

#ifdef JS_JITSPEW
void FallbackICSpew(JSContext* cx, ICFallbackStub* stub, const char* fmt, ...);
void TypeFallbackICSpew(JSContext* cx, ICTypeMonitor_Fallback* stub, const char* fmt, ...);
#else
#define FallbackICSpew(...)
#define TypeFallbackICSpew(...)
#endif

//
// An entry in the JIT IC descriptor table.
//
class ICEntry
{
  private:
    // A pointer to the shared IC stub for this instruction.
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

    CodeOffset returnOffset() const {
        return CodeOffset(returnOffset_);
    }

    void setReturnOffset(CodeOffset offset) {
        MOZ_ASSERT(offset.offset() <= (size_t) UINT32_MAX);
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

    void trace(JSTracer* trc);
};

class IonICEntry : public ICEntry
{
  JSScript* script_;

  public:
    IonICEntry(uint32_t pcOffset, Kind kind, JSScript* script)
      : ICEntry(pcOffset, kind),
        script_(script)
    { }

    JSScript* script() {
        return script_;
    }

};


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
        IC_BASELINE_STUB_KIND_LIST(DEF_ENUM_KIND)
        IC_SHARED_STUB_KIND_LIST(DEF_ENUM_KIND)
#undef DEF_ENUM_KIND
        LIMIT
    };

    static inline bool IsValidKind(Kind k) {
        return (k > INVALID) && (k < LIMIT);
    }

    static const char* KindString(Kind k) {
        switch(k) {
#define DEF_KIND_STR(kindName) case kindName: return #kindName;
            IC_BASELINE_STUB_KIND_LIST(DEF_KIND_STR)
            IC_SHARED_STUB_KIND_LIST(DEF_KIND_STR)
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
    static T* New(JSContext* cx, ICStubSpace* space, JitCode* code, Args&&... args) {
        if (!code)
            return nullptr;
        T* result = space->allocate<T>(code, mozilla::Forward<Args>(args)...);
        if (!result)
            ReportOutOfMemory(cx);
        return result;
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
    IC_BASELINE_STUB_KIND_LIST(KIND_METHODS)
    IC_SHARED_STUB_KIND_LIST(KIND_METHODS)
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
          case GetElem_NativeSlotName:
          case GetElem_NativeSlotSymbol:
          case GetElem_NativePrototypeSlotName:
          case GetElem_NativePrototypeSlotSymbol:
          case GetElem_NativePrototypeCallNativeName:
          case GetElem_NativePrototypeCallNativeSymbol:
          case GetElem_NativePrototypeCallScriptedName:
          case GetElem_NativePrototypeCallScriptedSymbol:
          case GetElem_UnboxedPropertyName:
          case GetProp_CallScripted:
          case GetProp_CallNative:
          case GetProp_CallNativeGlobal:
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
    // field, and as new stubs are added, it'll point to the current
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
    // address until after compile when the JitScript is created.  This method
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

  public:
    enum class Engine {
        Baseline = 0,
        IonMonkey
    };

  protected:
    JSContext* cx;
    ICStub::Kind kind;
    Engine engine_;
    bool inStubFrame_;

#ifdef DEBUG
    bool entersStubFrame_;
    uint32_t framePushedAtEnterStubFrame_;
#endif

    // By default the stubcode key is just the kind.
    virtual int32_t getKey() const {
        return static_cast<int32_t>(engine_) |
              (static_cast<int32_t>(kind) << 1);
    }

    virtual bool generateStubCode(MacroAssembler& masm) = 0;
    virtual void postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> genCode) {}

    JitCode* getStubCode();

    ICStubCompiler(JSContext* cx, ICStub::Kind kind, Engine engine)
      : suppressGC(cx), cx(cx), kind(kind), engine_(engine), inStubFrame_(false)
#ifdef DEBUG
      , entersStubFrame_(false), framePushedAtEnterStubFrame_(0)
#endif
    {}

    // Pushes the frame ptr.
    void PushFramePtr(MacroAssembler& masm, Register scratch);
    void pushFramePtr(MacroAssembler& masm, Register scratch);

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

    inline AllocatableGeneralRegisterSet availableGeneralRegs(size_t numInputs) const {
        AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
#if defined(JS_CODEGEN_ARM)
        MOZ_ASSERT(!regs.has(BaselineStackReg));
        MOZ_ASSERT(!regs.has(ICTailCallReg));
        regs.take(BaselineSecondScratchReg);
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        MOZ_ASSERT(!regs.has(BaselineStackReg));
        MOZ_ASSERT(!regs.has(ICTailCallReg));
        MOZ_ASSERT(!regs.has(BaselineSecondScratchReg));
#elif defined(JS_CODEGEN_ARM64)
        MOZ_ASSERT(!regs.has(PseudoStackPointer));
        MOZ_ASSERT(!regs.has(RealStackPointer));
        MOZ_ASSERT(!regs.has(ICTailCallReg));
#else
        MOZ_ASSERT(!regs.has(BaselineStackReg));
#endif
        regs.take(BaselineFrameReg);
        regs.take(ICStubReg);
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

    bool emitPostWriteBarrierSlot(MacroAssembler& masm, Register obj, ValueOperand val,
                                  Register scratch, LiveGeneralRegisterSet saveRegs);

    template <typename T, typename... Args>
    T* newStub(Args&&... args) {
        return ICStub::New<T>(cx, mozilla::Forward<Args>(args)...);
    }

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
    bool initMonitoringChain(JSContext* cx, ICStubSpace* space, ICStubCompiler::Engine engine);
    bool addMonitorStubForValue(JSContext* cx, JSScript* script, HandleValue val,
                                ICStubCompiler::Engine engine);

    inline ICTypeMonitor_Fallback* fallbackMonitorStub() const {
        return fallbackMonitorStub_;
    }

    static inline size_t offsetOfFallbackMonitorStub() {
        return offsetof(ICMonitoredFallbackStub, fallbackMonitorStub_);
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
        return static_cast<int32_t>(engine_) |
              (static_cast<int32_t>(kind) << 1) |
              (static_cast<int32_t>(op) << 17);
    }

    ICMultiStubCompiler(JSContext* cx, ICStub::Kind kind, JSOp op, Engine engine)
      : ICStubCompiler(cx, kind, engine), op(op) {}
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
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(flags_) << 17);
        }

      public:
        Compiler(JSContext* cx, Kind kind, Engine engine_, TypeCheckPrimitiveSetStub* existingStub,
                 JSValueType type)
          : ICStubCompiler(cx, kind, engine_),
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
    bool addMonitorStubForValue(JSContext* cx, JSScript* script, HandleValue val,
                                ICStubCompiler::Engine engine);

    void resetMonitorStubChain(Zone* zone);

    // Compiler for this stub kind.
    class Compiler : public ICStubCompiler {
        ICMonitoredFallbackStub* mainFallbackStub_;
        uint32_t argumentIndex_;

      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, Engine engine, ICMonitoredFallbackStub* mainFallbackStub)
            : ICStubCompiler(cx, ICStub::TypeMonitor_Fallback, engine),
            mainFallbackStub_(mainFallbackStub),
            argumentIndex_(BYTECODE_INDEX)
        { }

        Compiler(JSContext* cx, Engine engine, uint32_t argumentIndex)
          : ICStubCompiler(cx, ICStub::TypeMonitor_Fallback, engine),
            mainFallbackStub_(nullptr),
            argumentIndex_(argumentIndex)
        { }

        ICTypeMonitor_Fallback* getStub(ICStubSpace* space) {
            return newStub<ICTypeMonitor_Fallback>(space, getStubCode(), mainFallbackStub_,
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
        Compiler(JSContext* cx, Engine engine, ICTypeMonitor_PrimitiveSet* existingStub,
                 JSValueType type)
          : TypeCheckPrimitiveSetStub::Compiler(cx, TypeMonitor_PrimitiveSet, engine, existingStub,
                                                type)
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
            return newStub<ICTypeMonitor_PrimitiveSet>(space, getStubCode(), flags_);
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
          : ICStubCompiler(cx, TypeMonitor_SingleObject, Engine::Baseline),
            obj_(obj)
        { }

        ICTypeMonitor_SingleObject* getStub(ICStubSpace* space) {
            return newStub<ICTypeMonitor_SingleObject>(space, getStubCode(), obj_);
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
          : ICStubCompiler(cx, TypeMonitor_ObjectGroup, Engine::Baseline),
            group_(group)
        { }

        ICTypeMonitor_ObjectGroup* getStub(ICStubSpace* space) {
            return newStub<ICTypeMonitor_ObjectGroup>(space, getStubCode(), group_);
        }
    };
};


// BinaryArith
//      JSOP_ADD, JSOP_SUB, JSOP_MUL, JOP_DIV, JSOP_MOD
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
        explicit Compiler(JSContext* cx, Engine engine)
          : ICStubCompiler(cx, ICStub::BinaryArith_Fallback, engine) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICBinaryArith_Fallback>(space, getStubCode());
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
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(op_) << 17) |
                  (static_cast<int32_t>(allowDouble_) << 25);
        }

      public:
        Compiler(JSContext* cx, JSOp op, Engine engine, bool allowDouble)
          : ICStubCompiler(cx, ICStub::BinaryArith_Int32, engine),
            op_(op), allowDouble_(allowDouble) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICBinaryArith_Int32>(space, getStubCode(), allowDouble_);
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
        explicit Compiler(JSContext* cx, Engine engine)
          : ICStubCompiler(cx, ICStub::BinaryArith_StringConcat, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICBinaryArith_StringConcat>(space, getStubCode());
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
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(lhsIsString_) << 17);
        }

      public:
        Compiler(JSContext* cx, Engine engine, bool lhsIsString)
          : ICStubCompiler(cx, ICStub::BinaryArith_StringObjectConcat, engine),
            lhsIsString_(lhsIsString)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICBinaryArith_StringObjectConcat>(space, getStubCode(),
                                                                 lhsIsString_);
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
        Compiler(JSContext* cx, JSOp op, Engine engine)
          : ICMultiStubCompiler(cx, ICStub::BinaryArith_Double, op, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICBinaryArith_Double>(space, getStubCode());
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
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(op_) << 17) |
                  (static_cast<int32_t>(lhsIsBool_) << 25) |
                  (static_cast<int32_t>(rhsIsBool_) << 26);
        }

      public:
        Compiler(JSContext* cx, JSOp op, Engine engine, bool lhsIsBool, bool rhsIsBool)
          : ICStubCompiler(cx, ICStub::BinaryArith_BooleanWithInt32, engine),
            op_(op), lhsIsBool_(lhsIsBool), rhsIsBool_(rhsIsBool)
        {
            MOZ_ASSERT(op_ == JSOP_ADD || op_ == JSOP_SUB || op_ == JSOP_BITOR ||
                       op_ == JSOP_BITAND || op_ == JSOP_BITXOR);
            MOZ_ASSERT(lhsIsBool_ || rhsIsBool_);
        }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICBinaryArith_BooleanWithInt32>(space, getStubCode(),
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
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(op) << 17) |
                  (static_cast<int32_t>(lhsIsDouble_) << 25);
        }

      public:
        Compiler(JSContext* cx, JSOp op, Engine engine, bool lhsIsDouble)
          : ICMultiStubCompiler(cx, ICStub::BinaryArith_DoubleWithInt32, op, engine),
            lhsIsDouble_(lhsIsDouble)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICBinaryArith_DoubleWithInt32>(space, getStubCode(),
                                                              lhsIsDouble_);
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
        explicit Compiler(JSContext* cx, Engine engine)
          : ICStubCompiler(cx, ICStub::UnaryArith_Fallback, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICUnaryArith_Fallback>(space, getStubCode());
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
        Compiler(JSContext* cx, JSOp op, Engine engine)
          : ICMultiStubCompiler(cx, ICStub::UnaryArith_Int32, op, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICUnaryArith_Int32>(space, getStubCode());
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
        Compiler(JSContext* cx, JSOp op, Engine engine)
          : ICMultiStubCompiler(cx, ICStub::UnaryArith_Double, op, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICUnaryArith_Double>(space, getStubCode());
        }
    };
};

// Compare
//      JSOP_LT
//      JSOP_LE
//      JSOP_GT
//      JSOP_GE
//      JSOP_EQ
//      JSOP_NE
//      JSOP_STRICTEQ
//      JSOP_STRICTNE

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
        explicit Compiler(JSContext* cx, Engine engine)
          : ICStubCompiler(cx, ICStub::Compare_Fallback, engine) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCompare_Fallback>(space, getStubCode());
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
        Compiler(JSContext* cx, JSOp op, Engine engine)
          : ICMultiStubCompiler(cx, ICStub::Compare_Int32, op, engine) {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCompare_Int32>(space, getStubCode());
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
        Compiler(JSContext* cx, JSOp op, Engine engine)
          : ICMultiStubCompiler(cx, ICStub::Compare_Double, op, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCompare_Double>(space, getStubCode());
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
        Compiler(JSContext* cx, JSOp op, Engine engine, bool lhsIsUndefined)
          : ICMultiStubCompiler(cx, ICStub::Compare_NumberWithUndefined, op, engine),
            lhsIsUndefined(lhsIsUndefined)
        {}

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(op) << 17) |
                  (static_cast<int32_t>(lhsIsUndefined) << 25);
        }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCompare_NumberWithUndefined>(space, getStubCode(),
                                                              lhsIsUndefined);
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
        Compiler(JSContext* cx, JSOp op, Engine engine)
          : ICMultiStubCompiler(cx, ICStub::Compare_String, op, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCompare_String>(space, getStubCode());
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
        Compiler(JSContext* cx, JSOp op, Engine engine)
          : ICMultiStubCompiler(cx, ICStub::Compare_Boolean, op, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCompare_Boolean>(space, getStubCode());
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
        Compiler(JSContext* cx, JSOp op, Engine engine)
          : ICMultiStubCompiler(cx, ICStub::Compare_Object, op, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCompare_Object>(space, getStubCode());
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
        Compiler(JSContext* cx, JSOp op, Engine engine, bool lhsIsUndefined, bool compareWithNull)
          : ICMultiStubCompiler(cx, ICStub::Compare_ObjectWithUndefined, op, engine),
            lhsIsUndefined(lhsIsUndefined),
            compareWithNull(compareWithNull)
        {}

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(op) << 17) |
                  (static_cast<int32_t>(lhsIsUndefined) << 25) |
                  (static_cast<int32_t>(compareWithNull) << 26);
        }

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCompare_ObjectWithUndefined>(space, getStubCode());
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
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(op_) << 17) |
                  (static_cast<int32_t>(lhsIsInt32_) << 25);
        }

      public:
        Compiler(JSContext* cx, JSOp op, Engine engine, bool lhsIsInt32)
          : ICStubCompiler(cx, ICStub::Compare_Int32WithBoolean, engine),
            op_(op),
            lhsIsInt32_(lhsIsInt32)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICCompare_Int32WithBoolean>(space, getStubCode(), lhsIsInt32_);
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

bool
IsPreliminaryObject(JSObject* obj);

void
StripPreliminaryObjectStubs(JSContext* cx, ICFallbackStub* stub);

bool
EffectlesslyLookupProperty(JSContext* cx, HandleObject obj, HandleId name,
                           MutableHandleObject holder, MutableHandleShape shape,
                           bool* checkDOMProxy=nullptr,
                           DOMProxyShadowsResult* shadowsResult=nullptr,
                           bool* domProxyHasGeneration=nullptr);

JSObject*
GetDOMProxyProto(JSObject* obj);

bool
IsCacheableProtoChain(JSObject* obj, JSObject* holder, bool isDOMProxy=false);

bool
IsCacheableGetPropReadSlot(JSObject* obj, JSObject* holder, Shape* shape, bool isDOMProxy=false);

void
GetFixedOrDynamicSlotOffset(Shape* shape, bool* isFixed, uint32_t* offset);

bool
IsCacheableGetPropCall(JSContext* cx, JSObject* obj, JSObject* holder, Shape* shape,
                       bool* isScripted, bool* isTemporarilyUnoptimizable, bool isDOMProxy=false);

bool
UpdateExistingGetPropCallStubs(ICFallbackStub* fallbackStub,
                               ICStub::Kind kind,
                               HandleNativeObject holder,
                               HandleObject receiver,
                               HandleFunction getter);
bool
CheckHasNoSuchProperty(JSContext* cx, HandleObject obj, HandlePropertyName name,
                       MutableHandleObject lastProto, size_t* protoChainDepthOut);

void
GuardReceiverObject(MacroAssembler& masm, ReceiverGuard guard,
                    Register object, Register scratch,
                    size_t receiverGuardOffset, Label* failure);

bool
GetProtoShapes(JSObject* obj, size_t protoChainDepth, MutableHandle<ShapeVector> shapes);

void
CheckDOMProxyExpandoDoesNotShadow(JSContext* cx, MacroAssembler& masm, Register object,
                                  const Address& checkExpandoShapeAddr,
                                  Address* expandoAndGenerationAddr,
                                  Address* generationAddr,
                                  Register scratch,
                                  AllocatableGeneralRegisterSet& domProxyRegSet,
                                  Label* checkFailed);

void
CheckForNeuteredTypedObject(JSContext* cx, MacroAssembler& masm, Label* failure);

bool
DoCallNativeGetter(JSContext* cx, HandleFunction callee, HandleObject obj,
                   MutableHandleValue result);

void
LoadTypedThingData(MacroAssembler& masm, TypedThingLayout layout, Register obj, Register result);

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
      public:
        static const int32_t BASELINE_KEY =
            (static_cast<int32_t>(Engine::Baseline)) |
            (static_cast<int32_t>(ICStub::GetProp_Fallback) << 1);

      protected:
        uint32_t returnOffset_;
        bool generateStubCode(MacroAssembler& masm);
        void postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code);

      public:
        explicit Compiler(JSContext* cx, Engine engine)
          : ICStubCompiler(cx, ICStub::GetProp_Fallback, engine)
        { }

        ICStub* getStub(ICStubSpace* space) {
            ICGetProp_Fallback* stub = newStub<ICGetProp_Fallback>(space, getStubCode());
            if (!stub || !stub->initMonitoringChain(cx, space, engine_))
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
    static ICGetProp_Generic* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                    ICGetProp_Generic& other);

    class Compiler : public ICStubCompiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);
        ICStub* firstMonitorStub_;
      public:
        explicit Compiler(JSContext* cx, Engine engine, ICStub* firstMonitorStub)
          : ICStubCompiler(cx, ICStub::GetProp_Generic, engine),
            firstMonitorStub_(firstMonitorStub)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetProp_Generic>(space, getStubCode(), firstMonitorStub_);
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
        explicit Compiler(JSContext* cx, Engine engine)
          : ICStubCompiler(cx, ICStub::GetProp_ArrayLength, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetProp_ArrayLength>(space, getStubCode());
        }
    };
};

// Stub for accessing an unboxed array's length.
class ICGetProp_UnboxedArrayLength : public ICStub
{
    friend class ICStubSpace;

    explicit ICGetProp_UnboxedArrayLength(JitCode* stubCode)
      : ICStub(GetProp_UnboxedArrayLength, stubCode)
    {}

  public:
    class Compiler : public ICStubCompiler {
        bool generateStubCode(MacroAssembler& masm);

      public:
        explicit Compiler(JSContext* cx, Engine engine)
          : ICStubCompiler(cx, ICStub::GetProp_UnboxedArrayLength, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetProp_UnboxedArrayLength>(space, getStubCode());
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

    ICGetProp_Primitive(JitCode* stubCode, ICStub* firstMonitorStub, JSValueType primitiveType,
                        Shape* protoShape, uint32_t offset);

  public:
    HeapPtrShape& protoShape() {
        return protoShape_;
    }
    JSValueType primitiveType() const {
        return JSValueType(extra_);
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
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(isFixedSlot_) << 17) |
                  (static_cast<int32_t>(primitiveType_) << 25);
        }

      public:
        Compiler(JSContext* cx, Engine engine, ICStub* firstMonitorStub, JSValueType primitiveType,
                 HandleObject prototype, bool isFixedSlot, uint32_t offset)
          : ICStubCompiler(cx, ICStub::GetProp_Primitive, engine),
            firstMonitorStub_(firstMonitorStub),
            primitiveType_(primitiveType),
            prototype_(cx, prototype),
            isFixedSlot_(isFixedSlot),
            offset_(offset)
        {}

        ICStub* getStub(ICStubSpace* space) {
            RootedShape protoShape(cx, prototype_->as<NativeObject>().lastProperty());
            return newStub<ICGetProp_Primitive>(space, getStubCode(), firstMonitorStub_,
                                                primitiveType_, protoShape, offset_);
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
        explicit Compiler(JSContext* cx, Engine engine)
          : ICStubCompiler(cx, ICStub::GetProp_StringLength, engine)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetProp_StringLength>(space, getStubCode());
        }
    };
};

// Base class for native GetProp stubs.
class ICGetPropNativeStub : public ICMonitoredStub
{
    // Object shape/group.
    HeapReceiverGuard receiverGuard_;

    // Fixed or dynamic slot offset.
    uint32_t offset_;

  protected:
    ICGetPropNativeStub(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                        ReceiverGuard guard, uint32_t offset);

  public:
    HeapReceiverGuard& receiverGuard() {
        return receiverGuard_;
    }
    uint32_t offset() const {
        return offset_;
    }

    void notePreliminaryObject() {
        extra_ = 1;
    }
    bool hasPreliminaryObject() const {
        return extra_;
    }

    static size_t offsetOfReceiverGuard() {
        return offsetof(ICGetPropNativeStub, receiverGuard_);
    }
    static size_t offsetOfOffset() {
        return offsetof(ICGetPropNativeStub, offset_);
    }
};

// Stub for accessing an own property on a native object.
class ICGetProp_Native : public ICGetPropNativeStub
{
    friend class ICStubSpace;

    ICGetProp_Native(JitCode* stubCode, ICStub* firstMonitorStub, ReceiverGuard guard,
                     uint32_t offset)
      : ICGetPropNativeStub(GetProp_Native, stubCode, firstMonitorStub, guard, offset)
    {}

  public:
    static ICGetProp_Native* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                   ICGetProp_Native& other);
};

class ICGetPropNativePrototypeStub : public ICGetPropNativeStub
{
    // Holder and its shape.
    HeapPtrObject holder_;
    HeapPtrShape holderShape_;

  protected:
    ICGetPropNativePrototypeStub(ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                                 ReceiverGuard guard, uint32_t offset, JSObject* holder,
                                 Shape* holderShape);

  public:
    HeapPtrObject& holder() {
        return holder_;
    }
    HeapPtrShape& holderShape() {
        return holderShape_;
    }
    static size_t offsetOfHolder() {
        return offsetof(ICGetPropNativePrototypeStub, holder_);
    }
    static size_t offsetOfHolderShape() {
        return offsetof(ICGetPropNativePrototypeStub, holderShape_);
    }
};

// Stub for accessing a property on the native prototype of a native or unboxed
// object. Note that due to the shape teleporting optimization, we only have to
// guard on the object's shape/group and the holder's shape.
class ICGetProp_NativePrototype : public ICGetPropNativePrototypeStub
{
    friend class ICStubSpace;

  protected:
    ICGetProp_NativePrototype(JitCode* stubCode, ICStub* firstMonitorStub, ReceiverGuard guard,
                              uint32_t offset, JSObject* holder, Shape* holderShape)
      : ICGetPropNativePrototypeStub(GetProp_NativePrototype, stubCode, firstMonitorStub, guard,
                                     offset, holder, holderShape)
    { }

  public:
    static ICGetProp_NativePrototype* Clone(JSContext* cx,
                                            ICStubSpace* space,
                                            ICStub* firstMonitorStub,
                                            ICGetProp_NativePrototype& other);
};

// Stub for accessing a non-lexical global name. Semantically, it is really a
// getprop: the name is either on the GlobalObject or its prototype chain. We
// teleport to the object that has the name, but we also need to guard on the
// shape of the global object.
//
// The receiver object is the global lexical scope.
class ICGetName_Global : public ICGetPropNativePrototypeStub
{
    friend class ICStubSpace;

  protected:
    HeapPtrShape globalShape_;

    ICGetName_Global(JitCode* stubCode, ICStub* firstMonitorStub, ReceiverGuard guard,
                     uint32_t slot, JSObject* holder, Shape* holderShape, Shape* globalShape);

  public:
    static ICGetName_Global* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                   ICGetName_Global& other);

    HeapPtrShape& globalShape() {
        return globalShape_;
    }
    static size_t offsetOfGlobalShape() {
        return offsetof(ICGetName_Global, globalShape_);
    }
};

// Compiler for native GetProp stubs.
class ICGetPropNativeCompiler : public ICStubCompiler
{
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
        return static_cast<int32_t>(engine_) |
              (static_cast<int32_t>(kind) << 1) |
              (static_cast<int32_t>(isFixedSlot_) << 17) |
              (static_cast<int32_t>(inputDefinitelyObject_) << 18) |
              (HeapReceiverGuard::keyBits(obj_) << 19);
    }

  public:
    ICGetPropNativeCompiler(JSContext* cx, ICStub::Kind kind, ICStubCompiler::Engine engine,
                            ICStub* firstMonitorStub, HandleObject obj, HandleObject holder,
                            HandlePropertyName propName, bool isFixedSlot, uint32_t offset,
                            bool inputDefinitelyObject = false)
      : ICStubCompiler(cx, kind, engine),
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
    HeapReceiverGuard guard_;

    static const size_t MAX_PROTO_CHAIN_DEPTH = 8;

  protected:
    ICGetProp_NativeDoesNotExist(JitCode* stubCode, ICStub* firstMonitorStub,
                                 ReceiverGuard guard,
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

    HeapReceiverGuard& guard() {
        return guard_;
    }

    static size_t offsetOfGuard() {
        return offsetof(ICGetProp_NativeDoesNotExist, guard_);
    }

    static size_t offsetOfShape(size_t idx);
};

template <size_t ProtoChainDepth>
class ICGetProp_NativeDoesNotExistImpl : public ICGetProp_NativeDoesNotExist
{
    friend class ICStubSpace;
  public:
    static const size_t MAX_PROTO_CHAIN_DEPTH = 8;
    static const size_t NumShapes = ProtoChainDepth;

  private:
    mozilla::Array<HeapPtrShape, NumShapes> shapes_;

    ICGetProp_NativeDoesNotExistImpl(JitCode* stubCode, ICStub* firstMonitorStub,
                                     ReceiverGuard guard,
                                     Handle<ShapeVector> shapes);

  public:
    void traceShapes(JSTracer* trc) {
        // Note: using int32_t here to avoid gcc warning.
        for (int32_t i = 0; i < int32_t(NumShapes); i++)
            TraceEdge(trc, &shapes_[i], "baseline-getpropnativedoesnotexist-stub-shape");
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
        return static_cast<int32_t>(engine_) |
              (static_cast<int32_t>(kind) << 1) |
              (HeapReceiverGuard::keyBits(obj_) << 17) |
              (static_cast<int32_t>(protoChainDepth_) << 19);
    }

    bool generateStubCode(MacroAssembler& masm);

  public:
    ICGetPropNativeDoesNotExistCompiler(JSContext* cx, ICStubCompiler::Engine engine,
                                        ICStub* firstMonitorStub, HandleObject obj,
                                        size_t protoChainDepth);

    template <size_t ProtoChainDepth>
    ICStub* getStubSpecific(ICStubSpace* space, Handle<ShapeVector> shapes) {
        ReceiverGuard guard(obj_);
        return newStub<ICGetProp_NativeDoesNotExistImpl<ProtoChainDepth>>
            (space, getStubCode(), firstMonitorStub_, guard, shapes);
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
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(fieldType_) << 17);
        }

      public:
        Compiler(JSContext* cx, Engine engine, ICStub* firstMonitorStub,
                 ObjectGroup* group, uint32_t fieldOffset, JSValueType fieldType)
          : ICStubCompiler(cx, ICStub::GetProp_Unboxed, engine),
            firstMonitorStub_(firstMonitorStub),
            group_(cx, group),
            fieldOffset_(fieldOffset),
            fieldType_(fieldType)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetProp_Unboxed>(space, getStubCode(), firstMonitorStub_, group_,
                                              fieldOffset_);
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
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(SimpleTypeDescrKey(fieldDescr_)) << 17) |
                  (static_cast<int32_t>(layout_) << 25);
        }

      public:
        Compiler(JSContext* cx, Engine engine, ICStub* firstMonitorStub,
                 Shape* shape, uint32_t fieldOffset, SimpleTypeDescr* fieldDescr)
          : ICStubCompiler(cx, ICStub::GetProp_TypedObject, engine),
            firstMonitorStub_(firstMonitorStub),
            shape_(cx, shape),
            fieldOffset_(fieldOffset),
            layout_(GetTypedThingLayout(shape->getObjectClass())),
            fieldDescr_(cx, fieldDescr)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetProp_TypedObject>(space, getStubCode(), firstMonitorStub_, shape_,
                                                  fieldOffset_);
        }
    };
};

class ICGetProp_ModuleNamespace : public ICMonitoredStub
{
    friend class ICStubSpace;

    HeapPtrModuleNamespaceObject namespace_;
    HeapPtrModuleEnvironmentObject environment_;
    uint32_t offset_;

    ICGetProp_ModuleNamespace(JitCode* stubCode, ICStub* firstMonitorStub,
                              HandleModuleNamespaceObject ns, HandleModuleEnvironmentObject env,
                              uint32_t offset)
      : ICMonitoredStub(ICStub::GetProp_ModuleNamespace, stubCode, firstMonitorStub),
        namespace_(ns), environment_(env), offset_(offset)
    {
        (void) offset_; // Silence clang warning.
    }

  public:
    HeapPtrModuleNamespaceObject& getNamespace() {
        return namespace_;
    }
    HeapPtrModuleEnvironmentObject& environment() {
        return environment_;
    }

    static size_t offsetOfNamespace() {
        return offsetof(ICGetProp_ModuleNamespace, namespace_);
    }
    static size_t offsetOfEnvironment() {
        return offsetof(ICGetProp_ModuleNamespace, environment_);
    }
    static size_t offsetOfOffset() {
        return offsetof(ICGetProp_ModuleNamespace, offset_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        RootedModuleNamespaceObject namespace_;
        RootedModuleEnvironmentObject environment_;
        bool isFixedSlot_;
        uint32_t offset_;

        bool generateStubCode(MacroAssembler& masm);

        virtual int32_t getKey() const {
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(isFixedSlot_) << 17);
        }

      public:
        Compiler(JSContext* cx, Engine engine, ICStub* firstMonitorStub,
                 HandleModuleNamespaceObject ns, HandleModuleEnvironmentObject env, bool isFixedSlot,
                 uint32_t offset)
          : ICStubCompiler(cx, ICStub::GetProp_ModuleNamespace, engine),
            firstMonitorStub_(firstMonitorStub),
            namespace_(cx, ns), environment_(cx, env), isFixedSlot_(isFixedSlot), offset_(offset)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetProp_ModuleNamespace>(space, getStubCode(), firstMonitorStub_,
                                                      namespace_, environment_, offset_);
        }
    };
};

class ICGetPropCallGetter : public ICMonitoredStub
{
    friend class ICStubSpace;

  protected:
    // Shape/group of receiver object. Used for both own and proto getters.
    // In the GetPropCallDOMProxyNative case, the receiver guard enforces
    // the proxy handler, because Shape implies Class.
    HeapReceiverGuard receiverGuard_;

    // Holder and holder shape. For own getters, guarding on receiverGuard_ is
    // sufficient, although Ion may use holder_ and holderShape_ even for own
    // getters. In this case holderShape_ == receiverGuard_.shape_ (isOwnGetter
    // below relies on this).
    HeapPtrObject holder_;

    HeapPtrShape holderShape_;

    // Function to call.
    HeapPtrFunction getter_;

    // PC offset of call
    uint32_t pcOffset_;

    ICGetPropCallGetter(Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                        ReceiverGuard receiverGuard, JSObject* holder,
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
    HeapReceiverGuard& receiverGuard() {
        return receiverGuard_;
    }

    bool isOwnGetter() const {
        MOZ_ASSERT(holder_->isNative());
        MOZ_ASSERT(holderShape_);
        return receiverGuard_.shape() == holderShape_;
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
    static size_t offsetOfReceiverGuard() {
        return offsetof(ICGetPropCallGetter, receiverGuard_);
    }

    class Compiler : public ICStubCompiler {
      protected:
        ICStub* firstMonitorStub_;
        RootedObject receiver_;
        RootedObject holder_;
        RootedFunction getter_;
        uint32_t pcOffset_;
        const Class* outerClass_;

        virtual int32_t getKey() const {
            // ICGetPropCallNativeCompiler::getKey adds more bits to our
            // return value, so be careful when making changes here.
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (HeapReceiverGuard::keyBits(receiver_) << 17) |
                  (static_cast<int32_t>(!!outerClass_) << 19) |
                  (static_cast<int32_t>(receiver_ != holder_) << 20);
        }

      public:
        Compiler(JSContext* cx, ICStub::Kind kind, Engine engine, ICStub* firstMonitorStub,
                 HandleObject receiver, HandleObject holder, HandleFunction getter,
                 uint32_t pcOffset, const Class* outerClass)
          : ICStubCompiler(cx, kind, engine),
            firstMonitorStub_(firstMonitorStub),
            receiver_(cx, receiver),
            holder_(cx, holder),
            getter_(cx, getter),
            pcOffset_(pcOffset),
            outerClass_(outerClass)
        {
            MOZ_ASSERT(kind == ICStub::GetProp_CallScripted ||
                       kind == ICStub::GetProp_CallNative ||
                       kind == ICStub::GetProp_CallNativeGlobal);
        }
    };
};

// Stub for calling a scripted getter on a native object when the getter is kept on the
// proto-chain.
class ICGetProp_CallScripted : public ICGetPropCallGetter
{
    friend class ICStubSpace;

  protected:
    ICGetProp_CallScripted(JitCode* stubCode, ICStub* firstMonitorStub,
                           ReceiverGuard receiverGuard,
                           JSObject* holder, Shape* holderShape,
                           JSFunction* getter, uint32_t pcOffset)
      : ICGetPropCallGetter(GetProp_CallScripted, stubCode, firstMonitorStub,
                            receiverGuard, holder, holderShape, getter, pcOffset)
    {}

  public:
    static ICGetProp_CallScripted* Clone(JSContext* cx, ICStubSpace* space,
                                         ICStub* firstMonitorStub, ICGetProp_CallScripted& other);

    class Compiler : public ICGetPropCallGetter::Compiler {
      protected:
        bool generateStubCode(MacroAssembler& masm);

      public:
        Compiler(JSContext* cx, ICStub* firstMonitorStub, HandleObject obj,
                 HandleObject holder, HandleFunction getter, uint32_t pcOffset)
          : ICGetPropCallGetter::Compiler(cx, ICStub::GetProp_CallScripted, Engine::Baseline,
                                          firstMonitorStub, obj, holder,
                                          getter, pcOffset, /* outerClass = */ nullptr)
        {}

        ICStub* getStub(ICStubSpace* space) {
            ReceiverGuard guard(receiver_);
            Shape* holderShape = holder_->as<NativeObject>().lastProperty();
            return newStub<ICGetProp_CallScripted>(space, getStubCode(), firstMonitorStub_,
                                                       guard, holder_, holderShape, getter_,
                                                       pcOffset_);
        }
    };
};

// Stub for calling a native getter on a native object.
class ICGetProp_CallNative : public ICGetPropCallGetter
{
    friend class ICStubSpace;

  protected:

    ICGetProp_CallNative(JitCode* stubCode, ICStub* firstMonitorStub,
                         ReceiverGuard receiverGuard,
                         JSObject* holder, Shape* holderShape,
                         JSFunction* getter, uint32_t pcOffset)
      : ICGetPropCallGetter(GetProp_CallNative, stubCode, firstMonitorStub,
                            receiverGuard, holder, holderShape, getter, pcOffset)
    {}

  public:
    static ICGetProp_CallNative* Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                       ICGetProp_CallNative& other);

};

// Stub for calling a native getter on the GlobalObject.
class ICGetProp_CallNativeGlobal : public ICGetPropCallGetter
{
    friend class ICStubSpace;

  protected:
    HeapPtrShape globalShape_;

    ICGetProp_CallNativeGlobal(JitCode* stubCode, ICStub* firstMonitorStub,
                               ReceiverGuard receiverGuard,
                               JSObject* holder, Shape* holderShape, Shape* globalShape,
                               JSFunction* getter, uint32_t pcOffset)
      : ICGetPropCallGetter(GetProp_CallNativeGlobal, stubCode, firstMonitorStub,
                            receiverGuard, holder, holderShape, getter, pcOffset),
        globalShape_(globalShape)
    { }

  public:
    static ICGetProp_CallNativeGlobal* Clone(JSContext* cx, ICStubSpace* space,
                                             ICStub* firstMonitorStub,
                                             ICGetProp_CallNativeGlobal& other);

    HeapPtrShape& globalShape() {
        return globalShape_;
    }
    static size_t offsetOfGlobalShape() {
        return offsetof(ICGetProp_CallNativeGlobal, globalShape_);
    }
};

class ICGetPropCallNativeCompiler : public ICGetPropCallGetter::Compiler
{
    bool inputDefinitelyObject_;
  protected:
    bool generateStubCode(MacroAssembler& masm);

    virtual int32_t getKey() const {
        int32_t baseKey = ICGetPropCallGetter::Compiler::getKey();
        MOZ_ASSERT((baseKey >> 21) == 0);
        return baseKey | (static_cast<int32_t>(inputDefinitelyObject_) << 21);
    }

  public:
    ICGetPropCallNativeCompiler(JSContext* cx, ICStub::Kind kind, ICStubCompiler::Engine engine,
                                ICStub* firstMonitorStub, HandleObject receiver,
                                HandleObject holder, HandleFunction getter, uint32_t pcOffset,
                                const Class* outerClass, bool inputDefinitelyObject = false)
      : ICGetPropCallGetter::Compiler(cx, kind, engine, firstMonitorStub, receiver, holder,
                                      getter, pcOffset, outerClass),
        inputDefinitelyObject_(inputDefinitelyObject)
    {}

    ICStub* getStub(ICStubSpace* space);
};

class ICGetPropCallDOMProxyNativeStub : public ICGetPropCallGetter
{
  friend class ICStubSpace;
  protected:
    // Object shape of expected expando object. (nullptr if no expando object should be there)
    HeapPtrShape expandoShape_;

    ICGetPropCallDOMProxyNativeStub(ICStub::Kind kind, JitCode* stubCode,
                                    ICStub* firstMonitorStub, Shape* shape,
                                    Shape* expandoShape,
                                    JSObject* holder, Shape* holderShape,
                                    JSFunction* getter, uint32_t pcOffset);

  public:
    HeapPtrShape& expandoShape() {
        return expandoShape_;
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
    static ICGetProp_CallDOMProxyNative* Clone(JSContext* cx,
                                               ICStubSpace* space,
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
    Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
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
                                        ICStubCompiler::Engine engine,
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
    static ICGetProp_DOMProxyShadowed* Clone(JSContext* cx, ICStubSpace* space,
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
        Compiler(JSContext* cx, Engine engine, ICStub* firstMonitorStub, Handle<ProxyObject*> proxy,
                 HandlePropertyName name, uint32_t pcOffset)
          : ICStubCompiler(cx, ICStub::GetProp_CallNative, engine),
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
    enum Which { Mapped, Unmapped, Magic };

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
            return static_cast<int32_t>(engine_) |
                  (static_cast<int32_t>(kind) << 1) |
                  (static_cast<int32_t>(which_) << 17);
        }

      public:
        Compiler(JSContext* cx, Engine engine, Which which)
          : ICStubCompiler(cx, ICStub::GetProp_ArgumentsLength, engine),
            which_(which)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetProp_ArgumentsLength>(space, getStubCode());
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
        Compiler(JSContext* cx, Engine engine, ICStub* firstMonitorStub)
          : ICStubCompiler(cx, ICStub::GetProp_ArgumentsCallee, engine),
            firstMonitorStub_(firstMonitorStub)
        {}

        ICStub* getStub(ICStubSpace* space) {
            return newStub<ICGetProp_ArgumentsCallee>(space, getStubCode(), firstMonitorStub_);
        }
    };
};

} // namespace jit
} // namespace js

#endif /* jit_SharedIC_h */
