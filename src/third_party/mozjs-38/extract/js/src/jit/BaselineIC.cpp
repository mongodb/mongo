/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineIC.h"

#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/TemplateLib.h"

#include "jslibmath.h"
#include "jstypes.h"

#include "builtin/Eval.h"
#include "builtin/SIMD.h"
#include "jit/BaselineDebugModeOSR.h"
#include "jit/BaselineHelpers.h"
#include "jit/BaselineJIT.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#include "jit/Lowering.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/VMFunctions.h"
#include "js/Conversions.h"
#include "vm/Opcodes.h"
#include "vm/TypedArrayCommon.h"

#include "jsboolinlines.h"
#include "jsscriptinlines.h"

#include "jit/JitFrames-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/ScopeObject-inl.h"
#include "vm/StringObject-inl.h"

using mozilla::BitwiseCast;
using mozilla::DebugOnly;

namespace js {
namespace jit {

#ifdef DEBUG
void
FallbackICSpew(JSContext* cx, ICFallbackStub* stub, const char* fmt, ...)
{
    if (JitSpewEnabled(JitSpew_BaselineICFallback)) {
        RootedScript script(cx, GetTopJitJSScript(cx));
        jsbytecode* pc = stub->icEntry()->pc(script);

        char fmtbuf[100];
        va_list args;
        va_start(args, fmt);
        vsnprintf(fmtbuf, 100, fmt, args);
        va_end(args);

        JitSpew(JitSpew_BaselineICFallback,
                "Fallback hit for (%s:%d) (pc=%d,line=%d,uses=%d,stubs=%d): %s",
                script->filename(),
                script->lineno(),
                (int) script->pcToOffset(pc),
                PCToLineNumber(script, pc),
                script->getWarmUpCount(),
                (int) stub->numOptimizedStubs(),
                fmtbuf);
    }
}

void
TypeFallbackICSpew(JSContext* cx, ICTypeMonitor_Fallback* stub, const char* fmt, ...)
{
    if (JitSpewEnabled(JitSpew_BaselineICFallback)) {
        RootedScript script(cx, GetTopJitJSScript(cx));
        jsbytecode* pc = stub->icEntry()->pc(script);

        char fmtbuf[100];
        va_list args;
        va_start(args, fmt);
        vsnprintf(fmtbuf, 100, fmt, args);
        va_end(args);

        JitSpew(JitSpew_BaselineICFallback,
                "Type monitor fallback hit for (%s:%d) (pc=%d,line=%d,uses=%d,stubs=%d): %s",
                script->filename(),
                script->lineno(),
                (int) script->pcToOffset(pc),
                PCToLineNumber(script, pc),
                script->getWarmUpCount(),
                (int) stub->numOptimizedMonitorStubs(),
                fmtbuf);
    }
}

#else
#define FallbackICSpew(...)
#define TypeFallbackICSpew(...)
#endif


ICFallbackStub*
ICEntry::fallbackStub() const
{
    return firstStub()->getChainFallback();
}


ICStubConstIterator&
ICStubConstIterator::operator++()
{
    MOZ_ASSERT(currentStub_ != nullptr);
    currentStub_ = currentStub_->next();
    return *this;
}


ICStubIterator::ICStubIterator(ICFallbackStub* fallbackStub, bool end)
  : icEntry_(fallbackStub->icEntry()),
    fallbackStub_(fallbackStub),
    previousStub_(nullptr),
    currentStub_(end ? fallbackStub : icEntry_->firstStub()),
    unlinked_(false)
{ }

ICStubIterator&
ICStubIterator::operator++()
{
    MOZ_ASSERT(currentStub_->next() != nullptr);
    if (!unlinked_)
        previousStub_ = currentStub_;
    currentStub_ = currentStub_->next();
    unlinked_ = false;
    return *this;
}

void
ICStubIterator::unlink(JSContext* cx)
{
    MOZ_ASSERT(currentStub_->next() != nullptr);
    MOZ_ASSERT(currentStub_ != fallbackStub_);
    MOZ_ASSERT(!unlinked_);

    fallbackStub_->unlinkStub(cx->zone(), previousStub_, currentStub_);

    // Mark the current iterator position as unlinked, so operator++ works properly.
    unlinked_ = true;
}


void
ICStub::markCode(JSTracer* trc, const char* name)
{
    JitCode* stubJitCode = jitCode();
    MarkJitCodeUnbarriered(trc, &stubJitCode, name);
}

void
ICStub::updateCode(JitCode* code)
{
    // Write barrier on the old code.
    JitCode::writeBarrierPre(jitCode());
    stubCode_ = code->raw();
}

/* static */ void
ICStub::trace(JSTracer* trc)
{
    markCode(trc, "baseline-stub-jitcode");

    // If the stub is a monitored fallback stub, then mark the monitor ICs hanging
    // off of that stub.  We don't need to worry about the regular monitored stubs,
    // because the regular monitored stubs will always have a monitored fallback stub
    // that references the same stub chain.
    if (isMonitoredFallback()) {
        ICTypeMonitor_Fallback* lastMonStub = toMonitoredFallbackStub()->fallbackMonitorStub();
        for (ICStubConstIterator iter(lastMonStub->firstMonitorStub()); !iter.atEnd(); iter++) {
            MOZ_ASSERT_IF(iter->next() == nullptr, *iter == lastMonStub);
            iter->trace(trc);
        }
    }

    if (isUpdated()) {
        for (ICStubConstIterator iter(toUpdatedStub()->firstUpdateStub()); !iter.atEnd(); iter++) {
            MOZ_ASSERT_IF(iter->next() == nullptr, iter->isTypeUpdate_Fallback());
            iter->trace(trc);
        }
    }

    switch (kind()) {
      case ICStub::Call_Scripted: {
        ICCall_Scripted* callStub = toCall_Scripted();
        MarkObject(trc, &callStub->callee(), "baseline-callscripted-callee");
        if (callStub->templateObject())
            MarkObject(trc, &callStub->templateObject(), "baseline-callscripted-template");
        break;
      }
      case ICStub::Call_Native: {
        ICCall_Native* callStub = toCall_Native();
        MarkObject(trc, &callStub->callee(), "baseline-callnative-callee");
        if (callStub->templateObject())
            MarkObject(trc, &callStub->templateObject(), "baseline-callnative-template");
        break;
      }
      case ICStub::Call_ClassHook: {
        ICCall_ClassHook* callStub = toCall_ClassHook();
        if (callStub->templateObject())
            MarkObject(trc, &callStub->templateObject(), "baseline-callclasshook-template");
        break;
      }
      case ICStub::Call_StringSplit: {
        ICCall_StringSplit* callStub = toCall_StringSplit();
        MarkObject(trc, &callStub->templateObject(), "baseline-callstringsplit-template");
        MarkString(trc, &callStub->expectedArg(), "baseline-callstringsplit-arg");
        MarkString(trc, &callStub->expectedThis(), "baseline-callstringsplit-this");
        break;
      }
      case ICStub::GetElem_NativeSlot: {
        ICGetElem_NativeSlot* getElemStub = toGetElem_NativeSlot();
        MarkShape(trc, &getElemStub->shape(), "baseline-getelem-native-shape");
        MarkString(trc, &getElemStub->name(), "baseline-getelem-native-name");
        break;
      }
      case ICStub::GetElem_NativePrototypeSlot: {
        ICGetElem_NativePrototypeSlot* getElemStub = toGetElem_NativePrototypeSlot();
        MarkShape(trc, &getElemStub->shape(), "baseline-getelem-nativeproto-shape");
        MarkString(trc, &getElemStub->name(), "baseline-getelem-nativeproto-name");
        MarkObject(trc, &getElemStub->holder(), "baseline-getelem-nativeproto-holder");
        MarkShape(trc, &getElemStub->holderShape(), "baseline-getelem-nativeproto-holdershape");
        break;
      }
      case ICStub::GetElem_NativePrototypeCallNative:
      case ICStub::GetElem_NativePrototypeCallScripted: {
        ICGetElemNativePrototypeCallStub* callStub =
            reinterpret_cast<ICGetElemNativePrototypeCallStub*>(this);
        MarkShape(trc, &callStub->shape(), "baseline-getelem-nativeprotocall-shape");
        MarkString(trc, &callStub->name(), "baseline-getelem-nativeprotocall-name");
        MarkObject(trc, &callStub->getter(), "baseline-getelem-nativeprotocall-getter");
        MarkObject(trc, &callStub->holder(), "baseline-getelem-nativeprotocall-holder");
        MarkShape(trc, &callStub->holderShape(), "baseline-getelem-nativeprotocall-holdershape");
        break;
      }
      case ICStub::GetElem_Dense: {
        ICGetElem_Dense* getElemStub = toGetElem_Dense();
        MarkShape(trc, &getElemStub->shape(), "baseline-getelem-dense-shape");
        break;
      }
      case ICStub::GetElem_TypedArray: {
        ICGetElem_TypedArray* getElemStub = toGetElem_TypedArray();
        MarkShape(trc, &getElemStub->shape(), "baseline-getelem-typedarray-shape");
        break;
      }
      case ICStub::SetElem_Dense: {
        ICSetElem_Dense* setElemStub = toSetElem_Dense();
        MarkShape(trc, &setElemStub->shape(), "baseline-getelem-dense-shape");
        MarkObjectGroup(trc, &setElemStub->group(), "baseline-setelem-dense-group");
        break;
      }
      case ICStub::SetElem_DenseAdd: {
        ICSetElem_DenseAdd* setElemStub = toSetElem_DenseAdd();
        MarkObjectGroup(trc, &setElemStub->group(), "baseline-setelem-denseadd-group");

        JS_STATIC_ASSERT(ICSetElem_DenseAdd::MAX_PROTO_CHAIN_DEPTH == 4);

        switch (setElemStub->protoChainDepth()) {
          case 0: setElemStub->toImpl<0>()->traceShapes(trc); break;
          case 1: setElemStub->toImpl<1>()->traceShapes(trc); break;
          case 2: setElemStub->toImpl<2>()->traceShapes(trc); break;
          case 3: setElemStub->toImpl<3>()->traceShapes(trc); break;
          case 4: setElemStub->toImpl<4>()->traceShapes(trc); break;
          default: MOZ_CRASH("Invalid proto stub.");
        }
        break;
      }
      case ICStub::SetElem_TypedArray: {
        ICSetElem_TypedArray* setElemStub = toSetElem_TypedArray();
        MarkShape(trc, &setElemStub->shape(), "baseline-setelem-typedarray-shape");
        break;
      }
      case ICStub::TypeMonitor_SingleObject: {
        ICTypeMonitor_SingleObject* monitorStub = toTypeMonitor_SingleObject();
        MarkObject(trc, &monitorStub->object(), "baseline-monitor-singleton");
        break;
      }
      case ICStub::TypeMonitor_ObjectGroup: {
        ICTypeMonitor_ObjectGroup* monitorStub = toTypeMonitor_ObjectGroup();
        MarkObjectGroup(trc, &monitorStub->group(), "baseline-monitor-group");
        break;
      }
      case ICStub::TypeUpdate_SingleObject: {
        ICTypeUpdate_SingleObject* updateStub = toTypeUpdate_SingleObject();
        MarkObject(trc, &updateStub->object(), "baseline-update-singleton");
        break;
      }
      case ICStub::TypeUpdate_ObjectGroup: {
        ICTypeUpdate_ObjectGroup* updateStub = toTypeUpdate_ObjectGroup();
        MarkObjectGroup(trc, &updateStub->group(), "baseline-update-group");
        break;
      }
      case ICStub::GetName_Global: {
        ICGetName_Global* globalStub = toGetName_Global();
        MarkShape(trc, &globalStub->shape(), "baseline-global-stub-shape");
        break;
      }
      case ICStub::GetName_Scope0:
        static_cast<ICGetName_Scope<0>*>(this)->traceScopes(trc);
        break;
      case ICStub::GetName_Scope1:
        static_cast<ICGetName_Scope<1>*>(this)->traceScopes(trc);
        break;
      case ICStub::GetName_Scope2:
        static_cast<ICGetName_Scope<2>*>(this)->traceScopes(trc);
        break;
      case ICStub::GetName_Scope3:
        static_cast<ICGetName_Scope<3>*>(this)->traceScopes(trc);
        break;
      case ICStub::GetName_Scope4:
        static_cast<ICGetName_Scope<4>*>(this)->traceScopes(trc);
        break;
      case ICStub::GetName_Scope5:
        static_cast<ICGetName_Scope<5>*>(this)->traceScopes(trc);
        break;
      case ICStub::GetName_Scope6:
        static_cast<ICGetName_Scope<6>*>(this)->traceScopes(trc);
        break;
      case ICStub::GetIntrinsic_Constant: {
        ICGetIntrinsic_Constant* constantStub = toGetIntrinsic_Constant();
        gc::MarkValue(trc, &constantStub->value(), "baseline-getintrinsic-constant-value");
        break;
      }
      case ICStub::GetProp_Primitive: {
        ICGetProp_Primitive* propStub = toGetProp_Primitive();
        MarkShape(trc, &propStub->protoShape(), "baseline-getprop-primitive-stub-shape");
        break;
      }
      case ICStub::GetProp_Native: {
        ICGetProp_Native* propStub = toGetProp_Native();
        MarkShape(trc, &propStub->shape(), "baseline-getpropnative-stub-shape");
        break;
      }
      case ICStub::GetProp_NativePrototype: {
        ICGetProp_NativePrototype* propStub = toGetProp_NativePrototype();
        MarkShape(trc, &propStub->shape(), "baseline-getpropnativeproto-stub-shape");
        MarkObject(trc, &propStub->holder(), "baseline-getpropnativeproto-stub-holder");
        MarkShape(trc, &propStub->holderShape(), "baseline-getpropnativeproto-stub-holdershape");
        break;
      }
      case ICStub::GetProp_UnboxedPrototype: {
        ICGetProp_UnboxedPrototype* propStub = toGetProp_UnboxedPrototype();
        MarkObjectGroup(trc, &propStub->group(), "baseline-getpropnativeproto-stub-group");
        MarkObject(trc, &propStub->holder(), "baseline-getpropnativeproto-stub-holder");
        MarkShape(trc, &propStub->holderShape(), "baseline-getpropnativeproto-stub-holdershape");
        break;
      }
      case ICStub::GetProp_NativeDoesNotExist: {
        ICGetProp_NativeDoesNotExist* propStub = toGetProp_NativeDoesNotExist();
        JS_STATIC_ASSERT(ICGetProp_NativeDoesNotExist::MAX_PROTO_CHAIN_DEPTH == 8);
        switch (propStub->protoChainDepth()) {
          case 0: propStub->toImpl<0>()->traceShapes(trc); break;
          case 1: propStub->toImpl<1>()->traceShapes(trc); break;
          case 2: propStub->toImpl<2>()->traceShapes(trc); break;
          case 3: propStub->toImpl<3>()->traceShapes(trc); break;
          case 4: propStub->toImpl<4>()->traceShapes(trc); break;
          case 5: propStub->toImpl<5>()->traceShapes(trc); break;
          case 6: propStub->toImpl<6>()->traceShapes(trc); break;
          case 7: propStub->toImpl<7>()->traceShapes(trc); break;
          case 8: propStub->toImpl<8>()->traceShapes(trc); break;
          default: MOZ_CRASH("Invalid proto stub.");
        }
        break;
      }
      case ICStub::GetProp_Unboxed: {
        ICGetProp_Unboxed* propStub = toGetProp_Unboxed();
        MarkObjectGroup(trc, &propStub->group(), "baseline-getprop-unboxed-stub-group");
        break;
      }
      case ICStub::GetProp_TypedObject: {
        ICGetProp_TypedObject* propStub = toGetProp_TypedObject();
        MarkShape(trc, &propStub->shape(), "baseline-getprop-typedobject-stub-shape");
        break;
      }
      case ICStub::GetProp_CallDOMProxyNative:
      case ICStub::GetProp_CallDOMProxyWithGenerationNative: {
        ICGetPropCallDOMProxyNativeStub* propStub;
        if (kind() ==  ICStub::GetProp_CallDOMProxyNative)
            propStub = toGetProp_CallDOMProxyNative();
        else
            propStub = toGetProp_CallDOMProxyWithGenerationNative();
        MarkShape(trc, &propStub->shape(), "baseline-getproplistbasenative-stub-shape");
        if (propStub->expandoShape()) {
            MarkShape(trc, &propStub->expandoShape(),
                      "baseline-getproplistbasenative-stub-expandoshape");
        }
        MarkObject(trc, &propStub->holder(), "baseline-getproplistbasenative-stub-holder");
        MarkShape(trc, &propStub->holderShape(), "baseline-getproplistbasenative-stub-holdershape");
        MarkObject(trc, &propStub->getter(), "baseline-getproplistbasenative-stub-getter");
        break;
      }
      case ICStub::GetProp_DOMProxyShadowed: {
        ICGetProp_DOMProxyShadowed* propStub = toGetProp_DOMProxyShadowed();
        MarkShape(trc, &propStub->shape(), "baseline-getproplistbaseshadowed-stub-shape");
        MarkString(trc, &propStub->name(), "baseline-getproplistbaseshadowed-stub-name");
        break;
      }
      case ICStub::GetProp_CallScripted: {
        ICGetProp_CallScripted* callStub = toGetProp_CallScripted();
        MarkShape(trc, &callStub->receiverShape(), "baseline-getpropcallscripted-stub-receivershape");
        MarkObject(trc, &callStub->holder(), "baseline-getpropcallscripted-stub-holder");
        MarkShape(trc, &callStub->holderShape(), "baseline-getpropcallscripted-stub-holdershape");
        MarkObject(trc, &callStub->getter(), "baseline-getpropcallscripted-stub-getter");
        break;
      }
      case ICStub::GetProp_CallNative: {
        ICGetProp_CallNative* callStub = toGetProp_CallNative();
        MarkObject(trc, &callStub->holder(), "baseline-getpropcallnative-stub-holder");
        MarkShape(trc, &callStub->holderShape(), "baseline-getpropcallnative-stub-holdershape");
        MarkObject(trc, &callStub->getter(), "baseline-getpropcallnative-stub-getter");
        break;
      }
      case ICStub::GetProp_CallNativePrototype: {
        ICGetProp_CallNativePrototype* callStub = toGetProp_CallNativePrototype();
        MarkShape(trc, &callStub->receiverShape(), "baseline-getpropcallnativeproto-stub-receivershape");
        MarkObject(trc, &callStub->holder(), "baseline-getpropcallnativeproto-stub-holder");
        MarkShape(trc, &callStub->holderShape(), "baseline-getpropcallnativeproto-stub-holdershape");
        MarkObject(trc, &callStub->getter(), "baseline-getpropcallnativeproto-stub-getter");
        break;
      }
      case ICStub::SetProp_Native: {
        ICSetProp_Native* propStub = toSetProp_Native();
        MarkShape(trc, &propStub->shape(), "baseline-setpropnative-stub-shape");
        MarkObjectGroup(trc, &propStub->group(), "baseline-setpropnative-stub-group");
        break;
      }
      case ICStub::SetProp_NativeAdd: {
        ICSetProp_NativeAdd* propStub = toSetProp_NativeAdd();
        MarkObjectGroup(trc, &propStub->group(), "baseline-setpropnativeadd-stub-group");
        MarkShape(trc, &propStub->newShape(), "baseline-setpropnativeadd-stub-newshape");
        if (propStub->newGroup())
            MarkObjectGroup(trc, &propStub->newGroup(), "baseline-setpropnativeadd-stub-new-group");
        JS_STATIC_ASSERT(ICSetProp_NativeAdd::MAX_PROTO_CHAIN_DEPTH == 4);
        switch (propStub->protoChainDepth()) {
          case 0: propStub->toImpl<0>()->traceShapes(trc); break;
          case 1: propStub->toImpl<1>()->traceShapes(trc); break;
          case 2: propStub->toImpl<2>()->traceShapes(trc); break;
          case 3: propStub->toImpl<3>()->traceShapes(trc); break;
          case 4: propStub->toImpl<4>()->traceShapes(trc); break;
          default: MOZ_CRASH("Invalid proto stub.");
        }
        break;
      }
      case ICStub::SetProp_Unboxed: {
        ICSetProp_Unboxed* propStub = toSetProp_Unboxed();
        MarkObjectGroup(trc, &propStub->group(), "baseline-setprop-unboxed-stub-group");
        break;
      }
      case ICStub::SetProp_TypedObject: {
        ICSetProp_TypedObject* propStub = toSetProp_TypedObject();
        MarkShape(trc, &propStub->shape(), "baseline-setprop-typedobject-stub-shape");
        MarkObjectGroup(trc, &propStub->group(), "baseline-setprop-typedobject-stub-group");
        break;
      }
      case ICStub::SetProp_CallScripted: {
        ICSetProp_CallScripted* callStub = toSetProp_CallScripted();
        MarkShape(trc, &callStub->shape(), "baseline-setpropcallscripted-stub-shape");
        MarkObject(trc, &callStub->holder(), "baseline-setpropcallscripted-stub-holder");
        MarkShape(trc, &callStub->holderShape(), "baseline-setpropcallscripted-stub-holdershape");
        MarkObject(trc, &callStub->setter(), "baseline-setpropcallscripted-stub-setter");
        break;
      }
      case ICStub::SetProp_CallNative: {
        ICSetProp_CallNative* callStub = toSetProp_CallNative();
        MarkShape(trc, &callStub->shape(), "baseline-setpropcallnative-stub-shape");
        MarkObject(trc, &callStub->holder(), "baseline-setpropcallnative-stub-holder");
        MarkShape(trc, &callStub->holderShape(), "baseline-setpropcallnative-stub-holdershape");
        MarkObject(trc, &callStub->setter(), "baseline-setpropcallnative-stub-setter");
        break;
      }
      case ICStub::InstanceOf_Function: {
        ICInstanceOf_Function* instanceofStub = toInstanceOf_Function();
        MarkShape(trc, &instanceofStub->shape(), "baseline-instanceof-fun-shape");
        MarkObject(trc, &instanceofStub->prototypeObject(), "baseline-instanceof-fun-prototype");
        break;
      }
      case ICStub::NewArray_Fallback: {
        ICNewArray_Fallback* stub = toNewArray_Fallback();
        MarkObject(trc, &stub->templateObject(), "baseline-newarray-template");
        break;
      }
      case ICStub::NewObject_Fallback: {
        ICNewObject_Fallback* stub = toNewObject_Fallback();
        MarkObject(trc, &stub->templateObject(), "baseline-newobject-template");
        break;
      }
      case ICStub::Rest_Fallback: {
        ICRest_Fallback* stub = toRest_Fallback();
        MarkObject(trc, &stub->templateObject(), "baseline-rest-template");
        break;
      }
      default:
        break;
    }
}

void
ICFallbackStub::unlinkStub(Zone* zone, ICStub* prev, ICStub* stub)
{
    MOZ_ASSERT(stub->next());

    // If stub is the last optimized stub, update lastStubPtrAddr.
    if (stub->next() == this) {
        MOZ_ASSERT(lastStubPtrAddr_ == stub->addressOfNext());
        if (prev)
            lastStubPtrAddr_ = prev->addressOfNext();
        else
            lastStubPtrAddr_ = icEntry()->addressOfFirstStub();
        *lastStubPtrAddr_ = this;
    } else {
        if (prev) {
            MOZ_ASSERT(prev->next() == stub);
            prev->setNext(stub->next());
        } else {
            MOZ_ASSERT(icEntry()->firstStub() == stub);
            icEntry()->setFirstStub(stub->next());
        }
    }

    MOZ_ASSERT(numOptimizedStubs_ > 0);
    numOptimizedStubs_--;

    if (zone->needsIncrementalBarrier()) {
        // We are removing edges from ICStub to gcthings. Perform one final trace
        // of the stub for incremental GC, as it must know about those edges.
        stub->trace(zone->barrierTracer());
    }

    if (ICStub::CanMakeCalls(stub->kind()) && stub->isMonitored()) {
        // This stub can make calls so we can return to it if it's on the stack.
        // We just have to reset its firstMonitorStub_ field to avoid a stale
        // pointer when purgeOptimizedStubs destroys all optimized monitor
        // stubs (unlinked stubs won't be updated).
        ICTypeMonitor_Fallback* monitorFallback = toMonitoredFallbackStub()->fallbackMonitorStub();
        stub->toMonitoredStub()->resetFirstMonitorStub(monitorFallback);
    }

#ifdef DEBUG
    // Poison stub code to ensure we don't call this stub again. However, if this
    // stub can make calls, a pointer to it may be stored in a stub frame on the
    // stack, so we can't touch the stubCode_ or GC will crash when marking this
    // pointer.
    if (!ICStub::CanMakeCalls(stub->kind()))
        stub->stubCode_ = (uint8_t*)0xbad;
#endif
}

void
ICFallbackStub::unlinkStubsWithKind(JSContext* cx, ICStub::Kind kind)
{
    for (ICStubIterator iter = beginChain(); !iter.atEnd(); iter++) {
        if (iter->kind() == kind)
            iter.unlink(cx);
    }
}

void
ICTypeMonitor_Fallback::resetMonitorStubChain(Zone* zone)
{
    if (zone->needsIncrementalBarrier()) {
        // We are removing edges from monitored stubs to gcthings (JitCode).
        // Perform one final trace of all monitor stubs for incremental GC,
        // as it must know about those edges.
        for (ICStub* s = firstMonitorStub_; !s->isTypeMonitor_Fallback(); s = s->next())
            s->trace(zone->barrierTracer());
    }

    firstMonitorStub_ = this;
    numOptimizedMonitorStubs_ = 0;

    if (hasFallbackStub_) {
        lastMonitorStubPtrAddr_ = nullptr;

        // Reset firstMonitorStub_ field of all monitored stubs.
        for (ICStubConstIterator iter = mainFallbackStub_->beginChainConst();
             !iter.atEnd(); iter++)
        {
            if (!iter->isMonitored())
                continue;
            iter->toMonitoredStub()->resetFirstMonitorStub(this);
        }
    } else {
        icEntry_->setFirstStub(this);
        lastMonitorStubPtrAddr_ = icEntry_->addressOfFirstStub();
    }
}

ICMonitoredStub::ICMonitoredStub(Kind kind, JitCode* stubCode, ICStub* firstMonitorStub)
  : ICStub(kind, ICStub::Monitored, stubCode),
    firstMonitorStub_(firstMonitorStub)
{
    // If the first monitored stub is a ICTypeMonitor_Fallback stub, then
    // double check that _its_ firstMonitorStub is the same as this one.
    MOZ_ASSERT_IF(firstMonitorStub_->isTypeMonitor_Fallback(),
                  firstMonitorStub_->toTypeMonitor_Fallback()->firstMonitorStub() ==
                     firstMonitorStub_);
}

bool
ICMonitoredFallbackStub::initMonitoringChain(JSContext* cx, ICStubSpace* space)
{
    MOZ_ASSERT(fallbackMonitorStub_ == nullptr);

    ICTypeMonitor_Fallback::Compiler compiler(cx, this);
    ICTypeMonitor_Fallback* stub = compiler.getStub(space);
    if (!stub)
        return false;
    fallbackMonitorStub_ = stub;
    return true;
}

bool
ICMonitoredFallbackStub::addMonitorStubForValue(JSContext* cx, JSScript* script, HandleValue val)
{
    return fallbackMonitorStub_->addMonitorStubForValue(cx, script, val);
}

bool
ICUpdatedStub::initUpdatingChain(JSContext* cx, ICStubSpace* space)
{
    MOZ_ASSERT(firstUpdateStub_ == nullptr);

    ICTypeUpdate_Fallback::Compiler compiler(cx);
    ICTypeUpdate_Fallback* stub = compiler.getStub(space);
    if (!stub)
        return false;

    firstUpdateStub_ = stub;
    return true;
}

JitCode*
ICStubCompiler::getStubCode()
{
    JitCompartment* comp = cx->compartment()->jitCompartment();

    // Check for existing cached stubcode.
    uint32_t stubKey = getKey();
    JitCode* stubCode = comp->getStubCode(stubKey);
    if (stubCode)
        return stubCode;

    // Compile new stubcode.
    JitContext jctx(cx, nullptr);
    MacroAssembler masm;
#ifdef JS_CODEGEN_ARM
    masm.setSecondScratchReg(BaselineSecondScratchReg);
#endif

    if (!generateStubCode(masm))
        return nullptr;
    Linker linker(masm);
    AutoFlushICache afc("getStubCode");
    Rooted<JitCode*> newStubCode(cx, linker.newCode<CanGC>(cx, BASELINE_CODE));
    if (!newStubCode)
        return nullptr;

    // After generating code, run postGenerateStubCode()
    if (!postGenerateStubCode(masm, newStubCode))
        return nullptr;

    // All barriers are emitted off-by-default, enable them if needed.
    if (cx->zone()->needsIncrementalBarrier())
        newStubCode->togglePreBarriers(true);

    // Cache newly compiled stubcode.
    if (!comp->putStubCode(stubKey, newStubCode))
        return nullptr;

    MOZ_ASSERT(entersStubFrame_ == ICStub::CanMakeCalls(kind));

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(newStubCode, "BaselineIC");
#endif

    return newStubCode;
}

bool
ICStubCompiler::tailCallVM(const VMFunction& fun, MacroAssembler& masm)
{
    JitCode* code = cx->runtime()->jitRuntime()->getVMWrapper(fun);
    if (!code)
        return false;

    MOZ_ASSERT(fun.expectTailCall == TailCall);
    uint32_t argSize = fun.explicitStackSlots() * sizeof(void*);
    EmitTailCallVM(code, masm, argSize);
    return true;
}

bool
ICStubCompiler::callVM(const VMFunction& fun, MacroAssembler& masm)
{
    JitCode* code = cx->runtime()->jitRuntime()->getVMWrapper(fun);
    if (!code)
        return false;

    MOZ_ASSERT(fun.expectTailCall == NonTailCall);
    EmitCallVM(code, masm);
    return true;
}

bool
ICStubCompiler::callTypeUpdateIC(MacroAssembler& masm, uint32_t objectOffset)
{
    JitCode* code = cx->runtime()->jitRuntime()->getVMWrapper(DoTypeUpdateFallbackInfo);
    if (!code)
        return false;

    EmitCallTypeUpdateIC(masm, code, objectOffset);
    return true;
}

void
ICStubCompiler::enterStubFrame(MacroAssembler& masm, Register scratch)
{
    EmitEnterStubFrame(masm, scratch);
#ifdef DEBUG
    entersStubFrame_ = true;
#endif
}

void
ICStubCompiler::leaveStubFrame(MacroAssembler& masm, bool calledIntoIon)
{
    MOZ_ASSERT(entersStubFrame_);
    EmitLeaveStubFrame(masm, calledIntoIon);
}

inline bool
ICStubCompiler::emitPostWriteBarrierSlot(MacroAssembler& masm, Register obj, ValueOperand val,
                                         Register scratch, GeneralRegisterSet saveRegs)
{
    Label skipBarrier;
    masm.branchPtrInNurseryRange(Assembler::Equal, obj, scratch, &skipBarrier);
    masm.branchValueIsNurseryObject(Assembler::NotEqual, val, scratch, &skipBarrier);

    // void PostWriteBarrier(JSRuntime* rt, JSObject* obj);
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
    saveRegs.add(BaselineTailCallReg);
#endif
    saveRegs = GeneralRegisterSet::Intersect(saveRegs, GeneralRegisterSet::Volatile());
    masm.PushRegsInMask(saveRegs);
    masm.setupUnalignedABICall(2, scratch);
    masm.movePtr(ImmPtr(cx->runtime()), scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(obj);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, PostWriteBarrier));
    masm.PopRegsInMask(saveRegs);

    masm.bind(&skipBarrier);
    return true;
}

//
// WarmUpCounter_Fallback
//

static bool
EnsureCanEnterIon(JSContext* cx, ICWarmUpCounter_Fallback* stub, BaselineFrame* frame,
                  HandleScript script, jsbytecode* pc, void** jitcodePtr)
{
    MOZ_ASSERT(jitcodePtr);
    MOZ_ASSERT(!*jitcodePtr);

    bool isLoopEntry = (JSOp(*pc) == JSOP_LOOPENTRY);

    MethodStatus stat;
    if (isLoopEntry) {
        MOZ_ASSERT(LoopEntryCanIonOsr(pc));
        JitSpew(JitSpew_BaselineOSR, "  Compile at loop entry!");
        stat = CanEnterAtBranch(cx, script, frame, pc);
    } else if (frame->isFunctionFrame()) {
        JitSpew(JitSpew_BaselineOSR, "  Compile function from top for later entry!");
        stat = CompileFunctionForBaseline(cx, script, frame);
    } else {
        return true;
    }

    if (stat == Method_Error) {
        JitSpew(JitSpew_BaselineOSR, "  Compile with Ion errored!");
        return false;
    }

    if (stat == Method_CantCompile)
        JitSpew(JitSpew_BaselineOSR, "  Can't compile with Ion!");
    else if (stat == Method_Skipped)
        JitSpew(JitSpew_BaselineOSR, "  Skipped compile with Ion!");
    else if (stat == Method_Compiled)
        JitSpew(JitSpew_BaselineOSR, "  Compiled with Ion!");
    else
        MOZ_CRASH("Invalid MethodStatus!");

    // Failed to compile.  Reset warm-up counter and return.
    if (stat != Method_Compiled) {
        // TODO: If stat == Method_CantCompile, insert stub that just skips the
        // warm-up counter entirely, instead of resetting it.
        bool bailoutExpected = script->hasIonScript() && script->ionScript()->bailoutExpected();
        if (stat == Method_CantCompile || bailoutExpected) {
            JitSpew(JitSpew_BaselineOSR, "  Reset WarmUpCounter cantCompile=%s bailoutExpected=%s!",
                    stat == Method_CantCompile ? "yes" : "no",
                    bailoutExpected ? "yes" : "no");
            script->resetWarmUpCounter();
        }
        return true;
    }

    if (isLoopEntry) {
        IonScript* ion = script->ionScript();
        MOZ_ASSERT(cx->runtime()->spsProfiler.enabled() == ion->hasProfilingInstrumentation());
        MOZ_ASSERT(ion->osrPc() == pc);

        JitSpew(JitSpew_BaselineOSR, "  OSR possible!");
        *jitcodePtr = ion->method()->raw() + ion->osrEntryOffset();
    }

    return true;
}

//
// The following data is kept in a temporary heap-allocated buffer, stored in
// JitRuntime (high memory addresses at top, low at bottom):
//
//     +----->+=================================+  --      <---- High Address
//     |      |                                 |   |
//     |      |     ...BaselineFrame...         |   |-- Copy of BaselineFrame + stack values
//     |      |                                 |   |
//     |      +---------------------------------+   |
//     |      |                                 |   |
//     |      |     ...Locals/Stack...          |   |
//     |      |                                 |   |
//     |      +=================================+  --
//     |      |     Padding(Maybe Empty)        |
//     |      +=================================+  --
//     +------|-- baselineFrame                 |   |-- IonOsrTempData
//            |   jitcode                       |   |
//            +=================================+  --      <---- Low Address
//
// A pointer to the IonOsrTempData is returned.

struct IonOsrTempData
{
    void* jitcode;
    uint8_t* baselineFrame;
};

static IonOsrTempData*
PrepareOsrTempData(JSContext* cx, ICWarmUpCounter_Fallback* stub, BaselineFrame* frame,
                   HandleScript script, jsbytecode* pc, void* jitcode)
{
    size_t numLocalsAndStackVals = frame->numValueSlots();

    // Calculate the amount of space to allocate:
    //      BaselineFrame space:
    //          (sizeof(Value) * (numLocals + numStackVals))
    //        + sizeof(BaselineFrame)
    //
    //      IonOsrTempData space:
    //          sizeof(IonOsrTempData)

    size_t frameSpace = sizeof(BaselineFrame) + sizeof(Value) * numLocalsAndStackVals;
    size_t ionOsrTempDataSpace = sizeof(IonOsrTempData);

    size_t totalSpace = AlignBytes(frameSpace, sizeof(Value)) +
                        AlignBytes(ionOsrTempDataSpace, sizeof(Value));

    IonOsrTempData* info = (IonOsrTempData*)cx->runtime()->getJitRuntime(cx)->allocateOsrTempData(totalSpace);
    if (!info)
        return nullptr;

    memset(info, 0, totalSpace);

    info->jitcode = jitcode;

    // Copy the BaselineFrame + local/stack Values to the buffer. Arguments and
    // |this| are not copied but left on the stack: the Baseline and Ion frame
    // share the same frame prefix and Ion won't clobber these values. Note
    // that info->baselineFrame will point to the *end* of the frame data, like
    // the frame pointer register in baseline frames.
    uint8_t* frameStart = (uint8_t*)info + AlignBytes(ionOsrTempDataSpace, sizeof(Value));
    info->baselineFrame = frameStart + frameSpace;

    memcpy(frameStart, (uint8_t*)frame - numLocalsAndStackVals * sizeof(Value), frameSpace);

    JitSpew(JitSpew_BaselineOSR, "Allocated IonOsrTempData at %p", (void*) info);
    JitSpew(JitSpew_BaselineOSR, "Jitcode is %p", info->jitcode);

    // All done.
    return info;
}

static bool
DoWarmUpCounterFallback(JSContext* cx, ICWarmUpCounter_Fallback* stub, BaselineFrame* frame,
                   IonOsrTempData** infoPtr)
{
    MOZ_ASSERT(infoPtr);
    *infoPtr = nullptr;

    // A TI OOM will disable TI and Ion.
    if (!jit::IsIonEnabled(cx))
        return true;

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    bool isLoopEntry = JSOp(*pc) == JSOP_LOOPENTRY;

    MOZ_ASSERT(!isLoopEntry || LoopEntryCanIonOsr(pc));

    FallbackICSpew(cx, stub, "WarmUpCounter(%d)", isLoopEntry ? int(script->pcToOffset(pc)) : int(-1));

    if (!script->canIonCompile()) {
        // TODO: ASSERT that ion-compilation-disabled checker stub doesn't exist.
        // TODO: Clear all optimized stubs.
        // TODO: Add a ion-compilation-disabled checker IC stub
        script->resetWarmUpCounter();
        return true;
    }

    MOZ_ASSERT(!script->isIonCompilingOffThread());

    // If Ion script exists, but PC is not at a loop entry, then Ion will be entered for
    // this script at an appropriate LOOPENTRY or the next time this function is called.
    if (script->hasIonScript() && !isLoopEntry) {
        JitSpew(JitSpew_BaselineOSR, "IonScript exists, but not at loop entry!");
        // TODO: ASSERT that a ion-script-already-exists checker stub doesn't exist.
        // TODO: Clear all optimized stubs.
        // TODO: Add a ion-script-already-exists checker stub.
        return true;
    }

    // Ensure that Ion-compiled code is available.
    JitSpew(JitSpew_BaselineOSR,
            "WarmUpCounter for %s:%d reached %d at pc %p, trying to switch to Ion!",
            script->filename(), script->lineno(), (int) script->getWarmUpCount(), (void*) pc);
    void* jitcode = nullptr;
    if (!EnsureCanEnterIon(cx, stub, frame, script, pc, &jitcode))
        return false;

    // Jitcode should only be set here if not at loop entry.
    MOZ_ASSERT_IF(!isLoopEntry, !jitcode);
    if (!jitcode)
        return true;

    // Prepare the temporary heap copy of the fake InterpreterFrame and actual args list.
    JitSpew(JitSpew_BaselineOSR, "Got jitcode.  Preparing for OSR into ion.");
    IonOsrTempData* info = PrepareOsrTempData(cx, stub, frame, script, pc, jitcode);
    if (!info)
        return false;
    *infoPtr = info;

    return true;
}

typedef bool (*DoWarmUpCounterFallbackFn)(JSContext*, ICWarmUpCounter_Fallback*, BaselineFrame* frame,
                                     IonOsrTempData** infoPtr);
static const VMFunction DoWarmUpCounterFallbackInfo =
    FunctionInfo<DoWarmUpCounterFallbackFn>(DoWarmUpCounterFallback);

bool
ICWarmUpCounter_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    // enterStubFrame is going to clobber the BaselineFrameReg, save it in R0.scratchReg()
    // first.
    masm.movePtr(BaselineFrameReg, R0.scratchReg());

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, R1.scratchReg());

    Label noCompiledCode;
    // Call DoWarmUpCounterFallback to compile/check-for Ion-compiled function
    {
        // Push IonOsrTempData pointer storage
        masm.subPtr(Imm32(sizeof(void*)), BaselineStackReg);
        masm.push(BaselineStackReg);

        // Push JitFrameLayout pointer.
        masm.loadBaselineFramePtr(R0.scratchReg(), R0.scratchReg());
        masm.push(R0.scratchReg());

        // Push stub pointer.
        masm.push(BaselineStubReg);

        if (!callVM(DoWarmUpCounterFallbackInfo, masm))
            return false;

        // Pop IonOsrTempData pointer.
        masm.pop(R0.scratchReg());

        leaveStubFrame(masm);

        // If no JitCode was found, then skip just exit the IC.
        masm.branchPtr(Assembler::Equal, R0.scratchReg(), ImmPtr(nullptr), &noCompiledCode);
    }

    // Get a scratch register.
    GeneralRegisterSet regs(availableGeneralRegs(0));
    Register osrDataReg = R0.scratchReg();
    regs.take(osrDataReg);
    regs.takeUnchecked(OsrFrameReg);

    Register scratchReg = regs.takeAny();

    // At this point, stack looks like:
    //  +-> [...Calling-Frame...]
    //  |   [...Actual-Args/ThisV/ArgCount/Callee...]
    //  |   [Descriptor]
    //  |   [Return-Addr]
    //  +---[Saved-FramePtr]            <-- BaselineFrameReg points here.
    //      [...Baseline-Frame...]

    // Restore the stack pointer to point to the saved frame pointer.
    masm.movePtr(BaselineFrameReg, BaselineStackReg);

    // Discard saved frame pointer, so that the return address is on top of
    // the stack.
    masm.pop(scratchReg);

#ifdef DEBUG
    // If profiler instrumentation is on, ensure that lastProfilingFrame is
    // the frame currently being OSR-ed
    {
        Label checkOk;
        AbsoluteAddress addressOfEnabled(cx->runtime()->spsProfiler.addressOfEnabled());
        masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &checkOk);
        masm.loadPtr(AbsoluteAddress((void*)&cx->runtime()->jitActivation), scratchReg);
        masm.loadPtr(Address(scratchReg, JitActivation::offsetOfLastProfilingFrame()), scratchReg);

        // It may be the case that we entered the baseline frame with
        // profiling turned off on, then in a call within a loop (i.e. a
        // callee frame), turn on profiling, then return to this frame,
        // and then OSR with profiling turned on.  In this case, allow for
        // lastProfilingFrame to be null.
        masm.branchPtr(Assembler::Equal, scratchReg, Imm32(0), &checkOk);

        masm.branchPtr(Assembler::Equal, scratchReg, BaselineStackReg, &checkOk);
        masm.assumeUnreachable("Baseline OSR lastProfilingFrame mismatch.");
        masm.bind(&checkOk);
    }
#endif

    // Jump into Ion.
    masm.loadPtr(Address(osrDataReg, offsetof(IonOsrTempData, jitcode)), scratchReg);
    masm.loadPtr(Address(osrDataReg, offsetof(IonOsrTempData, baselineFrame)), OsrFrameReg);
    masm.jump(scratchReg);

    // No jitcode available, do nothing.
    masm.bind(&noCompiledCode);
    EmitReturnFromIC(masm);
    return true;
}


//
// TypeMonitor_Fallback
//

bool
ICTypeMonitor_Fallback::addMonitorStubForValue(JSContext* cx, JSScript* script, HandleValue val)
{
    bool wasDetachedMonitorChain = lastMonitorStubPtrAddr_ == nullptr;
    MOZ_ASSERT_IF(wasDetachedMonitorChain, numOptimizedMonitorStubs_ == 0);

    if (numOptimizedMonitorStubs_ >= MAX_OPTIMIZED_STUBS) {
        // TODO: if the TypeSet becomes unknown or has the AnyObject type,
        // replace stubs with a single stub to handle these.
        return true;
    }

    if (val.isPrimitive()) {
        MOZ_ASSERT(!val.isMagic());
        JSValueType type = val.isDouble() ? JSVAL_TYPE_DOUBLE : val.extractNonDoubleType();

        // Check for existing TypeMonitor stub.
        ICTypeMonitor_PrimitiveSet* existingStub = nullptr;
        for (ICStubConstIterator iter(firstMonitorStub()); !iter.atEnd(); iter++) {
            if (iter->isTypeMonitor_PrimitiveSet()) {
                existingStub = iter->toTypeMonitor_PrimitiveSet();
                if (existingStub->containsType(type))
                    return true;
            }
        }

        ICTypeMonitor_PrimitiveSet::Compiler compiler(cx, existingStub, type);
        ICStub* stub = existingStub ? compiler.updateStub()
                                    : compiler.getStub(compiler.getStubSpace(script));
        if (!stub) {
            js_ReportOutOfMemory(cx);
            return false;
        }

        JitSpew(JitSpew_BaselineIC, "  %s TypeMonitor stub %p for primitive type %d",
                existingStub ? "Modified existing" : "Created new", stub, type);

        if (!existingStub) {
            MOZ_ASSERT(!hasStub(TypeMonitor_PrimitiveSet));
            addOptimizedMonitorStub(stub);
        }

    } else if (val.toObject().isSingleton()) {
        RootedObject obj(cx, &val.toObject());

        // Check for existing TypeMonitor stub.
        for (ICStubConstIterator iter(firstMonitorStub()); !iter.atEnd(); iter++) {
            if (iter->isTypeMonitor_SingleObject() &&
                iter->toTypeMonitor_SingleObject()->object() == obj)
            {
                return true;
            }
        }

        ICTypeMonitor_SingleObject::Compiler compiler(cx, obj);
        ICStub* stub = compiler.getStub(compiler.getStubSpace(script));
        if (!stub) {
            js_ReportOutOfMemory(cx);
            return false;
        }

        JitSpew(JitSpew_BaselineIC, "  Added TypeMonitor stub %p for singleton %p",
                stub, obj.get());

        addOptimizedMonitorStub(stub);

    } else {
        RootedObjectGroup group(cx, val.toObject().group());

        // Check for existing TypeMonitor stub.
        for (ICStubConstIterator iter(firstMonitorStub()); !iter.atEnd(); iter++) {
            if (iter->isTypeMonitor_ObjectGroup() &&
                iter->toTypeMonitor_ObjectGroup()->group() == group)
            {
                return true;
            }
        }

        ICTypeMonitor_ObjectGroup::Compiler compiler(cx, group);
        ICStub* stub = compiler.getStub(compiler.getStubSpace(script));
        if (!stub) {
            js_ReportOutOfMemory(cx);
            return false;
        }

        JitSpew(JitSpew_BaselineIC, "  Added TypeMonitor stub %p for ObjectGroup %p",
                stub, group.get());

        addOptimizedMonitorStub(stub);
    }

    bool firstMonitorStubAdded = wasDetachedMonitorChain && (numOptimizedMonitorStubs_ > 0);

    if (firstMonitorStubAdded) {
        // Was an empty monitor chain before, but a new stub was added.  This is the
        // only time that any main stubs' firstMonitorStub fields need to be updated to
        // refer to the newly added monitor stub.
        ICStub* firstStub = mainFallbackStub_->icEntry()->firstStub();
        for (ICStubConstIterator iter(firstStub); !iter.atEnd(); iter++) {
            // Non-monitored stubs are used if the result has always the same type,
            // e.g. a StringLength stub will always return int32.
            if (!iter->isMonitored())
                continue;

            // Since we just added the first optimized monitoring stub, any
            // existing main stub's |firstMonitorStub| MUST be pointing to the fallback
            // monitor stub (i.e. this stub).
            MOZ_ASSERT(iter->toMonitoredStub()->firstMonitorStub() == this);
            iter->toMonitoredStub()->updateFirstMonitorStub(firstMonitorStub_);
        }
    }

    return true;
}

static bool
DoTypeMonitorFallback(JSContext* cx, BaselineFrame* frame, ICTypeMonitor_Fallback* stub,
                      HandleValue value, MutableHandleValue res)
{
    // It's possible that we arrived here from bailing out of Ion, and that
    // Ion proved that the value is dead and optimized out. In such cases, do
    // nothing.
    if (value.isMagic(JS_OPTIMIZED_OUT)) {
        res.set(value);
        return true;
    }

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    TypeFallbackICSpew(cx, stub, "TypeMonitor");

    uint32_t argument;
    if (stub->monitorsThis()) {
        MOZ_ASSERT(pc == script->code());
        TypeScript::SetThis(cx, script, value);
    } else if (stub->monitorsArgument(&argument)) {
        MOZ_ASSERT(pc == script->code());
        TypeScript::SetArgument(cx, script, argument, value);
    } else {
        TypeScript::Monitor(cx, script, pc, value);
    }

    if (!stub->addMonitorStubForValue(cx, script, value))
        return false;

    // Copy input value to res.
    res.set(value);
    return true;
}

typedef bool (*DoTypeMonitorFallbackFn)(JSContext*, BaselineFrame*, ICTypeMonitor_Fallback*,
                                        HandleValue, MutableHandleValue);
static const VMFunction DoTypeMonitorFallbackInfo =
    FunctionInfo<DoTypeMonitorFallbackFn>(DoTypeMonitorFallback, TailCall);

bool
ICTypeMonitor_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoTypeMonitorFallbackInfo, masm);
}

bool
ICTypeMonitor_PrimitiveSet::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label success;
    if ((flags_ & TypeToFlag(JSVAL_TYPE_INT32)) && !(flags_ & TypeToFlag(JSVAL_TYPE_DOUBLE)))
        masm.branchTestInt32(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_DOUBLE))
        masm.branchTestNumber(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_UNDEFINED))
        masm.branchTestUndefined(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_BOOLEAN))
        masm.branchTestBoolean(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_STRING))
        masm.branchTestString(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_SYMBOL))
        masm.branchTestSymbol(Assembler::Equal, R0, &success);

    // Currently, we will never generate primitive stub checks for object.  However,
    // when we do get to the point where we want to collapse our monitor chains of
    // objects and singletons down (when they get too long) to a generic "any object"
    // in coordination with the typeset doing the same thing, this will need to
    // be re-enabled.
    /*
    if (flags_ & TypeToFlag(JSVAL_TYPE_OBJECT))
        masm.branchTestObject(Assembler::Equal, R0, &success);
    */
    MOZ_ASSERT(!(flags_ & TypeToFlag(JSVAL_TYPE_OBJECT)));

    if (flags_ & TypeToFlag(JSVAL_TYPE_NULL))
        masm.branchTestNull(Assembler::Equal, R0, &success);

    EmitStubGuardFailure(masm);

    masm.bind(&success);
    EmitReturnFromIC(masm);
    return true;
}

bool
ICTypeMonitor_SingleObject::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Guard on the object's identity.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    Address expectedObject(BaselineStubReg, ICTypeMonitor_SingleObject::offsetOfObject());
    masm.branchPtr(Assembler::NotEqual, expectedObject, obj, &failure);

    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICTypeMonitor_ObjectGroup::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Guard on the object's ObjectGroup.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(obj, JSObject::offsetOfGroup()), R1.scratchReg());

    Address expectedGroup(BaselineStubReg, ICTypeMonitor_ObjectGroup::offsetOfGroup());
    masm.branchPtr(Assembler::NotEqual, expectedGroup, R1.scratchReg(), &failure);

    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICUpdatedStub::addUpdateStubForValue(JSContext* cx, HandleScript script, HandleObject obj,
                                     HandleId id, HandleValue val)
{
    if (numOptimizedStubs_ >= MAX_OPTIMIZED_STUBS) {
        // TODO: if the TypeSet becomes unknown or has the AnyObject type,
        // replace stubs with a single stub to handle these.
        return true;
    }

    EnsureTrackPropertyTypes(cx, obj, id);

    // Make sure that undefined values are explicitly included in the property
    // types for an object if generating a stub to write an undefined value.
    if (val.isUndefined() && CanHaveEmptyPropertyTypesForOwnProperty(obj))
        AddTypePropertyId(cx, obj, id, val);

    if (val.isPrimitive()) {
        JSValueType type = val.isDouble() ? JSVAL_TYPE_DOUBLE : val.extractNonDoubleType();

        // Check for existing TypeUpdate stub.
        ICTypeUpdate_PrimitiveSet* existingStub = nullptr;
        for (ICStubConstIterator iter(firstUpdateStub_); !iter.atEnd(); iter++) {
            if (iter->isTypeUpdate_PrimitiveSet()) {
                existingStub = iter->toTypeUpdate_PrimitiveSet();
                if (existingStub->containsType(type))
                    return true;
            }
        }

        ICTypeUpdate_PrimitiveSet::Compiler compiler(cx, existingStub, type);
        ICStub* stub = existingStub ? compiler.updateStub()
                                    : compiler.getStub(compiler.getStubSpace(script));
        if (!stub)
            return false;
        if (!existingStub) {
            MOZ_ASSERT(!hasTypeUpdateStub(TypeUpdate_PrimitiveSet));
            addOptimizedUpdateStub(stub);
        }

        JitSpew(JitSpew_BaselineIC, "  %s TypeUpdate stub %p for primitive type %d",
                existingStub ? "Modified existing" : "Created new", stub, type);

    } else if (val.toObject().isSingleton()) {
        RootedObject obj(cx, &val.toObject());

        // Check for existing TypeUpdate stub.
        for (ICStubConstIterator iter(firstUpdateStub_); !iter.atEnd(); iter++) {
            if (iter->isTypeUpdate_SingleObject() &&
                iter->toTypeUpdate_SingleObject()->object() == obj)
            {
                return true;
            }
        }

        ICTypeUpdate_SingleObject::Compiler compiler(cx, obj);
        ICStub* stub = compiler.getStub(compiler.getStubSpace(script));
        if (!stub)
            return false;

        JitSpew(JitSpew_BaselineIC, "  Added TypeUpdate stub %p for singleton %p", stub, obj.get());

        addOptimizedUpdateStub(stub);

    } else {
        RootedObjectGroup group(cx, val.toObject().group());

        // Check for existing TypeUpdate stub.
        for (ICStubConstIterator iter(firstUpdateStub_); !iter.atEnd(); iter++) {
            if (iter->isTypeUpdate_ObjectGroup() &&
                iter->toTypeUpdate_ObjectGroup()->group() == group)
            {
                return true;
            }
        }

        ICTypeUpdate_ObjectGroup::Compiler compiler(cx, group);
        ICStub* stub = compiler.getStub(compiler.getStubSpace(script));
        if (!stub)
            return false;

        JitSpew(JitSpew_BaselineIC, "  Added TypeUpdate stub %p for ObjectGroup %p",
                stub, group.get());

        addOptimizedUpdateStub(stub);
    }

    return true;
}

//
// TypeUpdate_Fallback
//
static bool
DoTypeUpdateFallback(JSContext* cx, BaselineFrame* frame, ICUpdatedStub* stub, HandleValue objval,
                     HandleValue value)
{
    FallbackICSpew(cx, stub->getChainFallback(), "TypeUpdate(%s)",
                   ICStub::KindString(stub->kind()));

    RootedScript script(cx, frame->script());
    RootedObject obj(cx, &objval.toObject());
    RootedId id(cx);

    switch(stub->kind()) {
      case ICStub::SetElem_Dense:
      case ICStub::SetElem_DenseAdd: {
        MOZ_ASSERT(obj->isNative());
        id = JSID_VOID;
        AddTypePropertyId(cx, obj, id, value);
        break;
      }
      case ICStub::SetProp_Native:
      case ICStub::SetProp_NativeAdd:
      case ICStub::SetProp_Unboxed: {
        MOZ_ASSERT(obj->isNative() || obj->is<UnboxedPlainObject>());
        jsbytecode* pc = stub->getChainFallback()->icEntry()->pc(script);
        if (*pc == JSOP_SETALIASEDVAR || *pc == JSOP_INITALIASEDLEXICAL)
            id = NameToId(ScopeCoordinateName(cx->runtime()->scopeCoordinateNameCache, script, pc));
        else
            id = NameToId(script->getName(pc));
        AddTypePropertyId(cx, obj, id, value);
        break;
      }
      case ICStub::SetProp_TypedObject: {
        MOZ_ASSERT(obj->is<TypedObject>());
        jsbytecode* pc = stub->getChainFallback()->icEntry()->pc(script);
        id = NameToId(script->getName(pc));
        if (stub->toSetProp_TypedObject()->isObjectReference()) {
            // Ignore all values being written except plain objects. Null
            // is included implicitly in type information for this property,
            // and non-object non-null values will cause the stub to fail to
            // match shortly and we will end up doing the assignment in the VM.
            if (value.isObject())
                AddTypePropertyId(cx, obj, id, value);
        } else {
            // Ignore undefined values, which are included implicitly in type
            // information for this property.
            if (!value.isUndefined())
                AddTypePropertyId(cx, obj, id, value);
        }
        break;
      }
      default:
        MOZ_CRASH("Invalid stub");
    }

    return stub->addUpdateStubForValue(cx, script, obj, id, value);
}

typedef bool (*DoTypeUpdateFallbackFn)(JSContext*, BaselineFrame*, ICUpdatedStub*, HandleValue,
                                       HandleValue);
const VMFunction DoTypeUpdateFallbackInfo =
    FunctionInfo<DoTypeUpdateFallbackFn>(DoTypeUpdateFallback, NonTailCall);

bool
ICTypeUpdate_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    // Just store false into R1.scratchReg() and return.
    masm.move32(Imm32(0), R1.scratchReg());
    EmitReturnFromIC(masm);
    return true;
}

bool
ICTypeUpdate_PrimitiveSet::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label success;
    if ((flags_ & TypeToFlag(JSVAL_TYPE_INT32)) && !(flags_ & TypeToFlag(JSVAL_TYPE_DOUBLE)))
        masm.branchTestInt32(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_DOUBLE))
        masm.branchTestNumber(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_UNDEFINED))
        masm.branchTestUndefined(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_BOOLEAN))
        masm.branchTestBoolean(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_STRING))
        masm.branchTestString(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_SYMBOL))
        masm.branchTestSymbol(Assembler::Equal, R0, &success);

    // Currently, we will never generate primitive stub checks for object.  However,
    // when we do get to the point where we want to collapse our monitor chains of
    // objects and singletons down (when they get too long) to a generic "any object"
    // in coordination with the typeset doing the same thing, this will need to
    // be re-enabled.
    /*
    if (flags_ & TypeToFlag(JSVAL_TYPE_OBJECT))
        masm.branchTestObject(Assembler::Equal, R0, &success);
    */
    MOZ_ASSERT(!(flags_ & TypeToFlag(JSVAL_TYPE_OBJECT)));

    if (flags_ & TypeToFlag(JSVAL_TYPE_NULL))
        masm.branchTestNull(Assembler::Equal, R0, &success);

    EmitStubGuardFailure(masm);

    // Type matches, load true into R1.scratchReg() and return.
    masm.bind(&success);
    masm.mov(ImmWord(1), R1.scratchReg());
    EmitReturnFromIC(masm);

    return true;
}

bool
ICTypeUpdate_SingleObject::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Guard on the object's identity.
    Register obj = masm.extractObject(R0, R1.scratchReg());
    Address expectedObject(BaselineStubReg, ICTypeUpdate_SingleObject::offsetOfObject());
    masm.branchPtr(Assembler::NotEqual, expectedObject, obj, &failure);

    // Identity matches, load true into R1.scratchReg() and return.
    masm.mov(ImmWord(1), R1.scratchReg());
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICTypeUpdate_ObjectGroup::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Guard on the object's ObjectGroup.
    Register obj = masm.extractObject(R0, R1.scratchReg());
    masm.loadPtr(Address(obj, JSObject::offsetOfGroup()), R1.scratchReg());

    Address expectedGroup(BaselineStubReg, ICTypeUpdate_ObjectGroup::offsetOfGroup());
    masm.branchPtr(Assembler::NotEqual, expectedGroup, R1.scratchReg(), &failure);

    // Group matches, load true into R1.scratchReg() and return.
    masm.mov(ImmWord(1), R1.scratchReg());
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// VM function to help call native getters.
//

static bool
DoCallNativeGetter(JSContext* cx, HandleFunction callee, HandleObject obj,
                   MutableHandleValue result)
{
    MOZ_ASSERT(callee->isNative());
    JSNative natfun = callee->native();

    JS::AutoValueArray<2> vp(cx);
    vp[0].setObject(*callee.get());
    vp[1].setObject(*obj.get());

    if (!natfun(cx, 0, vp.begin()))
        return false;

    result.set(vp[0]);
    return true;
}

typedef bool (*DoCallNativeGetterFn)(JSContext*, HandleFunction, HandleObject, MutableHandleValue);
static const VMFunction DoCallNativeGetterInfo =
    FunctionInfo<DoCallNativeGetterFn>(DoCallNativeGetter);

//
// This_Fallback
//

static bool
DoThisFallback(JSContext* cx, ICThis_Fallback* stub, HandleValue thisv, MutableHandleValue ret)
{
    FallbackICSpew(cx, stub, "This");

    JSObject* thisObj = BoxNonStrictThis(cx, thisv);
    if (!thisObj)
        return false;

    ret.setObject(*thisObj);
    return true;
}

typedef bool (*DoThisFallbackFn)(JSContext*, ICThis_Fallback*, HandleValue, MutableHandleValue);
static const VMFunction DoThisFallbackInfo = FunctionInfo<DoThisFallbackFn>(DoThisFallback, TailCall);

bool
ICThis_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    masm.pushValue(R0);
    masm.push(BaselineStubReg);

    return tailCallVM(DoThisFallbackInfo, masm);
}

//
// NewArray_Fallback
//

static bool
DoNewArray(JSContext* cx, ICNewArray_Fallback* stub, uint32_t length,
           HandleObjectGroup group, MutableHandleValue res)
{
    FallbackICSpew(cx, stub, "NewArray");

    JSObject* obj = NewDenseArray(cx, length, group, NewArray_FullyAllocating);
    if (!obj)
        return false;

    res.setObject(*obj);
    return true;
}

typedef bool(*DoNewArrayFn)(JSContext*, ICNewArray_Fallback*, uint32_t, HandleObjectGroup,
                            MutableHandleValue);
static const VMFunction DoNewArrayInfo = FunctionInfo<DoNewArrayFn>(DoNewArray, TailCall);

bool
ICNewArray_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    masm.push(R1.scratchReg()); // type
    masm.push(R0.scratchReg()); // length
    masm.push(BaselineStubReg); // stub.

    return tailCallVM(DoNewArrayInfo, masm);
}

//
// NewObject_Fallback
//

static bool
DoNewObject(JSContext* cx, ICNewObject_Fallback* stub, MutableHandleValue res)
{
    FallbackICSpew(cx, stub, "NewObject");

    RootedPlainObject templateObject(cx, stub->templateObject());
    JSObject* obj = NewInitObject(cx, templateObject);
    if (!obj)
        return false;

    res.setObject(*obj);
    return true;
}

typedef bool(*DoNewObjectFn)(JSContext*, ICNewObject_Fallback*, MutableHandleValue);
static const VMFunction DoNewObjectInfo = FunctionInfo<DoNewObjectFn>(DoNewObject, TailCall);

bool
ICNewObject_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    masm.push(BaselineStubReg); // stub.

    return tailCallVM(DoNewObjectInfo, masm);
}

//
// Compare_Fallback
//

static bool
DoCompareFallback(JSContext* cx, BaselineFrame* frame, ICCompare_Fallback* stub_, HandleValue lhs,
                  HandleValue rhs, MutableHandleValue ret)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICCompare_Fallback*> stub(frame, stub_);

    jsbytecode* pc = stub->icEntry()->pc(frame->script());
    JSOp op = JSOp(*pc);

    FallbackICSpew(cx, stub, "Compare(%s)", js_CodeName[op]);

    // Case operations in a CONDSWITCH are performing strict equality.
    if (op == JSOP_CASE)
        op = JSOP_STRICTEQ;

    // Don't pass lhs/rhs directly, we need the original values when
    // generating stubs.
    RootedValue lhsCopy(cx, lhs);
    RootedValue rhsCopy(cx, rhs);

    // Perform the compare operation.
    bool out;
    switch(op) {
      case JSOP_LT:
        if (!LessThan(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_LE:
        if (!LessThanOrEqual(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_GT:
        if (!GreaterThan(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_GE:
        if (!GreaterThanOrEqual(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_EQ:
        if (!LooselyEqual<true>(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_NE:
        if (!LooselyEqual<false>(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_STRICTEQ:
        if (!StrictlyEqual<true>(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_STRICTNE:
        if (!StrictlyEqual<false>(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      default:
        MOZ_ASSERT(!"Unhandled baseline compare op");
        return false;
    }

    ret.setBoolean(out);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Check to see if a new stub should be generated.
    if (stub->numOptimizedStubs() >= ICCompare_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with inert megamorphic stub.
        // But for now we just bail.
        return true;
    }

    JSScript* script = frame->script();

    // Try to generate new stubs.
    if (lhs.isInt32() && rhs.isInt32()) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Int32, Int32) stub", js_CodeName[op]);
        ICCompare_Int32::Compiler compiler(cx, op);
        ICStub* int32Stub = compiler.getStub(compiler.getStubSpace(script));
        if (!int32Stub)
            return false;

        stub->addNewStub(int32Stub);
        return true;
    }

    if (!cx->runtime()->jitSupportsFloatingPoint && (lhs.isNumber() || rhs.isNumber()))
        return true;

    if (lhs.isNumber() && rhs.isNumber()) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Number, Number) stub", js_CodeName[op]);

        // Unlink int32 stubs, it's faster to always use the double stub.
        stub->unlinkStubsWithKind(cx, ICStub::Compare_Int32);

        ICCompare_Double::Compiler compiler(cx, op);
        ICStub* doubleStub = compiler.getStub(compiler.getStubSpace(script));
        if (!doubleStub)
            return false;

        stub->addNewStub(doubleStub);
        return true;
    }

    if ((lhs.isNumber() && rhs.isUndefined()) ||
        (lhs.isUndefined() && rhs.isNumber()))
    {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(%s, %s) stub", js_CodeName[op],
                    rhs.isUndefined() ? "Number" : "Undefined",
                    rhs.isUndefined() ? "Undefined" : "Number");
        ICCompare_NumberWithUndefined::Compiler compiler(cx, op, lhs.isUndefined());
        ICStub* doubleStub = compiler.getStub(compiler.getStubSpace(script));
        if (!doubleStub)
            return false;

        stub->addNewStub(doubleStub);
        return true;
    }

    if (lhs.isBoolean() && rhs.isBoolean()) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Boolean, Boolean) stub", js_CodeName[op]);
        ICCompare_Boolean::Compiler compiler(cx, op);
        ICStub* booleanStub = compiler.getStub(compiler.getStubSpace(script));
        if (!booleanStub)
            return false;

        stub->addNewStub(booleanStub);
        return true;
    }

    if ((lhs.isBoolean() && rhs.isInt32()) || (lhs.isInt32() && rhs.isBoolean())) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(%s, %s) stub", js_CodeName[op],
                    rhs.isInt32() ? "Boolean" : "Int32",
                    rhs.isInt32() ? "Int32" : "Boolean");
        ICCompare_Int32WithBoolean::Compiler compiler(cx, op, lhs.isInt32());
        ICStub* optStub = compiler.getStub(compiler.getStubSpace(script));
        if (!optStub)
            return false;

        stub->addNewStub(optStub);
        return true;
    }

    if (IsEqualityOp(op)) {
        if (lhs.isString() && rhs.isString() && !stub->hasStub(ICStub::Compare_String)) {
            JitSpew(JitSpew_BaselineIC, "  Generating %s(String, String) stub", js_CodeName[op]);
            ICCompare_String::Compiler compiler(cx, op);
            ICStub* stringStub = compiler.getStub(compiler.getStubSpace(script));
            if (!stringStub)
                return false;

            stub->addNewStub(stringStub);
            return true;
        }

        if (lhs.isObject() && rhs.isObject()) {
            MOZ_ASSERT(!stub->hasStub(ICStub::Compare_Object));
            JitSpew(JitSpew_BaselineIC, "  Generating %s(Object, Object) stub", js_CodeName[op]);
            ICCompare_Object::Compiler compiler(cx, op);
            ICStub* objectStub = compiler.getStub(compiler.getStubSpace(script));
            if (!objectStub)
                return false;

            stub->addNewStub(objectStub);
            return true;
        }

        if ((lhs.isObject() || lhs.isNull() || lhs.isUndefined()) &&
            (rhs.isObject() || rhs.isNull() || rhs.isUndefined()) &&
            !stub->hasStub(ICStub::Compare_ObjectWithUndefined))
        {
            JitSpew(JitSpew_BaselineIC, "  Generating %s(Obj/Null/Undef, Obj/Null/Undef) stub",
                    js_CodeName[op]);
            bool lhsIsUndefined = lhs.isNull() || lhs.isUndefined();
            bool compareWithNull = lhs.isNull() || rhs.isNull();
            ICCompare_ObjectWithUndefined::Compiler compiler(cx, op,
                                                             lhsIsUndefined, compareWithNull);
            ICStub* objectStub = compiler.getStub(compiler.getStubSpace(script));
            if (!objectStub)
                return false;

            stub->addNewStub(objectStub);
            return true;
        }
    }

    stub->noteUnoptimizableAccess();

    return true;
}

typedef bool (*DoCompareFallbackFn)(JSContext*, BaselineFrame*, ICCompare_Fallback*,
                                    HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoCompareFallbackInfo =
    FunctionInfo<DoCompareFallbackFn>(DoCompareFallback, TailCall, PopValues(2));

bool
ICCompare_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
    return tailCallVM(DoCompareFallbackInfo, masm);
}

//
// Compare_String
//

bool
ICCompare_String::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestString(Assembler::NotEqual, R0, &failure);
    masm.branchTestString(Assembler::NotEqual, R1, &failure);

    MOZ_ASSERT(IsEqualityOp(op));

    Register left = masm.extractString(R0, ExtractTemp0);
    Register right = masm.extractString(R1, ExtractTemp1);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    masm.compareStrings(op, left, right, scratchReg, &failure);
    masm.tagValue(JSVAL_TYPE_BOOLEAN, scratchReg, R0);
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_Boolean
//

bool
ICCompare_Boolean::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestBoolean(Assembler::NotEqual, R0, &failure);
    masm.branchTestBoolean(Assembler::NotEqual, R1, &failure);

    Register left = masm.extractInt32(R0, ExtractTemp0);
    Register right = masm.extractInt32(R1, ExtractTemp1);

    // Compare payload regs of R0 and R1.
    Assembler::Condition cond = JSOpToCondition(op, /* signed = */true);
    masm.cmp32Set(cond, left, right, left);

    // Box the result and return
    masm.tagValue(JSVAL_TYPE_BOOLEAN, left, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_NumberWithUndefined
//

bool
ICCompare_NumberWithUndefined::Compiler::generateStubCode(MacroAssembler& masm)
{
    ValueOperand numberOperand, undefinedOperand;
    if (lhsIsUndefined) {
        numberOperand = R1;
        undefinedOperand = R0;
    } else {
        numberOperand = R0;
        undefinedOperand = R1;
    }

    Label failure;
    masm.branchTestNumber(Assembler::NotEqual, numberOperand, &failure);
    masm.branchTestUndefined(Assembler::NotEqual, undefinedOperand, &failure);

    // Comparing a number with undefined will always be true for NE/STRICTNE,
    // and always be false for other compare ops.
    masm.moveValue(BooleanValue(op == JSOP_NE || op == JSOP_STRICTNE), R0);

    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_Object
//

bool
ICCompare_Object::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    MOZ_ASSERT(IsEqualityOp(op));

    Register left = masm.extractObject(R0, ExtractTemp0);
    Register right = masm.extractObject(R1, ExtractTemp1);

    Label ifTrue;
    masm.branchPtr(JSOpToCondition(op, /* signed = */true), left, right, &ifTrue);

    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    masm.bind(&ifTrue);
    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_ObjectWithUndefined
//

bool
ICCompare_ObjectWithUndefined::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(IsEqualityOp(op));

    ValueOperand objectOperand, undefinedOperand;
    if (lhsIsUndefined) {
        objectOperand = R1;
        undefinedOperand = R0;
    } else {
        objectOperand = R0;
        undefinedOperand = R1;
    }

    Label failure;
    if (compareWithNull)
        masm.branchTestNull(Assembler::NotEqual, undefinedOperand, &failure);
    else
        masm.branchTestUndefined(Assembler::NotEqual, undefinedOperand, &failure);

    Label notObject;
    masm.branchTestObject(Assembler::NotEqual, objectOperand, &notObject);

    if (op == JSOP_STRICTEQ || op == JSOP_STRICTNE) {
        // obj !== undefined for all objects.
        masm.moveValue(BooleanValue(op == JSOP_STRICTNE), R0);
        EmitReturnFromIC(masm);
    } else {
        // obj != undefined only where !obj->getClass()->emulatesUndefined()
        Label emulatesUndefined;
        Register obj = masm.extractObject(objectOperand, ExtractTemp0);
        masm.loadPtr(Address(obj, JSObject::offsetOfGroup()), obj);
        masm.loadPtr(Address(obj, ObjectGroup::offsetOfClasp()), obj);
        masm.branchTest32(Assembler::NonZero,
                          Address(obj, Class::offsetOfFlags()),
                          Imm32(JSCLASS_EMULATES_UNDEFINED),
                          &emulatesUndefined);
        masm.moveValue(BooleanValue(op == JSOP_NE), R0);
        EmitReturnFromIC(masm);
        masm.bind(&emulatesUndefined);
        masm.moveValue(BooleanValue(op == JSOP_EQ), R0);
        EmitReturnFromIC(masm);
    }

    masm.bind(&notObject);

    // Also support null == null or undefined == undefined comparisons.
    if (compareWithNull)
        masm.branchTestNull(Assembler::NotEqual, objectOperand, &failure);
    else
        masm.branchTestUndefined(Assembler::NotEqual, objectOperand, &failure);

    masm.moveValue(BooleanValue(op == JSOP_STRICTEQ || op == JSOP_EQ), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_Int32WithBoolean
//

bool
ICCompare_Int32WithBoolean::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    ValueOperand int32Val;
    ValueOperand boolVal;
    if (lhsIsInt32_) {
        int32Val = R0;
        boolVal = R1;
    } else {
        boolVal = R0;
        int32Val = R1;
    }
    masm.branchTestBoolean(Assembler::NotEqual, boolVal, &failure);
    masm.branchTestInt32(Assembler::NotEqual, int32Val, &failure);

    if (op_ == JSOP_STRICTEQ || op_ == JSOP_STRICTNE) {
        // Ints and booleans are never strictly equal, always strictly not equal.
        masm.moveValue(BooleanValue(op_ == JSOP_STRICTNE), R0);
        EmitReturnFromIC(masm);
    } else {
        Register boolReg = masm.extractBoolean(boolVal, ExtractTemp0);
        Register int32Reg = masm.extractInt32(int32Val, ExtractTemp1);

        // Compare payload regs of R0 and R1.
        Assembler::Condition cond = JSOpToCondition(op_, /* signed = */true);
        masm.cmp32Set(cond, (lhsIsInt32_ ? int32Reg : boolReg),
                      (lhsIsInt32_ ? boolReg : int32Reg), R0.scratchReg());

        // Box the result and return
        masm.tagValue(JSVAL_TYPE_BOOLEAN, R0.scratchReg(), R0);
        EmitReturnFromIC(masm);
    }

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// ToBool_Fallback
//

static bool
DoToBoolFallback(JSContext* cx, BaselineFrame* frame, ICToBool_Fallback* stub, HandleValue arg,
                 MutableHandleValue ret)
{
    FallbackICSpew(cx, stub, "ToBool");

    bool cond = ToBoolean(arg);
    ret.setBoolean(cond);

    // Check to see if a new stub should be generated.
    if (stub->numOptimizedStubs() >= ICToBool_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with inert megamorphic stub.
        // But for now we just bail.
        return true;
    }

    MOZ_ASSERT(!arg.isBoolean());

    JSScript* script = frame->script();

    // Try to generate new stubs.
    if (arg.isInt32()) {
        JitSpew(JitSpew_BaselineIC, "  Generating ToBool(Int32) stub.");
        ICToBool_Int32::Compiler compiler(cx);
        ICStub* int32Stub = compiler.getStub(compiler.getStubSpace(script));
        if (!int32Stub)
            return false;

        stub->addNewStub(int32Stub);
        return true;
    }

    if (arg.isDouble() && cx->runtime()->jitSupportsFloatingPoint) {
        JitSpew(JitSpew_BaselineIC, "  Generating ToBool(Double) stub.");
        ICToBool_Double::Compiler compiler(cx);
        ICStub* doubleStub = compiler.getStub(compiler.getStubSpace(script));
        if (!doubleStub)
            return false;

        stub->addNewStub(doubleStub);
        return true;
    }

    if (arg.isString()) {
        JitSpew(JitSpew_BaselineIC, "  Generating ToBool(String) stub");
        ICToBool_String::Compiler compiler(cx);
        ICStub* stringStub = compiler.getStub(compiler.getStubSpace(script));
        if (!stringStub)
            return false;

        stub->addNewStub(stringStub);
        return true;
    }

    if (arg.isNull() || arg.isUndefined()) {
        ICToBool_NullUndefined::Compiler compiler(cx);
        ICStub* nilStub = compiler.getStub(compiler.getStubSpace(script));
        if (!nilStub)
            return false;

        stub->addNewStub(nilStub);
        return true;
    }

    if (arg.isObject()) {
        JitSpew(JitSpew_BaselineIC, "  Generating ToBool(Object) stub.");
        ICToBool_Object::Compiler compiler(cx);
        ICStub* objStub = compiler.getStub(compiler.getStubSpace(script));
        if (!objStub)
            return false;

        stub->addNewStub(objStub);
        return true;
    }

    return true;
}

typedef bool (*pf)(JSContext*, BaselineFrame*, ICToBool_Fallback*, HandleValue,
                   MutableHandleValue);
static const VMFunction fun = FunctionInfo<pf>(DoToBoolFallback, TailCall);

bool
ICToBool_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Push arguments.
    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(fun, masm);
}

//
// ToBool_Int32
//

bool
ICToBool_Int32::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestInt32(Assembler::NotEqual, R0, &failure);

    Label ifFalse;
    masm.branchTestInt32Truthy(false, R0, &ifFalse);

    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    masm.bind(&ifFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// ToBool_String
//

bool
ICToBool_String::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestString(Assembler::NotEqual, R0, &failure);

    Label ifFalse;
    masm.branchTestStringTruthy(false, R0, &ifFalse);

    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    masm.bind(&ifFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// ToBool_NullUndefined
//

bool
ICToBool_NullUndefined::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure, ifFalse;
    masm.branchTestNull(Assembler::Equal, R0, &ifFalse);
    masm.branchTestUndefined(Assembler::NotEqual, R0, &failure);

    masm.bind(&ifFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// ToBool_Double
//

bool
ICToBool_Double::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure, ifTrue;
    masm.branchTestDouble(Assembler::NotEqual, R0, &failure);
    masm.unboxDouble(R0, FloatReg0);
    masm.branchTestDoubleTruthy(true, FloatReg0, &ifTrue);

    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    masm.bind(&ifTrue);
    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// ToBool_Object
//

bool
ICToBool_Object::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure, ifFalse, slowPath;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    Register objReg = masm.extractObject(R0, ExtractTemp0);
    Register scratch = R1.scratchReg();
    masm.branchTestObjectTruthy(false, objReg, scratch, &slowPath, &ifFalse);

    // If object doesn't emulate undefined, it evaulates to true.
    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    masm.bind(&ifFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    masm.bind(&slowPath);
    masm.setupUnalignedABICall(1, scratch);
    masm.passABIArg(objReg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::EmulatesUndefined));
    masm.convertBoolToInt32(ReturnReg, ReturnReg);
    masm.xor32(Imm32(1), ReturnReg);
    masm.tagValue(JSVAL_TYPE_BOOLEAN, ReturnReg, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// ToNumber_Fallback
//

static bool
DoToNumberFallback(JSContext* cx, ICToNumber_Fallback* stub, HandleValue arg, MutableHandleValue ret)
{
    FallbackICSpew(cx, stub, "ToNumber");
    ret.set(arg);
    return ToNumber(cx, ret);
}

typedef bool (*DoToNumberFallbackFn)(JSContext*, ICToNumber_Fallback*, HandleValue, MutableHandleValue);
static const VMFunction DoToNumberFallbackInfo =
    FunctionInfo<DoToNumberFallbackFn>(DoToNumberFallback, TailCall, PopValues(1));

bool
ICToNumber_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);

    // Push arguments.
    masm.pushValue(R0);
    masm.push(BaselineStubReg);

    return tailCallVM(DoToNumberFallbackInfo, masm);
}

//
// BinaryArith_Fallback
//

static bool
DoBinaryArithFallback(JSContext* cx, BaselineFrame* frame, ICBinaryArith_Fallback* stub_,
                      HandleValue lhs, HandleValue rhs, MutableHandleValue ret)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICBinaryArith_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "BinaryArith(%s,%d,%d)", js_CodeName[op],
            int(lhs.isDouble() ? JSVAL_TYPE_DOUBLE : lhs.extractNonDoubleType()),
            int(rhs.isDouble() ? JSVAL_TYPE_DOUBLE : rhs.extractNonDoubleType()));

    // Don't pass lhs/rhs directly, we need the original values when
    // generating stubs.
    RootedValue lhsCopy(cx, lhs);
    RootedValue rhsCopy(cx, rhs);

    // Perform the compare operation.
    switch(op) {
      case JSOP_ADD:
        // Do an add.
        if (!AddValues(cx, &lhsCopy, &rhsCopy, ret))
            return false;
        break;
      case JSOP_SUB:
        if (!SubValues(cx, &lhsCopy, &rhsCopy, ret))
            return false;
        break;
      case JSOP_MUL:
        if (!MulValues(cx, &lhsCopy, &rhsCopy, ret))
            return false;
        break;
      case JSOP_DIV:
        if (!DivValues(cx, &lhsCopy, &rhsCopy, ret))
            return false;
        break;
      case JSOP_MOD:
        if (!ModValues(cx, &lhsCopy, &rhsCopy, ret))
            return false;
        break;
      case JSOP_BITOR: {
        int32_t result;
        if (!BitOr(cx, lhs, rhs, &result))
            return false;
        ret.setInt32(result);
        break;
      }
      case JSOP_BITXOR: {
        int32_t result;
        if (!BitXor(cx, lhs, rhs, &result))
            return false;
        ret.setInt32(result);
        break;
      }
      case JSOP_BITAND: {
        int32_t result;
        if (!BitAnd(cx, lhs, rhs, &result))
            return false;
        ret.setInt32(result);
        break;
      }
      case JSOP_LSH: {
        int32_t result;
        if (!BitLsh(cx, lhs, rhs, &result))
            return false;
        ret.setInt32(result);
        break;
      }
      case JSOP_RSH: {
        int32_t result;
        if (!BitRsh(cx, lhs, rhs, &result))
            return false;
        ret.setInt32(result);
        break;
      }
      case JSOP_URSH: {
        if (!UrshOperation(cx, lhs, rhs, ret))
            return false;
        break;
      }
      default:
        MOZ_CRASH("Unhandled baseline arith op");
    }

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (ret.isDouble())
        stub->setSawDoubleResult();

    // Check to see if a new stub should be generated.
    if (stub->numOptimizedStubs() >= ICBinaryArith_Fallback::MAX_OPTIMIZED_STUBS) {
        stub->noteUnoptimizableOperands();
        return true;
    }

    // Handle string concat.
    if (op == JSOP_ADD) {
        if (lhs.isString() && rhs.isString()) {
            JitSpew(JitSpew_BaselineIC, "  Generating %s(String, String) stub", js_CodeName[op]);
            MOZ_ASSERT(ret.isString());
            ICBinaryArith_StringConcat::Compiler compiler(cx);
            ICStub* strcatStub = compiler.getStub(compiler.getStubSpace(script));
            if (!strcatStub)
                return false;
            stub->addNewStub(strcatStub);
            return true;
        }

        if ((lhs.isString() && rhs.isObject()) || (lhs.isObject() && rhs.isString())) {
            JitSpew(JitSpew_BaselineIC, "  Generating %s(%s, %s) stub", js_CodeName[op],
                    lhs.isString() ? "String" : "Object",
                    lhs.isString() ? "Object" : "String");
            MOZ_ASSERT(ret.isString());
            ICBinaryArith_StringObjectConcat::Compiler compiler(cx, lhs.isString());
            ICStub* strcatStub = compiler.getStub(compiler.getStubSpace(script));
            if (!strcatStub)
                return false;
            stub->addNewStub(strcatStub);
            return true;
        }
    }

    if (((lhs.isBoolean() && (rhs.isBoolean() || rhs.isInt32())) ||
         (rhs.isBoolean() && (lhs.isBoolean() || lhs.isInt32()))) &&
        (op == JSOP_ADD || op == JSOP_SUB || op == JSOP_BITOR || op == JSOP_BITAND ||
         op == JSOP_BITXOR))
    {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(%s, %s) stub", js_CodeName[op],
                lhs.isBoolean() ? "Boolean" : "Int32", rhs.isBoolean() ? "Boolean" : "Int32");
        ICBinaryArith_BooleanWithInt32::Compiler compiler(cx, op, lhs.isBoolean(), rhs.isBoolean());
        ICStub* arithStub = compiler.getStub(compiler.getStubSpace(script));
        if (!arithStub)
            return false;
        stub->addNewStub(arithStub);
        return true;
    }

    // Handle only int32 or double.
    if (!lhs.isNumber() || !rhs.isNumber()) {
        stub->noteUnoptimizableOperands();
        return true;
    }

    MOZ_ASSERT(ret.isNumber());

    if (lhs.isDouble() || rhs.isDouble() || ret.isDouble()) {
        if (!cx->runtime()->jitSupportsFloatingPoint)
            return true;

        switch (op) {
          case JSOP_ADD:
          case JSOP_SUB:
          case JSOP_MUL:
          case JSOP_DIV:
          case JSOP_MOD: {
            // Unlink int32 stubs, it's faster to always use the double stub.
            stub->unlinkStubsWithKind(cx, ICStub::BinaryArith_Int32);
            JitSpew(JitSpew_BaselineIC, "  Generating %s(Double, Double) stub", js_CodeName[op]);

            ICBinaryArith_Double::Compiler compiler(cx, op);
            ICStub* doubleStub = compiler.getStub(compiler.getStubSpace(script));
            if (!doubleStub)
                return false;
            stub->addNewStub(doubleStub);
            return true;
          }
          default:
            break;
        }
    }

    if (lhs.isInt32() && rhs.isInt32()) {
        bool allowDouble = ret.isDouble();
        if (allowDouble)
            stub->unlinkStubsWithKind(cx, ICStub::BinaryArith_Int32);
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Int32, Int32%s) stub", js_CodeName[op],
                allowDouble ? " => Double" : "");
        ICBinaryArith_Int32::Compiler compilerInt32(cx, op, allowDouble);
        ICStub* int32Stub = compilerInt32.getStub(compilerInt32.getStubSpace(script));
        if (!int32Stub)
            return false;
        stub->addNewStub(int32Stub);
        return true;
    }

    // Handle Double <BITOP> Int32 or Int32 <BITOP> Double case.
    if (((lhs.isDouble() && rhs.isInt32()) || (lhs.isInt32() && rhs.isDouble())) &&
        ret.isInt32())
    {
        switch(op) {
          case JSOP_BITOR:
          case JSOP_BITXOR:
          case JSOP_BITAND: {
            JitSpew(JitSpew_BaselineIC, "  Generating %s(%s, %s) stub", js_CodeName[op],
                        lhs.isDouble() ? "Double" : "Int32",
                        lhs.isDouble() ? "Int32" : "Double");
            ICBinaryArith_DoubleWithInt32::Compiler compiler(cx, op, lhs.isDouble());
            ICStub* optStub = compiler.getStub(compiler.getStubSpace(script));
            if (!optStub)
                return false;
            stub->addNewStub(optStub);
            return true;
          }
          default:
            break;
        }
    }

    stub->noteUnoptimizableOperands();
    return true;
}

typedef bool (*DoBinaryArithFallbackFn)(JSContext*, BaselineFrame*, ICBinaryArith_Fallback*,
                                        HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoBinaryArithFallbackInfo =
    FunctionInfo<DoBinaryArithFallbackFn>(DoBinaryArithFallback, TailCall, PopValues(2));

bool
ICBinaryArith_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoBinaryArithFallbackInfo, masm);
}

static bool
DoConcatStrings(JSContext* cx, HandleString lhs, HandleString rhs, MutableHandleValue res)
{
    JSString* result = ConcatStrings<CanGC>(cx, lhs, rhs);
    if (!result)
        return false;

    res.setString(result);
    return true;
}

typedef bool (*DoConcatStringsFn)(JSContext*, HandleString, HandleString, MutableHandleValue);
static const VMFunction DoConcatStringsInfo = FunctionInfo<DoConcatStringsFn>(DoConcatStrings, TailCall);

bool
ICBinaryArith_StringConcat::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestString(Assembler::NotEqual, R0, &failure);
    masm.branchTestString(Assembler::NotEqual, R1, &failure);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    masm.unboxString(R0, R0.scratchReg());
    masm.unboxString(R1, R1.scratchReg());

    masm.push(R1.scratchReg());
    masm.push(R0.scratchReg());
    if (!tailCallVM(DoConcatStringsInfo, masm))
        return false;

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

static JSString*
ConvertObjectToStringForConcat(JSContext* cx, HandleValue obj)
{
    MOZ_ASSERT(obj.isObject());
    RootedValue rootedObj(cx, obj);
    if (!ToPrimitive(cx, &rootedObj))
        return nullptr;
    return ToString<CanGC>(cx, rootedObj);
}

static bool
DoConcatStringObject(JSContext* cx, bool lhsIsString, HandleValue lhs, HandleValue rhs,
                     MutableHandleValue res)
{
    JSString* lstr = nullptr;
    JSString* rstr = nullptr;
    if (lhsIsString) {
        // Convert rhs first.
        MOZ_ASSERT(lhs.isString() && rhs.isObject());
        rstr = ConvertObjectToStringForConcat(cx, rhs);
        if (!rstr)
            return false;

        // lhs is already string.
        lstr = lhs.toString();
    } else {
        MOZ_ASSERT(rhs.isString() && lhs.isObject());
        // Convert lhs first.
        lstr = ConvertObjectToStringForConcat(cx, lhs);
        if (!lstr)
            return false;

        // rhs is already string.
        rstr = rhs.toString();
    }

    JSString* str = ConcatStrings<NoGC>(cx, lstr, rstr);
    if (!str) {
        RootedString nlstr(cx, lstr), nrstr(cx, rstr);
        str = ConcatStrings<CanGC>(cx, nlstr, nrstr);
        if (!str)
            return false;
    }

    // Technically, we need to call TypeScript::MonitorString for this PC, however
    // it was called when this stub was attached so it's OK.

    res.setString(str);
    return true;
}

typedef bool (*DoConcatStringObjectFn)(JSContext*, bool lhsIsString, HandleValue, HandleValue,
                                       MutableHandleValue);
static const VMFunction DoConcatStringObjectInfo =
    FunctionInfo<DoConcatStringObjectFn>(DoConcatStringObject, TailCall, PopValues(2));

bool
ICBinaryArith_StringObjectConcat::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    if (lhsIsString_) {
        masm.branchTestString(Assembler::NotEqual, R0, &failure);
        masm.branchTestObject(Assembler::NotEqual, R1, &failure);
    } else {
        masm.branchTestObject(Assembler::NotEqual, R0, &failure);
        masm.branchTestString(Assembler::NotEqual, R1, &failure);
    }

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Sync for the decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(Imm32(lhsIsString_));
    if (!tailCallVM(DoConcatStringObjectInfo, masm))
        return false;

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICBinaryArith_Double::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.ensureDouble(R0, FloatReg0, &failure);
    masm.ensureDouble(R1, FloatReg1, &failure);

    switch (op) {
      case JSOP_ADD:
        masm.addDouble(FloatReg1, FloatReg0);
        break;
      case JSOP_SUB:
        masm.subDouble(FloatReg1, FloatReg0);
        break;
      case JSOP_MUL:
        masm.mulDouble(FloatReg1, FloatReg0);
        break;
      case JSOP_DIV:
        masm.divDouble(FloatReg1, FloatReg0);
        break;
      case JSOP_MOD:
        masm.setupUnalignedABICall(2, R0.scratchReg());
        masm.passABIArg(FloatReg0, MoveOp::DOUBLE);
        masm.passABIArg(FloatReg1, MoveOp::DOUBLE);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, NumberMod), MoveOp::DOUBLE);
        MOZ_ASSERT(ReturnDoubleReg == FloatReg0);
        break;
      default:
        MOZ_CRASH("Unexpected op");
    }

    masm.boxDouble(FloatReg0, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICBinaryArith_BooleanWithInt32::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    if (lhsIsBool_)
        masm.branchTestBoolean(Assembler::NotEqual, R0, &failure);
    else
        masm.branchTestInt32(Assembler::NotEqual, R0, &failure);

    if (rhsIsBool_)
        masm.branchTestBoolean(Assembler::NotEqual, R1, &failure);
    else
        masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    Register lhsReg = lhsIsBool_ ? masm.extractBoolean(R0, ExtractTemp0)
                                 : masm.extractInt32(R0, ExtractTemp0);
    Register rhsReg = rhsIsBool_ ? masm.extractBoolean(R1, ExtractTemp1)
                                 : masm.extractInt32(R1, ExtractTemp1);

    MOZ_ASSERT(op_ == JSOP_ADD || op_ == JSOP_SUB ||
               op_ == JSOP_BITOR || op_ == JSOP_BITXOR || op_ == JSOP_BITAND);

    switch(op_) {
      case JSOP_ADD: {
        Label fixOverflow;

        masm.branchAdd32(Assembler::Overflow, rhsReg, lhsReg, &fixOverflow);
        masm.tagValue(JSVAL_TYPE_INT32, lhsReg, R0);
        EmitReturnFromIC(masm);

        masm.bind(&fixOverflow);
        masm.sub32(rhsReg, lhsReg);
        // Proceed to failure below.
        break;
      }
      case JSOP_SUB: {
        Label fixOverflow;

        masm.branchSub32(Assembler::Overflow, rhsReg, lhsReg, &fixOverflow);
        masm.tagValue(JSVAL_TYPE_INT32, lhsReg, R0);
        EmitReturnFromIC(masm);

        masm.bind(&fixOverflow);
        masm.add32(rhsReg, lhsReg);
        // Proceed to failure below.
        break;
      }
      case JSOP_BITOR: {
        masm.orPtr(rhsReg, lhsReg);
        masm.tagValue(JSVAL_TYPE_INT32, lhsReg, R0);
        EmitReturnFromIC(masm);
        break;
      }
      case JSOP_BITXOR: {
        masm.xorPtr(rhsReg, lhsReg);
        masm.tagValue(JSVAL_TYPE_INT32, lhsReg, R0);
        EmitReturnFromIC(masm);
        break;
      }
      case JSOP_BITAND: {
        masm.andPtr(rhsReg, lhsReg);
        masm.tagValue(JSVAL_TYPE_INT32, lhsReg, R0);
        EmitReturnFromIC(masm);
        break;
      }
      default:
       MOZ_CRASH("Unhandled op for BinaryArith_BooleanWithInt32.");
    }

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICBinaryArith_DoubleWithInt32::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(op == JSOP_BITOR || op == JSOP_BITAND || op == JSOP_BITXOR);

    Label failure;
    Register intReg;
    Register scratchReg;
    if (lhsIsDouble_) {
        masm.branchTestDouble(Assembler::NotEqual, R0, &failure);
        masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
        intReg = masm.extractInt32(R1, ExtractTemp0);
        masm.unboxDouble(R0, FloatReg0);
        scratchReg = R0.scratchReg();
    } else {
        masm.branchTestInt32(Assembler::NotEqual, R0, &failure);
        masm.branchTestDouble(Assembler::NotEqual, R1, &failure);
        intReg = masm.extractInt32(R0, ExtractTemp0);
        masm.unboxDouble(R1, FloatReg0);
        scratchReg = R1.scratchReg();
    }

    // Truncate the double to an int32.
    {
        Label doneTruncate;
        Label truncateABICall;
        masm.branchTruncateDouble(FloatReg0, scratchReg, &truncateABICall);
        masm.jump(&doneTruncate);

        masm.bind(&truncateABICall);
        masm.push(intReg);
        masm.setupUnalignedABICall(1, scratchReg);
        masm.passABIArg(FloatReg0, MoveOp::DOUBLE);
        masm.callWithABI(mozilla::BitwiseCast<void*, int32_t(*)(double)>(JS::ToInt32));
        masm.storeCallResult(scratchReg);
        masm.pop(intReg);

        masm.bind(&doneTruncate);
    }

    Register intReg2 = scratchReg;
    // All handled ops commute, so no need to worry about ordering.
    switch(op) {
      case JSOP_BITOR:
        masm.orPtr(intReg, intReg2);
        break;
      case JSOP_BITXOR:
        masm.xorPtr(intReg, intReg2);
        break;
      case JSOP_BITAND:
        masm.andPtr(intReg, intReg2);
        break;
      default:
       MOZ_CRASH("Unhandled op for BinaryArith_DoubleWithInt32.");
    }
    masm.tagValue(JSVAL_TYPE_INT32, intReg2, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// UnaryArith_Fallback
//

static bool
DoUnaryArithFallback(JSContext* cx, BaselineFrame* frame, ICUnaryArith_Fallback* stub_,
                     HandleValue val, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICUnaryArith_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "UnaryArith(%s)", js_CodeName[op]);

    switch (op) {
      case JSOP_BITNOT: {
        int32_t result;
        if (!BitNot(cx, val, &result))
            return false;
        res.setInt32(result);
        break;
      }
      case JSOP_NEG:
        if (!NegOperation(cx, script, pc, val, res))
            return false;
        break;
      default:
        MOZ_CRASH("Unexpected op");
    }

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (res.isDouble())
        stub->setSawDoubleResult();

    if (stub->numOptimizedStubs() >= ICUnaryArith_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard/replace stubs.
        return true;
    }

    if (val.isInt32() && res.isInt32()) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Int32 => Int32) stub", js_CodeName[op]);
        ICUnaryArith_Int32::Compiler compiler(cx, op);
        ICStub* int32Stub = compiler.getStub(compiler.getStubSpace(script));
        if (!int32Stub)
            return false;
        stub->addNewStub(int32Stub);
        return true;
    }

    if (val.isNumber() && res.isNumber() && cx->runtime()->jitSupportsFloatingPoint) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Number => Number) stub", js_CodeName[op]);

        // Unlink int32 stubs, the double stub handles both cases and TI specializes for both.
        stub->unlinkStubsWithKind(cx, ICStub::UnaryArith_Int32);

        ICUnaryArith_Double::Compiler compiler(cx, op);
        ICStub* doubleStub = compiler.getStub(compiler.getStubSpace(script));
        if (!doubleStub)
            return false;
        stub->addNewStub(doubleStub);
        return true;
    }

    return true;
}

typedef bool (*DoUnaryArithFallbackFn)(JSContext*, BaselineFrame*, ICUnaryArith_Fallback*,
                                       HandleValue, MutableHandleValue);
static const VMFunction DoUnaryArithFallbackInfo =
    FunctionInfo<DoUnaryArithFallbackFn>(DoUnaryArithFallback, TailCall, PopValues(1));

bool
ICUnaryArith_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);

    // Push arguments.
    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoUnaryArithFallbackInfo, masm);
}

bool
ICUnaryArith_Double::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.ensureDouble(R0, FloatReg0, &failure);

    MOZ_ASSERT(op == JSOP_NEG || op == JSOP_BITNOT);

    if (op == JSOP_NEG) {
        masm.negateDouble(FloatReg0);
        masm.boxDouble(FloatReg0, R0);
    } else {
        // Truncate the double to an int32.
        Register scratchReg = R1.scratchReg();

        Label doneTruncate;
        Label truncateABICall;
        masm.branchTruncateDouble(FloatReg0, scratchReg, &truncateABICall);
        masm.jump(&doneTruncate);

        masm.bind(&truncateABICall);
        masm.setupUnalignedABICall(1, scratchReg);
        masm.passABIArg(FloatReg0, MoveOp::DOUBLE);
        masm.callWithABI(BitwiseCast<void*, int32_t(*)(double)>(JS::ToInt32));
        masm.storeCallResult(scratchReg);

        masm.bind(&doneTruncate);
        masm.not32(scratchReg);
        masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
    }

    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// GetElem_Fallback
//

static void GetFixedOrDynamicSlotOffset(NativeObject* obj, uint32_t slot,
                                        bool* isFixed, uint32_t* offset)
{
    MOZ_ASSERT(isFixed);
    MOZ_ASSERT(offset);
    *isFixed = obj->isFixedSlot(slot);
    *offset = *isFixed ? NativeObject::getFixedSlotOffset(slot)
                       : obj->dynamicSlotIndex(slot) * sizeof(Value);
}

static JSObject*
GetDOMProxyProto(JSObject* obj)
{
    MOZ_ASSERT(IsCacheableDOMProxy(obj));
    return obj->getTaggedProto().toObjectOrNull();
}

// Callers are expected to have already guarded on the shape of the
// object, which guarantees the object is a DOM proxy.
static void
CheckDOMProxyExpandoDoesNotShadow(JSContext* cx, MacroAssembler& masm, Register object,
                                  const Address& checkExpandoShapeAddr,
                                  Address* expandoAndGenerationAddr,
                                  Address* generationAddr,
                                  Register scratch,
                                  GeneralRegisterSet& domProxyRegSet,
                                  Label* checkFailed)
{
    // Guard that the object does not have expando properties, or has an expando
    // which is known to not have the desired property.

    // For the remaining code, we need to reserve some registers to load a value.
    // This is ugly, but unavoidable.
    ValueOperand tempVal = domProxyRegSet.takeAnyValue();
    masm.pushValue(tempVal);

    Label failDOMProxyCheck;
    Label domProxyOk;

    masm.loadPtr(Address(object, ProxyObject::offsetOfValues()), scratch);
    Address expandoAddr(scratch, ProxyObject::offsetOfExtraSlotInValues(GetDOMProxyExpandoSlot()));

    if (expandoAndGenerationAddr) {
        MOZ_ASSERT(generationAddr);

        masm.loadPtr(*expandoAndGenerationAddr, tempVal.scratchReg());
        masm.branchPrivatePtr(Assembler::NotEqual, expandoAddr, tempVal.scratchReg(),
                              &failDOMProxyCheck);

        masm.load32(*generationAddr, scratch);
        masm.branch32(Assembler::NotEqual,
                      Address(tempVal.scratchReg(), offsetof(ExpandoAndGeneration, generation)),
                      scratch, &failDOMProxyCheck);

        masm.loadValue(Address(tempVal.scratchReg(), 0), tempVal);
    } else {
        masm.loadValue(expandoAddr, tempVal);
    }

    // If the incoming object does not have an expando object then we're sure we're not
    // shadowing.
    masm.branchTestUndefined(Assembler::Equal, tempVal, &domProxyOk);

    // The reference object used to generate this check may not have had an
    // expando object at all, in which case the presence of a non-undefined
    // expando value in the incoming object is automatically a failure.
    masm.loadPtr(checkExpandoShapeAddr, scratch);
    masm.branchPtr(Assembler::Equal, scratch, ImmPtr(nullptr), &failDOMProxyCheck);

    // Otherwise, ensure that the incoming object has an object for its expando value and that
    // the shape matches.
    masm.branchTestObject(Assembler::NotEqual, tempVal, &failDOMProxyCheck);
    Register objReg = masm.extractObject(tempVal, tempVal.scratchReg());
    masm.branchTestObjShape(Assembler::Equal, objReg, scratch, &domProxyOk);

    // Failure case: restore the tempVal registers and jump to failures.
    masm.bind(&failDOMProxyCheck);
    masm.popValue(tempVal);
    masm.jump(checkFailed);

    // Success case: restore the tempval and proceed.
    masm.bind(&domProxyOk);
    masm.popValue(tempVal);
}

// Look up a property's shape on an object, being careful never to do any effectful
// operations.  This procedure not yielding a shape should not be taken as a lack of
// existence of the property on the object.
static bool
EffectlesslyLookupProperty(JSContext* cx, HandleObject obj, HandlePropertyName name,
                           MutableHandleObject holder, MutableHandleShape shape,
                           bool* checkDOMProxy=nullptr,
                           DOMProxyShadowsResult* shadowsResult=nullptr,
                           bool* domProxyHasGeneration=nullptr)
{
    shape.set(nullptr);
    holder.set(nullptr);

    if (checkDOMProxy) {
        *checkDOMProxy = false;
        *shadowsResult = ShadowCheckFailed;
    }

    // Check for list base if asked to.
    RootedObject checkObj(cx, obj);
    if (checkDOMProxy && IsCacheableDOMProxy(obj)) {
        MOZ_ASSERT(domProxyHasGeneration);
        MOZ_ASSERT(shadowsResult);

        *checkDOMProxy = true;
        if (obj->hasUncacheableProto())
            return true;

        RootedId id(cx, NameToId(name));
        *shadowsResult = GetDOMProxyShadowsCheck()(cx, obj, id);
        if (*shadowsResult == ShadowCheckFailed)
            return false;

        if (*shadowsResult == Shadows) {
            holder.set(obj);
            return true;
        }

        *domProxyHasGeneration = (*shadowsResult == DoesntShadowUnique);

        checkObj = GetDOMProxyProto(obj);
        if (!checkObj)
            return true;
    } else if (!obj->isNative()) {
        return true;
    }

    if (checkObj->hasIdempotentProtoChain()) {
        if (!LookupProperty(cx, checkObj, name, holder, shape))
            return false;
    } else if (checkObj->isNative()) {
        shape.set(checkObj->as<NativeObject>().lookup(cx, NameToId(name)));
        if (shape)
            holder.set(checkObj);
    }
    return true;
}

static bool
CheckHasNoSuchProperty(JSContext* cx, HandleObject obj, HandlePropertyName name,
                       MutableHandleObject lastProto, size_t* protoChainDepthOut)
{
    MOZ_ASSERT(protoChainDepthOut != nullptr);

    size_t depth = 0;
    RootedObject curObj(cx, obj);
    while (curObj) {
        if (!curObj->isNative())
            return false;

        // Don't handle proto chains with resolve hooks.
        if (curObj->getClass()->resolve)
            return false;

        Shape* shape = curObj->as<NativeObject>().lookup(cx, NameToId(name));
        if (shape)
            return false;

        JSObject* proto = curObj->getTaggedProto().toObjectOrNull();
        if (!proto)
            break;

        curObj = proto;
        depth++;
    }

    lastProto.set(curObj);
    *protoChainDepthOut = depth;
    return true;
}

static bool
IsCacheableProtoChain(JSObject* obj, JSObject* holder, bool isDOMProxy=false)
{
    MOZ_ASSERT_IF(isDOMProxy, IsCacheableDOMProxy(obj));

    if (!isDOMProxy && !obj->isNative()) {
        if (!obj->is<UnboxedPlainObject>() || obj == holder)
            return false;
    }

    // Don't handle objects which require a prototype guard. This should
    // be uncommon so handling it is likely not worth the complexity.
    if (obj->hasUncacheableProto())
        return false;

    JSObject* cur = obj;
    while (cur != holder) {
        // We cannot assume that we find the holder object on the prototype
        // chain and must check for null proto. The prototype chain can be
        // altered during the lookupProperty call.
        JSObject* proto;
        if (isDOMProxy && cur == obj)
            proto = cur->getTaggedProto().toObjectOrNull();
        else
            proto = cur->getProto();

        if (!proto || !proto->isNative())
            return false;

        if (proto->hasUncacheableProto())
            return false;

        cur = proto;
    }
    return true;
}

static bool
IsCacheableGetPropReadSlot(JSObject* obj, JSObject* holder, Shape* shape, bool isDOMProxy=false)
{
    if (!shape || !IsCacheableProtoChain(obj, holder, isDOMProxy))
        return false;

    if (!shape->hasSlot() || !shape->hasDefaultGetter())
        return false;

    return true;
}

static bool
IsCacheableGetPropCall(JSContext* cx, JSObject* obj, JSObject* holder, Shape* shape,
                       bool* isScripted, bool* isTemporarilyUnoptimizable, bool isDOMProxy=false)
{
    MOZ_ASSERT(isScripted);

    if (!shape || !IsCacheableProtoChain(obj, holder, isDOMProxy))
        return false;

    if (shape->hasSlot() || shape->hasDefaultGetter())
        return false;

    if (!shape->hasGetterValue())
        return false;

    if (!shape->getterValue().isObject() || !shape->getterObject()->is<JSFunction>())
        return false;

    JSFunction* func = &shape->getterObject()->as<JSFunction>();
    if (func->isNative()) {
        *isScripted = false;
        return true;
    }

    if (!func->hasJITCode()) {
        *isTemporarilyUnoptimizable = true;
        return false;
    }

    *isScripted = true;
    return true;
}

static bool
IsCacheableSetPropWriteSlot(JSObject* obj, Shape* oldShape, JSObject* holder, Shape* shape)
{
    if (!shape)
        return false;

    // Object shape must not have changed during the property set.
    if (obj->lastProperty() != oldShape)
        return false;

    // Currently we only optimize direct writes.
    if (obj != holder)
        return false;

    if (!shape->hasSlot() || !shape->hasDefaultSetter() || !shape->writable())
        return false;

    return true;
}

static bool
IsCacheableSetPropAddSlot(JSContext* cx, HandleObject obj, HandleShape oldShape, uint32_t oldSlots,
                          HandleId id, HandleObject holder, HandleShape shape,
                          size_t* protoChainDepth)
{
    if (!shape)
        return false;

    // Property must be set directly on object, and be last added property of object.
    if (obj != holder || shape != obj->lastProperty())
        return false;

    // Object must be extensible, oldShape must be immediate parent of curShape.
    if (!obj->nonProxyIsExtensible() || obj->lastProperty()->previous() != oldShape)
        return false;

    // Basic shape checks.
    if (shape->inDictionary() || !shape->hasSlot() || !shape->hasDefaultSetter() ||
        !shape->writable())
    {
        return false;
    }

    // If object has a resolve hook, don't inline
    if (obj->getClass()->resolve)
        return false;

    size_t chainDepth = 0;
    // walk up the object prototype chain and ensure that all prototypes
    // are native, and that all prototypes have setter defined on the property
    for (JSObject* proto = obj->getProto(); proto; proto = proto->getProto()) {
        chainDepth++;
        // if prototype is non-native, don't optimize
        if (!proto->isNative())
            return false;

        // if prototype defines this property in a non-plain way, don't optimize
        Shape* protoShape = proto->as<NativeObject>().lookup(cx, id);
        if (protoShape && !protoShape->hasDefaultSetter())
            return false;

        // Otherise, if there's no such property, watch out for a resolve hook that would need
        // to be invoked and thus prevent inlining of property addition.
        if (proto->getClass()->resolve)
             return false;
    }

    // Only add a IC entry if the dynamic slots didn't change when the shapes
    // changed.  Need to ensure that a shape change for a subsequent object
    // won't involve reallocating the slot array.
    if (obj->as<NativeObject>().numDynamicSlots() != oldSlots)
        return false;

    *protoChainDepth = chainDepth;
    return true;
}

static bool
IsCacheableSetPropCall(JSContext* cx, JSObject* obj, JSObject* holder, Shape* shape,
                       bool* isScripted, bool* isTemporarilyUnoptimizable)
{
    MOZ_ASSERT(isScripted);

    // Currently we only optimize setter calls for setters bound on prototypes.
    if (obj == holder)
        return false;

    if (!shape || !IsCacheableProtoChain(obj, holder))
        return false;

    if (shape->hasSlot() || shape->hasDefaultSetter())
        return false;

    if (!shape->hasSetterValue())
        return false;

    if (!shape->setterValue().isObject() || !shape->setterObject()->is<JSFunction>())
        return false;

    JSFunction* func = &shape->setterObject()->as<JSFunction>();

    if (func->isNative()) {
        *isScripted = false;
        return true;
    }

    if (!func->hasJITCode()) {
        *isTemporarilyUnoptimizable = true;
        return false;
    }

    *isScripted = true;
    return true;
}

static bool
LookupNoSuchMethodHandler(JSContext* cx, HandleObject obj, HandleValue id,
                          MutableHandleValue result)
{
    return OnUnknownMethod(cx, obj, id, result);
}

typedef bool (*LookupNoSuchMethodHandlerFn)(JSContext*, HandleObject, HandleValue,
                                            MutableHandleValue);
static const VMFunction LookupNoSuchMethodHandlerInfo =
    FunctionInfo<LookupNoSuchMethodHandlerFn>(LookupNoSuchMethodHandler);

static bool
GetElemNativeStubExists(ICGetElem_Fallback* stub, HandleObject obj, HandleObject holder,
                        HandlePropertyName propName, bool needsAtomize)
{
    bool indirect = (obj.get() != holder.get());

    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        if (iter->kind() != ICStub::GetElem_NativeSlot &&
            iter->kind() != ICStub::GetElem_NativePrototypeSlot &&
            iter->kind() != ICStub::GetElem_NativePrototypeCallNative &&
            iter->kind() != ICStub::GetElem_NativePrototypeCallScripted)
        {
            continue;
        }

        if (indirect && (iter->kind() != ICStub::GetElem_NativePrototypeSlot &&
                         iter->kind() != ICStub::GetElem_NativePrototypeCallNative &&
                         iter->kind() != ICStub::GetElem_NativePrototypeCallScripted))
        {
            continue;
        }

        ICGetElemNativeStub* getElemNativeStub = reinterpret_cast<ICGetElemNativeStub*>(*iter);
        if (propName != getElemNativeStub->name())
            continue;

        if (obj->lastProperty() != getElemNativeStub->shape())
            continue;

        // If the new stub needs atomization, and the old stub doesn't atomize, then
        // an appropriate stub doesn't exist.
        if (needsAtomize && !getElemNativeStub->needsAtomize())
            continue;

        // For prototype gets, check the holder and holder shape.
        if (indirect) {
            if (iter->isGetElem_NativePrototypeSlot()) {
                ICGetElem_NativePrototypeSlot* protoStub = iter->toGetElem_NativePrototypeSlot();

                if (holder != protoStub->holder())
                    continue;

                if (holder->lastProperty() != protoStub->holderShape())
                    continue;
            } else {
                MOZ_ASSERT(iter->isGetElem_NativePrototypeCallNative() ||
                           iter->isGetElem_NativePrototypeCallScripted());

                ICGetElemNativePrototypeCallStub* protoStub =
                    reinterpret_cast<ICGetElemNativePrototypeCallStub*>(*iter);

                if (holder != protoStub->holder())
                    continue;

                if (holder->lastProperty() != protoStub->holderShape())
                    continue;
            }
        }

        return true;
    }
    return false;
}

static void
RemoveExistingGetElemNativeStubs(JSContext* cx, ICGetElem_Fallback* stub, HandleObject obj,
                                 HandleObject holder, HandlePropertyName propName,
                                 bool needsAtomize)
{
    bool indirect = (obj.get() != holder.get());

    for (ICStubIterator iter = stub->beginChain(); !iter.atEnd(); iter++) {
        switch (iter->kind()) {
          case ICStub::GetElem_NativeSlot:
            if (indirect)
                continue;
          case ICStub::GetElem_NativePrototypeSlot:
          case ICStub::GetElem_NativePrototypeCallNative:
          case ICStub::GetElem_NativePrototypeCallScripted:
            break;
          default:
            continue;
        }

        ICGetElemNativeStub* getElemNativeStub = reinterpret_cast<ICGetElemNativeStub*>(*iter);
        if (propName != getElemNativeStub->name())
            continue;

        if (obj->lastProperty() != getElemNativeStub->shape())
            continue;

        // For prototype gets, check the holder and holder shape.
        if (indirect) {
            if (iter->isGetElem_NativePrototypeSlot()) {
                ICGetElem_NativePrototypeSlot* protoStub = iter->toGetElem_NativePrototypeSlot();

                if (holder != protoStub->holder())
                    continue;

                // If the holder matches, but the holder's lastProperty doesn't match, then
                // this stub is invalid anyway.  Unlink it.
                if (holder->lastProperty() != protoStub->holderShape()) {
                    iter.unlink(cx);
                    continue;
                }
            } else {
                MOZ_ASSERT(iter->isGetElem_NativePrototypeCallNative() ||
                           iter->isGetElem_NativePrototypeCallScripted());

                ICGetElemNativePrototypeCallStub* protoStub =
                    reinterpret_cast<ICGetElemNativePrototypeCallStub*>(*iter);

                if (holder != protoStub->holder())
                    continue;

                // If the holder matches, but the holder's lastProperty doesn't match, then
                // this stub is invalid anyway.  Unlink it.
                if (holder->lastProperty() != protoStub->holderShape()) {
                    iter.unlink(cx);
                    continue;
                }
            }
        }

        // If the new stub needs atomization, and the old stub doesn't atomize, then
        // remove the old stub.
        if (needsAtomize && !getElemNativeStub->needsAtomize()) {
            iter.unlink(cx);
            continue;
        }

        // Should never get here, because this means a matching stub exists, and if
        // a matching stub exists, this procedure should never have been called.
        MOZ_CRASH("Procedure should never have been called.");
    }
}

static bool
TypedArrayGetElemStubExists(ICGetElem_Fallback* stub, HandleObject obj)
{
    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        if (!iter->isGetElem_TypedArray())
            continue;
        if (obj->lastProperty() == iter->toGetElem_TypedArray()->shape())
            return true;
    }
    return false;
}

static bool
ArgumentsGetElemStubExists(ICGetElem_Fallback* stub, ICGetElem_Arguments::Which which)
{
    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        if (!iter->isGetElem_Arguments())
            continue;
        if (iter->toGetElem_Arguments()->which() == which)
            return true;
    }
    return false;
}


static bool
TryAttachNativeGetElemStub(JSContext* cx, HandleScript script, jsbytecode* pc,
                           ICGetElem_Fallback* stub, HandleObject obj,
                           HandleValue key)
{
    // Native-object GetElem stubs can't deal with non-string keys.
    if (!key.isString())
        return true;

    // Convert to interned property name.
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, key, &id))
        return false;

    uint32_t dummy;
    if (!JSID_IS_ATOM(id) || JSID_TO_ATOM(id)->isIndex(&dummy))
        return true;

    RootedPropertyName propName(cx, JSID_TO_ATOM(id)->asPropertyName());
    bool needsAtomize = !key.toString()->isAtom();
    bool isCallElem = (JSOp(*pc) == JSOP_CALLELEM);

    RootedShape shape(cx);
    RootedObject holder(cx);
    if (!EffectlesslyLookupProperty(cx, obj, propName, &holder, &shape))
        return false;

    if (IsCacheableGetPropReadSlot(obj, holder, shape)) {
        // If a suitable stub already exists, nothing else to do.
        if (GetElemNativeStubExists(stub, obj, holder, propName, needsAtomize))
            return true;

        // Remove any existing stubs that may interfere with the new stub being added.
        RemoveExistingGetElemNativeStubs(cx, stub, obj, holder, propName, needsAtomize);

        bool isFixedSlot;
        uint32_t offset;
        GetFixedOrDynamicSlotOffset(&holder->as<NativeObject>(),
                                    shape->slot(), &isFixedSlot, &offset);

        ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();
        ICStub::Kind kind = (obj == holder) ? ICStub::GetElem_NativeSlot
                                            : ICStub::GetElem_NativePrototypeSlot;

        JitSpew(JitSpew_BaselineIC, "  Generating GetElem(Native %s%s slot) stub "
                                    "(obj=%p, shape=%p, holder=%p, holderShape=%p)",
                    (obj == holder) ? "direct" : "prototype",
                    needsAtomize ? " atomizing" : "",
                    obj.get(), obj->lastProperty(), holder.get(), holder->lastProperty());

        ICGetElemNativeStub::AccessType acctype = isFixedSlot ? ICGetElemNativeStub::FixedSlot
                                                              : ICGetElemNativeStub::DynamicSlot;
        ICGetElemNativeCompiler compiler(cx, kind, isCallElem, monitorStub, obj, holder, propName,
                                         acctype, needsAtomize, offset);
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        return true;
    }

    bool getterIsScripted = false;
    bool isTemporarilyUnoptimizable = false;
    if (IsCacheableGetPropCall(cx, obj, holder, shape, &getterIsScripted,
                               &isTemporarilyUnoptimizable, /*isDOMProxy=*/false)) {
        RootedFunction getter(cx, &shape->getterObject()->as<JSFunction>());

#if JS_HAS_NO_SUCH_METHOD
        // It's unlikely that a getter function will be used in callelem locations.
        // Just don't attach stubs in that case to avoid issues with __noSuchMethod__ handling.
        if (isCallElem)
            return true;
#endif

        // For now, we do not handle own property getters
        if (obj == holder)
            return true;

        // If a suitable stub already exists, nothing else to do.
        if (GetElemNativeStubExists(stub, obj, holder, propName, needsAtomize))
            return true;

        // Remove any existing stubs that may interfere with the new stub being added.
        RemoveExistingGetElemNativeStubs(cx, stub, obj, holder, propName, needsAtomize);

        ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();
        ICStub::Kind kind = getterIsScripted ? ICStub::GetElem_NativePrototypeCallScripted
                                             : ICStub::GetElem_NativePrototypeCallNative;

        if (getterIsScripted) {
            JitSpew(JitSpew_BaselineIC,
                    "  Generating GetElem(Native %s%s call scripted %s:%d) stub "
                    "(obj=%p, shape=%p, holder=%p, holderShape=%p)",
                        (obj == holder) ? "direct" : "prototype",
                        needsAtomize ? " atomizing" : "",
                        getter->nonLazyScript()->filename(), getter->nonLazyScript()->lineno(),
                        obj.get(), obj->lastProperty(), holder.get(), holder->lastProperty());
        } else {
            JitSpew(JitSpew_BaselineIC,
                    "  Generating GetElem(Native %s%s call native) stub "
                    "(obj=%p, shape=%p, holder=%p, holderShape=%p)",
                        (obj == holder) ? "direct" : "prototype",
                        needsAtomize ? " atomizing" : "",
                        obj.get(), obj->lastProperty(), holder.get(), holder->lastProperty());
        }

        ICGetElemNativeStub::AccessType acctype = getterIsScripted
                                                           ? ICGetElemNativeStub::ScriptedGetter
                                                           : ICGetElemNativeStub::NativeGetter;
        ICGetElemNativeCompiler compiler(cx, kind, monitorStub, obj, holder, propName, acctype,
                                         needsAtomize, getter, script->pcToOffset(pc), isCallElem);
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        return true;
    }

    return true;
}

static bool
IsPrimitiveArrayTypedObject(JSObject* obj)
{
    if (!obj->is<TypedObject>())
        return false;
    TypeDescr& descr = obj->as<TypedObject>().typeDescr();
    return descr.is<ArrayTypeDescr>() &&
           descr.as<ArrayTypeDescr>().elementType().is<ScalarTypeDescr>();
}

static Scalar::Type
PrimitiveArrayTypedObjectType(JSObject* obj)
{
    MOZ_ASSERT(IsPrimitiveArrayTypedObject(obj));
    TypeDescr& descr = obj->as<TypedObject>().typeDescr();
    return descr.as<ArrayTypeDescr>().elementType().as<ScalarTypeDescr>().type();
}

static Scalar::Type
TypedThingElementType(JSObject* obj)
{
    return IsAnyTypedArray(obj)
           ? AnyTypedArrayType(obj)
           : PrimitiveArrayTypedObjectType(obj);
}

static bool
TypedThingRequiresFloatingPoint(JSObject* obj)
{
    Scalar::Type type = TypedThingElementType(obj);
    return type == Scalar::Uint32 ||
           type == Scalar::Float32 ||
           type == Scalar::Float64;
}

static bool
TryAttachGetElemStub(JSContext* cx, JSScript* script, jsbytecode* pc, ICGetElem_Fallback* stub,
                     HandleValue lhs, HandleValue rhs, HandleValue res)
{
    bool isCallElem = (JSOp(*pc) == JSOP_CALLELEM);

    // Check for String[i] => Char accesses.
    if (lhs.isString() && rhs.isInt32() && res.isString() &&
        !stub->hasStub(ICStub::GetElem_String))
    {
        // NoSuchMethod handling doesn't apply to string targets.

        JitSpew(JitSpew_BaselineIC, "  Generating GetElem(String[Int32]) stub");
        ICGetElem_String::Compiler compiler(cx);
        ICStub* stringStub = compiler.getStub(compiler.getStubSpace(script));
        if (!stringStub)
            return false;

        stub->addNewStub(stringStub);
        return true;
    }

    if (lhs.isMagic(JS_OPTIMIZED_ARGUMENTS) && rhs.isInt32() &&
        !ArgumentsGetElemStubExists(stub, ICGetElem_Arguments::Magic))
    {
        // Any script with a CALLPROP on arguments (arguments.foo())
        // should not have optimized arguments.
        MOZ_ASSERT(!isCallElem);

        JitSpew(JitSpew_BaselineIC, "  Generating GetElem(MagicArgs[Int32]) stub");
        ICGetElem_Arguments::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                               ICGetElem_Arguments::Magic, false);
        ICStub* argsStub = compiler.getStub(compiler.getStubSpace(script));
        if (!argsStub)
            return false;

        stub->addNewStub(argsStub);
        return true;
    }

    // Otherwise, GetElem is only optimized on objects.
    if (!lhs.isObject())
        return true;
    RootedObject obj(cx, &lhs.toObject());

    // Check for ArgumentsObj[int] accesses
    if (obj->is<ArgumentsObject>() && rhs.isInt32()) {
        ICGetElem_Arguments::Which which = ICGetElem_Arguments::Normal;
        if (obj->is<StrictArgumentsObject>())
            which = ICGetElem_Arguments::Strict;
        if (!ArgumentsGetElemStubExists(stub, which)) {
            JitSpew(JitSpew_BaselineIC, "  Generating GetElem(ArgsObj[Int32]) stub");
            ICGetElem_Arguments::Compiler compiler(
                cx, stub->fallbackMonitorStub()->firstMonitorStub(), which, isCallElem);
            ICStub* argsStub = compiler.getStub(compiler.getStubSpace(script));
            if (!argsStub)
                return false;

            stub->addNewStub(argsStub);
            return true;
        }
    }

    if (obj->isNative()) {
        // Check for NativeObject[int] dense accesses.
        if (rhs.isInt32() && rhs.toInt32() >= 0 && !IsAnyTypedArray(obj.get())) {
            JitSpew(JitSpew_BaselineIC, "  Generating GetElem(Native[Int32] dense) stub");
            ICGetElem_Dense::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                               obj->lastProperty(), isCallElem);
            ICStub* denseStub = compiler.getStub(compiler.getStubSpace(script));
            if (!denseStub)
                return false;

            stub->addNewStub(denseStub);
            return true;
        }

        // Check for NativeObject[id] shape-optimizable accesses.
        if (rhs.isString()) {
            RootedScript rootedScript(cx, script);
            if (!TryAttachNativeGetElemStub(cx, rootedScript, pc, stub, obj, rhs))
                return false;
            script = rootedScript;
        }
    }

    // Check for TypedArray[int] => Number and TypedObject[int] => Number accesses.
    if ((IsAnyTypedArray(obj.get()) || IsPrimitiveArrayTypedObject(obj)) &&
        rhs.isNumber() &&
        res.isNumber() &&
        !TypedArrayGetElemStubExists(stub, obj))
    {
        // Don't attach CALLELEM stubs for accesses on typed array expected to yield numbers.
#if JS_HAS_NO_SUCH_METHOD
        if (isCallElem)
            return true;
#endif

        if (!cx->runtime()->jitSupportsFloatingPoint &&
            (TypedThingRequiresFloatingPoint(obj) || rhs.isDouble()))
        {
            return true;
        }

        // Don't attach typed object stubs if they might be neutered, as the
        // stub will always bail out.
        if (IsPrimitiveArrayTypedObject(obj) && cx->compartment()->neuteredTypedObjects)
            return true;

        JitSpew(JitSpew_BaselineIC, "  Generating GetElem(TypedArray[Int32]) stub");
        ICGetElem_TypedArray::Compiler compiler(cx, obj->lastProperty(),
                                                TypedThingElementType(obj));
        ICStub* typedArrayStub = compiler.getStub(compiler.getStubSpace(script));
        if (!typedArrayStub)
            return false;

        stub->addNewStub(typedArrayStub);
        return true;
    }

    // GetElem operations on non-native objects cannot be cached by either
    // Baseline or Ion. Indicate this in the cache so that Ion does not
    // generate a cache for this op.
    if (!obj->isNative())
        stub->noteNonNativeAccess();

    // GetElem operations which could access negative indexes generally can't
    // be optimized without the potential for bailouts, as we can't statically
    // determine that an object has no properties on such indexes.
    if (rhs.isNumber() && rhs.toNumber() < 0)
        stub->noteNegativeIndex();

    return true;
}

static bool
DoGetElemFallback(JSContext* cx, BaselineFrame* frame, ICGetElem_Fallback* stub_, HandleValue lhs,
                  HandleValue rhs, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICGetElem_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(frame->script());
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "GetElem(%s)", js_CodeName[op]);

    MOZ_ASSERT(op == JSOP_GETELEM || op == JSOP_CALLELEM);

    // Don't pass lhs directly, we need it when generating stubs.
    RootedValue lhsCopy(cx, lhs);

    bool isOptimizedArgs = false;
    if (lhs.isMagic(JS_OPTIMIZED_ARGUMENTS)) {
        // Handle optimized arguments[i] access.
        if (!GetElemOptimizedArguments(cx, frame, &lhsCopy, rhs, res, &isOptimizedArgs))
            return false;
        if (isOptimizedArgs)
            TypeScript::Monitor(cx, frame->script(), pc, res);
    }

    if (!isOptimizedArgs) {
        if (!GetElementOperation(cx, op, &lhsCopy, rhs, res))
            return false;
        TypeScript::Monitor(cx, frame->script(), pc, res);
    }

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, frame->script(), res))
        return false;

    if (stub->numOptimizedStubs() >= ICGetElem_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with inert megamorphic stub.
        // But for now we just bail.
        return true;
    }

    // Try to attach an optimized stub.
    if (!TryAttachGetElemStub(cx, frame->script(), pc, stub, lhs, rhs, res))
        return false;

    // If we ever add a way to note unoptimizable accesses here, propagate the
    // isTemporarilyUnoptimizable state from TryAttachNativeGetElemStub to here.

    return true;
}

typedef bool (*DoGetElemFallbackFn)(JSContext*, BaselineFrame*, ICGetElem_Fallback*,
                                    HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoGetElemFallbackInfo =
    FunctionInfo<DoGetElemFallbackFn>(DoGetElemFallback, TailCall, PopValues(2));

bool
ICGetElem_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoGetElemFallbackInfo, masm);
}

//
// GetElem_NativeSlot
//

static bool
DoAtomizeString(JSContext* cx, HandleString string, MutableHandleValue result)
{
    JitSpew(JitSpew_BaselineIC, "  AtomizeString called");

    RootedValue key(cx, StringValue(string));

    // Convert to interned property name.
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, key, &id))
        return false;

    if (!JSID_IS_ATOM(id)) {
        result.set(key);
        return true;
    }

    result.set(StringValue(JSID_TO_ATOM(id)));
    return true;
}

typedef bool (*DoAtomizeStringFn)(JSContext*, HandleString, MutableHandleValue);
static const VMFunction DoAtomizeStringInfo = FunctionInfo<DoAtomizeStringFn>(DoAtomizeString);

bool
ICGetElemNativeCompiler::emitCallNative(MacroAssembler& masm, Register objReg)
{
    GeneralRegisterSet regs = availableGeneralRegs(0);
    regs.takeUnchecked(objReg);
    regs.takeUnchecked(BaselineTailCallReg);

    enterStubFrame(masm, regs.getAny());

    // Push object.
    masm.push(objReg);

    // Push native callee.
    masm.loadPtr(Address(BaselineStubReg, ICGetElemNativeGetterStub::offsetOfGetter()), objReg);
    masm.push(objReg);

    regs.add(objReg);

    // Call helper.
    if (!callVM(DoCallNativeGetterInfo, masm))
        return false;

    leaveStubFrame(masm);

    return true;
}

bool
ICGetElemNativeCompiler::emitCallScripted(MacroAssembler& masm, Register objReg)
{
    GeneralRegisterSet regs = availableGeneralRegs(0);
    regs.takeUnchecked(objReg);
    regs.takeUnchecked(BaselineTailCallReg);

    // Enter stub frame.
    enterStubFrame(masm, regs.getAny());

    // Align the stack such that the JitFrameLayout is aligned on
    // JitStackAlignment.
    masm.alignJitStackBasedOnNArgs(0);

    // Push |this| for getter (target object).
    {
        ValueOperand val = regs.takeAnyValue();
        masm.tagValue(JSVAL_TYPE_OBJECT, objReg, val);
        masm.Push(val);
        regs.add(val);
    }

    regs.add(objReg);

    Register callee = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICGetElemNativeGetterStub::offsetOfGetter()), callee);

    // Push argc, callee, and descriptor.
    {
        Register callScratch = regs.takeAny();
        EmitCreateStubFrameDescriptor(masm, callScratch);
        masm.Push(Imm32(0));  // ActualArgc is 0
        masm.Push(callee);
        masm.Push(callScratch);
        regs.add(callScratch);
    }

    Register code = regs.takeAnyExcluding(ArgumentsRectifierReg);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), code);
    masm.loadBaselineOrIonRaw(code, code, nullptr);

    Register scratch = regs.takeAny();

    // Handle arguments underflow.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), scratch);
    masm.branch32(Assembler::Equal, scratch, Imm32(0), &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != code);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), code);
        masm.loadPtr(Address(code, JitCode::offsetOfCode()), code);
        masm.mov(ImmWord(0), ArgumentsRectifierReg);
    }

    masm.bind(&noUnderflow);
    masm.callJit(code);

    leaveStubFrame(masm, true);

    return true;
}

bool
ICGetElemNativeCompiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    Label failurePopR1;
    bool popR1 = false;

    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    masm.branchTestString(Assembler::NotEqual, R1, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox object.
    Register objReg = masm.extractObject(R0, ExtractTemp0);

    // Check object shape.
    masm.loadPtr(Address(objReg, JSObject::offsetOfShape()), scratchReg);
    Address shapeAddr(BaselineStubReg, ICGetElemNativeStub::offsetOfShape());
    masm.branchPtr(Assembler::NotEqual, shapeAddr, scratchReg, &failure);

    // Check key identity.  Don't automatically fail if this fails, since the incoming
    // key maybe a non-interned string.  Switch to a slowpath vm-call based check.
    Address nameAddr(BaselineStubReg, ICGetElemNativeStub::offsetOfName());
    Register strExtract = masm.extractString(R1, ExtractTemp1);

    // If needsAtomize_ is true, and the string is not already an atom, then atomize the
    // string before proceeding.
    if (needsAtomize_) {
        Label skipAtomize;

        // If string is already an atom, skip the atomize.
        masm.branchTest32(Assembler::NonZero,
                          Address(strExtract, JSString::offsetOfFlags()),
                          Imm32(JSString::ATOM_BIT),
                          &skipAtomize);

        // Stow R0.
        EmitStowICValues(masm, 1);

        enterStubFrame(masm, R0.scratchReg());

        // Atomize the string into a new value.
        masm.push(strExtract);
        if (!callVM(DoAtomizeStringInfo, masm))
            return false;

        // Atomized string is now in JSReturnOperand (R0).
        // Leave stub frame, move atomized string into R1.
        MOZ_ASSERT(R0 == JSReturnOperand);
        leaveStubFrame(masm);
        masm.moveValue(JSReturnOperand, R1);

        // Unstow R0
        EmitUnstowICValues(masm, 1);

        // Extract string from R1 again.
        DebugOnly<Register> strExtract2 = masm.extractString(R1, ExtractTemp1);
        MOZ_ASSERT(Register(strExtract2) == strExtract);

        masm.bind(&skipAtomize);
    }

    // Since this stub sometimes enter a stub frame, we manually set this to true (lie).
#ifdef DEBUG
    entersStubFrame_ = true;
#endif

    // Key has been atomized if necessary.  Do identity check on string pointer.
    masm.branchPtr(Assembler::NotEqual, nameAddr, strExtract, &failure);

    Register holderReg;
    if (obj_ == holder_) {
        holderReg = objReg;
    } else {
        // Shape guard holder.
        if (regs.empty()) {
            masm.push(R1.scratchReg());
            popR1 = true;
            holderReg = R1.scratchReg();
        } else {
            holderReg = regs.takeAny();
        }

        if (kind == ICStub::GetElem_NativePrototypeCallNative ||
            kind == ICStub::GetElem_NativePrototypeCallScripted)
        {
            masm.loadPtr(Address(BaselineStubReg,
                                 ICGetElemNativePrototypeCallStub::offsetOfHolder()),
                         holderReg);
            masm.loadPtr(Address(BaselineStubReg,
                                 ICGetElemNativePrototypeCallStub::offsetOfHolderShape()),
                         scratchReg);
        } else {
            masm.loadPtr(Address(BaselineStubReg,
                                 ICGetElem_NativePrototypeSlot::offsetOfHolder()),
                         holderReg);
            masm.loadPtr(Address(BaselineStubReg,
                                 ICGetElem_NativePrototypeSlot::offsetOfHolderShape()),
                         scratchReg);
        }
        masm.branchTestObjShape(Assembler::NotEqual, holderReg, scratchReg,
                                popR1 ? &failurePopR1 : &failure);
    }

    if (acctype_ == ICGetElemNativeStub::DynamicSlot ||
        acctype_ == ICGetElemNativeStub::FixedSlot)
    {
        masm.load32(Address(BaselineStubReg, ICGetElemNativeSlotStub::offsetOfOffset()),
                    scratchReg);

        // Load from object.
        if (acctype_ == ICGetElemNativeStub::DynamicSlot)
            masm.addPtr(Address(holderReg, NativeObject::offsetOfSlots()), scratchReg);
        else
            masm.addPtr(holderReg, scratchReg);

        Address valAddr(scratchReg, 0);

        // Check if __noSuchMethod__ needs to be called.
#if JS_HAS_NO_SUCH_METHOD
        if (isCallElem_) {
            Label afterNoSuchMethod;
            Label skipNoSuchMethod;

            masm.branchTestUndefined(Assembler::NotEqual, valAddr, &skipNoSuchMethod);

            GeneralRegisterSet regs = availableGeneralRegs(0);
            regs.take(R1);
            regs.take(R0);
            regs.takeUnchecked(objReg);
            if (popR1)
                masm.pop(R1.scratchReg());

            // Box and push obj and key onto baseline frame stack for decompiler.
            masm.tagValue(JSVAL_TYPE_OBJECT, objReg, R0);
            EmitStowICValues(masm, 2);

            regs.add(R0);
            regs.takeUnchecked(objReg);

            enterStubFrame(masm, regs.getAnyExcluding(BaselineTailCallReg));

            masm.pushValue(R1);
            masm.push(objReg);
            if (!callVM(LookupNoSuchMethodHandlerInfo, masm))
                return false;

            leaveStubFrame(masm);

            // Pop pushed obj and key from baseline stack.
            EmitUnstowICValues(masm, 2, /* discard = */ true);

            // Result is already in R0
            masm.jump(&afterNoSuchMethod);
            masm.bind(&skipNoSuchMethod);

            if (popR1)
                masm.pop(R1.scratchReg());
            masm.loadValue(valAddr, R0);
            masm.bind(&afterNoSuchMethod);
        } else {
            masm.loadValue(valAddr, R0);
            if (popR1)
                masm.addPtr(ImmWord(sizeof(size_t)), BaselineStackReg);
        }
#else
        masm.loadValue(valAddr, R0);
        if (popR1)
            masm.addPtr(ImmWord(sizeof(size_t)), BaselineStackReg);
#endif

    } else {
        MOZ_ASSERT(acctype_ == ICGetElemNativeStub::NativeGetter ||
                   acctype_ == ICGetElemNativeStub::ScriptedGetter);
        MOZ_ASSERT(kind == ICStub::GetElem_NativePrototypeCallNative ||
                   kind == ICStub::GetElem_NativePrototypeCallScripted);

        if (acctype_ == ICGetElemNativeStub::NativeGetter) {
            // If calling a native getter, there is no chance of failure now.

            // GetElem key (R1) is no longer needed.
            if (popR1)
                masm.addPtr(ImmWord(sizeof(size_t)), BaselineStackReg);

            emitCallNative(masm, objReg);

        } else {
            MOZ_ASSERT(acctype_ == ICGetElemNativeStub::ScriptedGetter);

            // Load function in scratchReg and ensure that it has a jit script.
            masm.loadPtr(Address(BaselineStubReg, ICGetElemNativeGetterStub::offsetOfGetter()),
                         scratchReg);
            masm.branchIfFunctionHasNoScript(scratchReg, popR1 ? &failurePopR1 : &failure);
            masm.loadPtr(Address(scratchReg, JSFunction::offsetOfNativeOrScript()), scratchReg);
            masm.loadBaselineOrIonRaw(scratchReg, scratchReg, popR1 ? &failurePopR1 : &failure);

            // At this point, we are guaranteed to successfully complete.
            if (popR1)
                masm.addPtr(Imm32(sizeof(size_t)), BaselineStackReg);

            emitCallScripted(masm, objReg);
        }
    }

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    if (popR1) {
        masm.bind(&failurePopR1);
        masm.pop(R1.scratchReg());
    }
    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    return true;
}

//
// GetElem_String
//

bool
ICGetElem_String::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestString(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox string in R0.
    Register str = masm.extractString(R0, ExtractTemp0);

    // Check for non-linear strings.
    masm.branchIfRope(str, &failure);

    // Unbox key.
    Register key = masm.extractInt32(R1, ExtractTemp1);

    // Bounds check.
    masm.branch32(Assembler::BelowOrEqual, Address(str, JSString::offsetOfLength()),
                  key, &failure);

    // Get char code.
    masm.loadStringChar(str, key, scratchReg);

    // Check if char code >= UNIT_STATIC_LIMIT.
    masm.branch32(Assembler::AboveOrEqual, scratchReg, Imm32(StaticStrings::UNIT_STATIC_LIMIT),
                  &failure);

    // Load static string.
    masm.movePtr(ImmPtr(&cx->staticStrings().unitStaticTable), str);
    masm.loadPtr(BaseIndex(str, scratchReg, ScalePointer), str);

    // Return.
    masm.tagValue(JSVAL_TYPE_STRING, str, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// GetElem_Dense
//

bool
ICGetElem_Dense::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox R0 and shape guard.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICGetElem_Dense::offsetOfShape()), scratchReg);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratchReg, &failure);

    // Load obj->elements.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratchReg);

    // Unbox key.
    Register key = masm.extractInt32(R1, ExtractTemp1);

    // Bounds check.
    Address initLength(scratchReg, ObjectElements::offsetOfInitializedLength());
    masm.branch32(Assembler::BelowOrEqual, initLength, key, &failure);

    // Hole check and load value.
    BaseObjectElementIndex element(scratchReg, key);
    masm.branchTestMagic(Assembler::Equal, element, &failure);

    // Check if __noSuchMethod__ should be called.
#if JS_HAS_NO_SUCH_METHOD
#ifdef DEBUG
    entersStubFrame_ = true;
#endif
    if (isCallElem_) {
        Label afterNoSuchMethod;
        Label skipNoSuchMethod;
        regs = availableGeneralRegs(0);
        regs.takeUnchecked(obj);
        regs.takeUnchecked(key);
        regs.takeUnchecked(BaselineTailCallReg);
        ValueOperand val = regs.takeValueOperand();

        masm.loadValue(element, val);
        masm.branchTestUndefined(Assembler::NotEqual, val, &skipNoSuchMethod);

        // Box and push obj and key onto baseline frame stack for decompiler.
        EmitRestoreTailCallReg(masm);
        masm.tagValue(JSVAL_TYPE_OBJECT, obj, val);
        masm.pushValue(val);
        masm.tagValue(JSVAL_TYPE_INT32, key, val);
        masm.pushValue(val);
        EmitRepushTailCallReg(masm);

        regs.add(val);

        // Call __noSuchMethod__ checker.  Object pointer is in objReg.
        enterStubFrame(masm, regs.getAnyExcluding(BaselineTailCallReg));

        regs.take(val);

        masm.tagValue(JSVAL_TYPE_INT32, key, val);
        masm.pushValue(val);
        masm.push(obj);
        if (!callVM(LookupNoSuchMethodHandlerInfo, masm))
            return false;

        leaveStubFrame(masm);

        // Pop pushed obj and key from baseline stack.
        EmitUnstowICValues(masm, 2, /* discard = */ true);

        // Result is already in R0
        masm.jump(&afterNoSuchMethod);
        masm.bind(&skipNoSuchMethod);

        masm.moveValue(val, R0);
        masm.bind(&afterNoSuchMethod);
    } else {
        masm.loadValue(element, R0);
    }
#else
    // Load value from element location.
    masm.loadValue(element, R0);
#endif

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// GetElem_TypedArray
//

static void
LoadTypedThingLength(MacroAssembler& masm, TypedThingLayout layout, Register obj, Register result)
{
    switch (layout) {
      case Layout_TypedArray:
        masm.unboxInt32(Address(obj, TypedArrayLayout::lengthOffset()), result);
        break;
      case Layout_OutlineTypedObject:
      case Layout_InlineTypedObject:
        masm.loadPtr(Address(obj, JSObject::offsetOfGroup()), result);
        masm.loadPtr(Address(result, ObjectGroup::offsetOfAddendum()), result);
        masm.unboxInt32(Address(result, ArrayTypeDescr::offsetOfLength()), result);
        break;
      default:
        MOZ_CRASH();
    }
}

static void
LoadTypedThingData(MacroAssembler& masm, TypedThingLayout layout, Register obj, Register result)
{
    switch (layout) {
      case Layout_TypedArray:
        masm.loadPtr(Address(obj, TypedArrayLayout::dataOffset()), result);
        break;
      case Layout_OutlineTypedObject:
        masm.loadPtr(Address(obj, OutlineTypedObject::offsetOfData()), result);
        break;
      case Layout_InlineTypedObject:
        masm.computeEffectiveAddress(Address(obj, InlineTypedObject::offsetOfDataStart()), result);
        break;
      default:
        MOZ_CRASH();
    }
}

static void
CheckForNeuteredTypedObject(JSContext* cx, MacroAssembler& masm, Label* failure)
{
    // All stubs which manipulate typed objects need to check the compartment
    // wide flag indicating whether the objects are neutered, and bail out in
    // this case.
    int32_t* address = &cx->compartment()->neuteredTypedObjects;
    masm.branch32(Assembler::NotEqual, AbsoluteAddress(address), Imm32(0), failure);
}

bool
ICGetElem_TypedArray::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    if (layout_ != Layout_TypedArray)
        CheckForNeuteredTypedObject(cx, masm, &failure);

    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox R0 and shape guard.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICGetElem_TypedArray::offsetOfShape()), scratchReg);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratchReg, &failure);

    // Ensure the index is an integer.
    if (cx->runtime()->jitSupportsFloatingPoint) {
        Label isInt32;
        masm.branchTestInt32(Assembler::Equal, R1, &isInt32);
        {
            // If the index is a double, try to convert it to int32. It's okay
            // to convert -0 to 0: the shape check ensures the object is a typed
            // array so the difference is not observable.
            masm.branchTestDouble(Assembler::NotEqual, R1, &failure);
            masm.unboxDouble(R1, FloatReg0);
            masm.convertDoubleToInt32(FloatReg0, scratchReg, &failure, /* negZeroCheck = */false);
            masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R1);
        }
        masm.bind(&isInt32);
    } else {
        masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
    }

    // Unbox key.
    Register key = masm.extractInt32(R1, ExtractTemp1);

    // Bounds check.
    LoadTypedThingLength(masm, layout_, obj, scratchReg);
    masm.branch32(Assembler::BelowOrEqual, scratchReg, key, &failure);

    // Load the elements vector.
    LoadTypedThingData(masm, layout_, obj, scratchReg);

    // Load the value.
    BaseIndex source(scratchReg, key, ScaleFromElemWidth(Scalar::byteSize(type_)));
    masm.loadFromTypedArray(type_, source, R0, false, scratchReg, &failure);

    // Todo: Allow loading doubles from uint32 arrays, but this requires monitoring.
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// GetElem_Arguments
//
bool
ICGetElem_Arguments::Compiler::generateStubCode(MacroAssembler& masm)
{
    // Variants of GetElem_Arguments can enter stub frames if entered in CallProp
    // context when noSuchMethod support is on.
#if JS_HAS_NO_SUCH_METHOD
#ifdef DEBUG
    entersStubFrame_ = true;
#endif
#endif

    Label failure;
    if (which_ == ICGetElem_Arguments::Magic) {
        MOZ_ASSERT(!isCallElem_);

        // Ensure that this is a magic arguments value.
        masm.branchTestMagicValue(Assembler::NotEqual, R0, JS_OPTIMIZED_ARGUMENTS, &failure);

        // Ensure that frame has not loaded different arguments object since.
        masm.branchTest32(Assembler::NonZero,
                          Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFlags()),
                          Imm32(BaselineFrame::HAS_ARGS_OBJ),
                          &failure);

        // Ensure that index is an integer.
        masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
        Register idx = masm.extractInt32(R1, ExtractTemp1);

        GeneralRegisterSet regs(availableGeneralRegs(2));
        Register scratch = regs.takeAny();

        // Load num actual arguments
        Address actualArgs(BaselineFrameReg, BaselineFrame::offsetOfNumActualArgs());
        masm.loadPtr(actualArgs, scratch);

        // Ensure idx < argc
        masm.branch32(Assembler::AboveOrEqual, idx, scratch, &failure);

        // Load argval
        masm.movePtr(BaselineFrameReg, scratch);
        masm.addPtr(Imm32(BaselineFrame::offsetOfArg(0)), scratch);
        BaseValueIndex element(scratch, idx);
        masm.loadValue(element, R0);

        // Enter type monitor IC to type-check result.
        EmitEnterTypeMonitorIC(masm);

        masm.bind(&failure);
        EmitStubGuardFailure(masm);
        return true;
    }

    MOZ_ASSERT(which_ == ICGetElem_Arguments::Strict ||
               which_ == ICGetElem_Arguments::Normal);

    bool isStrict = which_ == ICGetElem_Arguments::Strict;
    const Class* clasp = isStrict ? &StrictArgumentsObject::class_ : &NormalArgumentsObject::class_;

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Guard on input being an arguments object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    Register objReg = masm.extractObject(R0, ExtractTemp0);
    masm.branchTestObjClass(Assembler::NotEqual, objReg, scratchReg, clasp, &failure);

    // Guard on index being int32
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
    Register idxReg = masm.extractInt32(R1, ExtractTemp1);

    // Get initial ArgsObj length value.
    masm.unboxInt32(Address(objReg, ArgumentsObject::getInitialLengthSlotOffset()), scratchReg);

    // Test if length has been overridden.
    masm.branchTest32(Assembler::NonZero,
                      scratchReg,
                      Imm32(ArgumentsObject::LENGTH_OVERRIDDEN_BIT),
                      &failure);

    // Length has not been overridden, ensure that R1 is an integer and is <= length.
    masm.rshiftPtr(Imm32(ArgumentsObject::PACKED_BITS_COUNT), scratchReg);
    masm.branch32(Assembler::AboveOrEqual, idxReg, scratchReg, &failure);

    // Length check succeeded, now check the correct bit.  We clobber potential type regs
    // now.  Inputs will have to be reconstructed if we fail after this point, but that's
    // unlikely.
    Label failureReconstructInputs;
    regs = availableGeneralRegs(0);
    regs.takeUnchecked(objReg);
    regs.takeUnchecked(idxReg);
    regs.take(scratchReg);
    Register argData = regs.takeAny();
    Register tempReg = regs.takeAny();

    // Load ArgumentsData
    masm.loadPrivate(Address(objReg, ArgumentsObject::getDataSlotOffset()), argData);

    // Load deletedBits bitArray pointer into scratchReg
    masm.loadPtr(Address(argData, offsetof(ArgumentsData, deletedBits)), scratchReg);

    // In tempReg, calculate index of word containing bit: (idx >> logBitsPerWord)
    masm.movePtr(idxReg, tempReg);
    const uint32_t shift = mozilla::tl::FloorLog2<(sizeof(size_t) * JS_BITS_PER_BYTE)>::value;
    MOZ_ASSERT(shift == 5 || shift == 6);
    masm.rshiftPtr(Imm32(shift), tempReg);
    masm.loadPtr(BaseIndex(scratchReg, tempReg, ScaleFromElemWidth(sizeof(size_t))), scratchReg);

    // Don't bother testing specific bit, if any bit is set in the word, fail.
    masm.branchPtr(Assembler::NotEqual, scratchReg, ImmPtr(nullptr), &failureReconstructInputs);

    // Load the value.  use scratchReg and tempReg to form a ValueOperand to load into.
    masm.addPtr(Imm32(ArgumentsData::offsetOfArgs()), argData);
    regs.add(scratchReg);
    regs.add(tempReg);
    ValueOperand tempVal = regs.takeAnyValue();
    masm.loadValue(BaseValueIndex(argData, idxReg), tempVal);

    // Makesure that this is not a FORWARD_TO_CALL_SLOT magic value.
    masm.branchTestMagic(Assembler::Equal, tempVal, &failureReconstructInputs);

#if JS_HAS_NO_SUCH_METHOD
    if (isCallElem_) {
        Label afterNoSuchMethod;
        Label skipNoSuchMethod;

        masm.branchTestUndefined(Assembler::NotEqual, tempVal, &skipNoSuchMethod);

        // Call __noSuchMethod__ checker.  Object pointer is in objReg.
        regs = availableGeneralRegs(0);
        regs.takeUnchecked(objReg);
        regs.takeUnchecked(idxReg);
        regs.takeUnchecked(BaselineTailCallReg);
        ValueOperand val = regs.takeValueOperand();

        // Box and push obj and key onto baseline frame stack for decompiler.
        EmitRestoreTailCallReg(masm);
        masm.tagValue(JSVAL_TYPE_OBJECT, objReg, val);
        masm.pushValue(val);
        masm.tagValue(JSVAL_TYPE_INT32, idxReg, val);
        masm.pushValue(val);
        EmitRepushTailCallReg(masm);

        regs.add(val);
        enterStubFrame(masm, regs.getAnyExcluding(BaselineTailCallReg));
        regs.take(val);

        masm.pushValue(val);
        masm.push(objReg);
        if (!callVM(LookupNoSuchMethodHandlerInfo, masm))
            return false;

        leaveStubFrame(masm);

        // Pop pushed obj and key from baseline stack.
        EmitUnstowICValues(masm, 2, /* discard = */ true);

        // Result is already in R0
        masm.jump(&afterNoSuchMethod);
        masm.bind(&skipNoSuchMethod);

        masm.moveValue(tempVal, R0);
        masm.bind(&afterNoSuchMethod);
    } else {
        masm.moveValue(tempVal, R0);
    }
#else
    // Copy value from temp to R0.
    masm.moveValue(tempVal, R0);
#endif

    // Type-check result
    EmitEnterTypeMonitorIC(masm);

    // Failed, but inputs are deconstructed into object and int, and need to be
    // reconstructed into values.
    masm.bind(&failureReconstructInputs);
    masm.tagValue(JSVAL_TYPE_OBJECT, objReg, R0);
    masm.tagValue(JSVAL_TYPE_INT32, idxReg, R1);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// SetElem_Fallback
//

static bool
SetElemDenseAddHasSameShapes(ICSetElem_DenseAdd* stub, JSObject* obj)
{
    size_t numShapes = stub->protoChainDepth() + 1;
    for (size_t i = 0; i < numShapes; i++) {
        static const size_t MAX_DEPTH = ICSetElem_DenseAdd::MAX_PROTO_CHAIN_DEPTH;
        if (obj->lastProperty() != stub->toImplUnchecked<MAX_DEPTH>()->shape(i))
            return false;
        obj = obj->getProto();
        if (!obj && i != numShapes - 1)
            return false;
    }

    return true;
}

static bool
DenseSetElemStubExists(JSContext* cx, ICStub::Kind kind, ICSetElem_Fallback* stub, HandleObject obj)
{
    MOZ_ASSERT(kind == ICStub::SetElem_Dense || kind == ICStub::SetElem_DenseAdd);

    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        if (kind == ICStub::SetElem_Dense && iter->isSetElem_Dense()) {
            ICSetElem_Dense* dense = iter->toSetElem_Dense();
            if (obj->lastProperty() == dense->shape() && obj->getGroup(cx) == dense->group())
                return true;
        }

        if (kind == ICStub::SetElem_DenseAdd && iter->isSetElem_DenseAdd()) {
            ICSetElem_DenseAdd* dense = iter->toSetElem_DenseAdd();
            if (obj->getGroup(cx) == dense->group() && SetElemDenseAddHasSameShapes(dense, obj))
                return true;
        }
    }
    return false;
}

static bool
TypedArraySetElemStubExists(ICSetElem_Fallback* stub, HandleObject obj, bool expectOOB)
{
    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        if (!iter->isSetElem_TypedArray())
            continue;
        ICSetElem_TypedArray* taStub = iter->toSetElem_TypedArray();
        if (obj->lastProperty() == taStub->shape() && taStub->expectOutOfBounds() == expectOOB)
            return true;
    }
    return false;
}

static bool
RemoveExistingTypedArraySetElemStub(JSContext* cx, ICSetElem_Fallback* stub, HandleObject obj)
{
    for (ICStubIterator iter = stub->beginChain(); !iter.atEnd(); iter++) {
        if (!iter->isSetElem_TypedArray())
            continue;

        if (obj->lastProperty() != iter->toSetElem_TypedArray()->shape())
            continue;

        // TypedArraySetElem stubs are only removed using this procedure if
        // being replaced with one that expects out of bounds index.
        MOZ_ASSERT(!iter->toSetElem_TypedArray()->expectOutOfBounds());
        iter.unlink(cx);
        return true;
    }
    return false;
}

static bool
CanOptimizeDenseSetElem(NativeObject* obj, uint32_t index,
                        Shape* oldShape, uint32_t oldCapacity, uint32_t oldInitLength,
                        bool* isAddingCaseOut, size_t* protoDepthOut)
{
    uint32_t initLength = obj->getDenseInitializedLength();
    uint32_t capacity = obj->getDenseCapacity();

    *isAddingCaseOut = false;
    *protoDepthOut = 0;

    // Some initial sanity checks.
    if (initLength < oldInitLength || capacity < oldCapacity)
        return false;

    Shape* shape = obj->lastProperty();

    // Cannot optimize if the shape changed.
    if (oldShape != shape)
        return false;

    // Cannot optimize if the capacity changed.
    if (oldCapacity != capacity)
        return false;

    // Cannot optimize if the index doesn't fit within the new initialized length.
    if (index >= initLength)
        return false;

    // Cannot optimize if the value at position after the set is a hole.
    if (!obj->containsDenseElement(index))
        return false;

    // At this point, if we know that the initLength did not change, then
    // an optimized set is possible.
    if (oldInitLength == initLength)
        return true;

    // If it did change, ensure that it changed specifically by incrementing by 1
    // to accomodate this particular indexed set.
    if (oldInitLength + 1 != initLength)
        return false;
    if (index != oldInitLength)
        return false;

    // The checks are not complete.  The object may have a setter definition,
    // either directly, or via a prototype, or via the target object for a prototype
    // which is a proxy, that handles a particular integer write.
    // Scan the prototype and shape chain to make sure that this is not the case.
    JSObject* curObj = obj;
    while (curObj) {
        // Ensure object is native.
        if (!curObj->isNative())
            return false;

        // Ensure all indexed properties are stored in dense elements.
        if (curObj->isIndexed())
            return false;

        curObj = curObj->getProto();
        if (curObj)
            ++*protoDepthOut;
    }

    if (*protoDepthOut > ICSetElem_DenseAdd::MAX_PROTO_CHAIN_DEPTH)
        return false;

    *isAddingCaseOut = true;

    return true;
}

static bool
DoSetElemFallback(JSContext* cx, BaselineFrame* frame, ICSetElem_Fallback* stub_, Value* stack,
                  HandleValue objv, HandleValue index, HandleValue rhs)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICSetElem_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "SetElem(%s)", js_CodeName[JSOp(*pc)]);

    MOZ_ASSERT(op == JSOP_SETELEM ||
               op == JSOP_STRICTSETELEM ||
               op == JSOP_INITELEM ||
               op == JSOP_INITELEM_ARRAY ||
               op == JSOP_INITELEM_INC);

    RootedObject obj(cx, ToObjectFromStack(cx, objv));
    if (!obj)
        return false;

    RootedShape oldShape(cx, obj->lastProperty());

    // Check the old capacity
    uint32_t oldCapacity = 0;
    uint32_t oldInitLength = 0;
    if (obj->isNative() && index.isInt32() && index.toInt32() >= 0) {
        oldCapacity = obj->as<NativeObject>().getDenseCapacity();
        oldInitLength = obj->as<NativeObject>().getDenseInitializedLength();
    }

    if (op == JSOP_INITELEM) {
        if (!InitElemOperation(cx, obj, index, rhs))
            return false;
    } else if (op == JSOP_INITELEM_ARRAY) {
        MOZ_ASSERT(uint32_t(index.toInt32()) == GET_UINT24(pc));
        if (!InitArrayElemOperation(cx, pc, obj, index.toInt32(), rhs))
            return false;
    } else if (op == JSOP_INITELEM_INC) {
        if (!InitArrayElemOperation(cx, pc, obj, index.toInt32(), rhs))
            return false;
    } else {
        if (!SetObjectElement(cx, obj, index, rhs, JSOp(*pc) == JSOP_STRICTSETELEM, script, pc))
            return false;
    }

    // Overwrite the object on the stack (pushed for the decompiler) with the rhs.
    MOZ_ASSERT(stack[2] == objv);
    stack[2] = rhs;

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (stub->numOptimizedStubs() >= ICSetElem_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with inert megamorphic stub.
        // But for now we just bail.
        return true;
    }

    // Try to generate new stubs.
    if (obj->isNative() &&
        !IsAnyTypedArray(obj.get()) &&
        index.isInt32() && index.toInt32() >= 0 &&
        !rhs.isMagic(JS_ELEMENTS_HOLE))
    {
        bool addingCase;
        size_t protoDepth;

        if (CanOptimizeDenseSetElem(&obj->as<NativeObject>(), index.toInt32(),
                                    oldShape, oldCapacity, oldInitLength,
                                    &addingCase, &protoDepth))
        {
            RootedShape shape(cx, obj->lastProperty());
            RootedObjectGroup group(cx, obj->getGroup(cx));
            if (!group)
                return false;

            if (addingCase && !DenseSetElemStubExists(cx, ICStub::SetElem_DenseAdd, stub, obj)) {
                JitSpew(JitSpew_BaselineIC,
                        "  Generating SetElem_DenseAdd stub "
                        "(shape=%p, group=%p, protoDepth=%u)",
                        obj->lastProperty(), group.get(), protoDepth);
                ICSetElemDenseAddCompiler compiler(cx, obj, protoDepth);
                ICUpdatedStub* denseStub = compiler.getStub(compiler.getStubSpace(script));
                if (!denseStub)
                    return false;
                if (!denseStub->addUpdateStubForValue(cx, script, obj, JSID_VOIDHANDLE, rhs))
                    return false;

                stub->addNewStub(denseStub);
            } else if (!addingCase &&
                       !DenseSetElemStubExists(cx, ICStub::SetElem_Dense, stub, obj))
            {
                JitSpew(JitSpew_BaselineIC,
                        "  Generating SetElem_Dense stub (shape=%p, group=%p)",
                        obj->lastProperty(), group.get());
                ICSetElem_Dense::Compiler compiler(cx, shape, group);
                ICUpdatedStub* denseStub = compiler.getStub(compiler.getStubSpace(script));
                if (!denseStub)
                    return false;
                if (!denseStub->addUpdateStubForValue(cx, script, obj, JSID_VOIDHANDLE, rhs))
                    return false;

                stub->addNewStub(denseStub);
            }
        }

        return true;
    }

    if ((IsAnyTypedArray(obj.get()) || IsPrimitiveArrayTypedObject(obj)) &&
        index.isNumber() &&
        rhs.isNumber())
    {
        if (!cx->runtime()->jitSupportsFloatingPoint &&
            (TypedThingRequiresFloatingPoint(obj) || index.isDouble()))
        {
            return true;
        }

        bool expectOutOfBounds;
        double idx = index.toNumber();
        if (IsAnyTypedArray(obj)) {
            expectOutOfBounds = (idx < 0 || idx >= double(AnyTypedArrayLength(obj)));
        } else {
            // Typed objects throw on out of bounds accesses. Don't attach
            // a stub in this case.
            if (idx < 0 || idx >= double(obj->as<TypedObject>().length()))
                return true;
            expectOutOfBounds = false;

            // Don't attach stubs if typed objects in the compartment might be
            // neutered, as the stub will always bail out.
            if (cx->compartment()->neuteredTypedObjects)
                return true;
        }

        if (!TypedArraySetElemStubExists(stub, obj, expectOutOfBounds)) {
            // Remove any existing TypedArraySetElemStub that doesn't handle out-of-bounds
            if (expectOutOfBounds)
                RemoveExistingTypedArraySetElemStub(cx, stub, obj);

            Shape* shape = obj->lastProperty();
            Scalar::Type type = TypedThingElementType(obj);

            JitSpew(JitSpew_BaselineIC,
                    "  Generating SetElem_TypedArray stub (shape=%p, type=%u, oob=%s)",
                    shape, type, expectOutOfBounds ? "yes" : "no");
            ICSetElem_TypedArray::Compiler compiler(cx, shape, type, expectOutOfBounds);
            ICStub* typedArrayStub = compiler.getStub(compiler.getStubSpace(script));
            if (!typedArrayStub)
                return false;

            stub->addNewStub(typedArrayStub);
            return true;
        }
    }

    return true;
}

typedef bool (*DoSetElemFallbackFn)(JSContext*, BaselineFrame*, ICSetElem_Fallback*, Value*,
                                    HandleValue, HandleValue, HandleValue);
static const VMFunction DoSetElemFallbackInfo =
    FunctionInfo<DoSetElemFallbackFn>(DoSetElemFallback, TailCall, PopValues(2));

bool
ICSetElem_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    // State: R0: object, R1: index, stack: rhs.
    // For the decompiler, the stack has to be: object, index, rhs,
    // so we push the index, then overwrite the rhs Value with R0
    // and push the rhs value.
    masm.pushValue(R1);
    masm.loadValue(Address(BaselineStackReg, sizeof(Value)), R1);
    masm.storeValue(R0, Address(BaselineStackReg, sizeof(Value)));
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1); // RHS

    // Push index. On x86 and ARM two push instructions are emitted so use a
    // separate register to store the old stack pointer.
    masm.mov(BaselineStackReg, R1.scratchReg());
    masm.pushValue(Address(R1.scratchReg(), 2 * sizeof(Value)));
    masm.pushValue(R0); // Object.

    // Push pointer to stack values, so that the stub can overwrite the object
    // (pushed for the decompiler) with the rhs.
    masm.computeEffectiveAddress(Address(BaselineStackReg, 3 * sizeof(Value)), R0.scratchReg());
    masm.push(R0.scratchReg());

    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoSetElemFallbackInfo, masm);
}

void
BaselineScript::noteArrayWriteHole(uint32_t pcOffset)
{
    ICEntry& entry = icEntryFromPCOffset(pcOffset);
    ICFallbackStub* stub = entry.fallbackStub();

    if (stub->isSetElem_Fallback())
        stub->toSetElem_Fallback()->noteArrayWriteHole();
}

//
// SetElem_Dense
//

bool
ICSetElem_Dense::Compiler::generateStubCode(MacroAssembler& masm)
{
    // R0 = object
    // R1 = key
    // Stack = { ... rhs-value, <return-addr>? }
    Label failure;
    Label failureUnstow;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox R0 and guard on its shape.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICSetElem_Dense::offsetOfShape()), scratchReg);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratchReg, &failure);

    // Stow both R0 and R1 (object and key)
    // But R0 and R1 still hold their values.
    EmitStowICValues(masm, 2);

    // We may need to free up some registers.
    regs = availableGeneralRegs(0);
    regs.take(R0);

    // Guard that the object group matches.
    Register typeReg = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICSetElem_Dense::offsetOfGroup()), typeReg);
    masm.branchPtr(Assembler::NotEqual, Address(obj, JSObject::offsetOfGroup()), typeReg,
                   &failureUnstow);
    regs.add(typeReg);

    // Stack is now: { ..., rhs-value, object-value, key-value, maybe?-RET-ADDR }
    // Load rhs-value in to R0
    masm.loadValue(Address(BaselineStackReg, 2 * sizeof(Value) + ICStackValueOffset), R0);

    // Call the type-update stub.
    if (!callTypeUpdateIC(masm, sizeof(Value)))
        return false;

    // Unstow R0 and R1 (object and key)
    EmitUnstowICValues(masm, 2);

    // Reset register set.
    regs = availableGeneralRegs(2);
    scratchReg = regs.takeAny();

    // Unbox object and key.
    obj = masm.extractObject(R0, ExtractTemp0);
    Register key = masm.extractInt32(R1, ExtractTemp1);

    // Load obj->elements in scratchReg.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratchReg);

    // Bounds check.
    Address initLength(scratchReg, ObjectElements::offsetOfInitializedLength());
    masm.branch32(Assembler::BelowOrEqual, initLength, key, &failure);

    // Hole check.
    BaseIndex element(scratchReg, key, TimesEight);
    masm.branchTestMagic(Assembler::Equal, element, &failure);

    // Perform a single test to see if we either need to convert double
    // elements or clone the copy on write elements in the object.
    Label noSpecialHandling;
    Address elementsFlags(scratchReg, ObjectElements::offsetOfFlags());
    masm.branchTest32(Assembler::Zero, elementsFlags,
                      Imm32(ObjectElements::CONVERT_DOUBLE_ELEMENTS |
                            ObjectElements::COPY_ON_WRITE),
                      &noSpecialHandling);

    // Fail if we need to clone copy on write elements.
    masm.branchTest32(Assembler::NonZero, elementsFlags,
                      Imm32(ObjectElements::COPY_ON_WRITE),
                      &failure);

    // Failure is not possible now.  Free up registers.
    regs.add(R0);
    regs.add(R1);
    regs.takeUnchecked(obj);
    regs.takeUnchecked(key);
    Address valueAddr(BaselineStackReg, ICStackValueOffset);

    // We need to convert int32 values being stored into doubles. In this case
    // the heap typeset is guaranteed to contain both int32 and double, so it's
    // okay to store a double. Note that double arrays are only created by
    // IonMonkey, so if we have no floating-point support Ion is disabled and
    // there should be no double arrays.
    if (cx->runtime()->jitSupportsFloatingPoint)
        masm.convertInt32ValueToDouble(valueAddr, regs.getAny(), &noSpecialHandling);
    else
        masm.assumeUnreachable("There shouldn't be double arrays when there is no FP support.");

    masm.bind(&noSpecialHandling);

    // Don't overwrite R0 becuase |obj| might overlap with it, and it's needed
    // for post-write barrier later.
    ValueOperand tmpVal = regs.takeAnyValue();
    masm.loadValue(valueAddr, tmpVal);
    EmitPreBarrier(masm, element, MIRType_Value);
    masm.storeValue(tmpVal, element);
    regs.add(key);
    if (cx->runtime()->gc.nursery.exists()) {
        Register r = regs.takeAny();
        GeneralRegisterSet saveRegs;
        emitPostWriteBarrierSlot(masm, obj, tmpVal, r, saveRegs);
        regs.add(r);
    }
    EmitReturnFromIC(masm);


    // Failure case - fail but first unstow R0 and R1
    masm.bind(&failureUnstow);
    EmitUnstowICValues(masm, 2);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

static bool
GetProtoShapes(JSObject* obj, size_t protoChainDepth, AutoShapeVector* shapes)
{
    MOZ_ASSERT(shapes->length() == 1);
    JSObject* curProto = obj->getProto();
    for (size_t i = 0; i < protoChainDepth; i++) {
        if (!shapes->append(curProto->lastProperty()))
            return false;
        curProto = curProto->getProto();
    }
    MOZ_ASSERT(!curProto);
    return true;
}

//
// SetElem_DenseAdd
//

ICUpdatedStub*
ICSetElemDenseAddCompiler::getStub(ICStubSpace* space)
{
    AutoShapeVector shapes(cx);
    if (!shapes.append(obj_->lastProperty()))
        return nullptr;

    if (!GetProtoShapes(obj_, protoChainDepth_, &shapes))
        return nullptr;

    JS_STATIC_ASSERT(ICSetElem_DenseAdd::MAX_PROTO_CHAIN_DEPTH == 4);

    ICUpdatedStub* stub = nullptr;
    switch (protoChainDepth_) {
      case 0: stub = getStubSpecific<0>(space, &shapes); break;
      case 1: stub = getStubSpecific<1>(space, &shapes); break;
      case 2: stub = getStubSpecific<2>(space, &shapes); break;
      case 3: stub = getStubSpecific<3>(space, &shapes); break;
      case 4: stub = getStubSpecific<4>(space, &shapes); break;
      default: MOZ_CRASH("ProtoChainDepth too high.");
    }
    if (!stub || !stub->initUpdatingChain(cx, space))
        return nullptr;
    return stub;
}

bool
ICSetElemDenseAddCompiler::generateStubCode(MacroAssembler& masm)
{
    // R0 = object
    // R1 = key
    // Stack = { ... rhs-value, <return-addr>? }
    Label failure;
    Label failureUnstow;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox R0 and guard on its shape.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICSetElem_DenseAddImpl<0>::offsetOfShape(0)),
                 scratchReg);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratchReg, &failure);

    // Stow both R0 and R1 (object and key)
    // But R0 and R1 still hold their values.
    EmitStowICValues(masm, 2);

    // We may need to free up some registers.
    regs = availableGeneralRegs(0);
    regs.take(R0);

    // Guard that the object group matches.
    Register typeReg = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICSetElem_DenseAdd::offsetOfGroup()), typeReg);
    masm.branchPtr(Assembler::NotEqual, Address(obj, JSObject::offsetOfGroup()), typeReg,
                   &failureUnstow);
    regs.add(typeReg);

    // Shape guard objects on the proto chain.
    scratchReg = regs.takeAny();
    Register protoReg = regs.takeAny();
    for (size_t i = 0; i < protoChainDepth_; i++) {
        masm.loadObjProto(i == 0 ? obj : protoReg, protoReg);
        masm.branchTestPtr(Assembler::Zero, protoReg, protoReg, &failureUnstow);
        masm.loadPtr(Address(BaselineStubReg, ICSetElem_DenseAddImpl<0>::offsetOfShape(i + 1)),
                     scratchReg);
        masm.branchTestObjShape(Assembler::NotEqual, protoReg, scratchReg, &failureUnstow);
    }
    regs.add(protoReg);
    regs.add(scratchReg);

    // Stack is now: { ..., rhs-value, object-value, key-value, maybe?-RET-ADDR }
    // Load rhs-value in to R0
    masm.loadValue(Address(BaselineStackReg, 2 * sizeof(Value) + ICStackValueOffset), R0);

    // Call the type-update stub.
    if (!callTypeUpdateIC(masm, sizeof(Value)))
        return false;

    // Unstow R0 and R1 (object and key)
    EmitUnstowICValues(masm, 2);

    // Reset register set.
    regs = availableGeneralRegs(2);
    scratchReg = regs.takeAny();

    // Unbox obj and key.
    obj = masm.extractObject(R0, ExtractTemp0);
    Register key = masm.extractInt32(R1, ExtractTemp1);

    // Load obj->elements in scratchReg.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratchReg);

    // Bounds check (key == initLength)
    Address initLength(scratchReg, ObjectElements::offsetOfInitializedLength());
    masm.branch32(Assembler::NotEqual, initLength, key, &failure);

    // Capacity check.
    Address capacity(scratchReg, ObjectElements::offsetOfCapacity());
    masm.branch32(Assembler::BelowOrEqual, capacity, key, &failure);

    // Check for copy on write elements.
    Address elementsFlags(scratchReg, ObjectElements::offsetOfFlags());
    masm.branchTest32(Assembler::NonZero, elementsFlags,
                      Imm32(ObjectElements::COPY_ON_WRITE),
                      &failure);

    // Failure is not possible now.  Free up registers.
    regs.add(R0);
    regs.add(R1);
    regs.takeUnchecked(obj);
    regs.takeUnchecked(key);

    // Increment initLength before write.
    masm.add32(Imm32(1), initLength);

    // If length is now <= key, increment length before write.
    Label skipIncrementLength;
    Address length(scratchReg, ObjectElements::offsetOfLength());
    masm.branch32(Assembler::Above, length, key, &skipIncrementLength);
    masm.add32(Imm32(1), length);
    masm.bind(&skipIncrementLength);

    Address valueAddr(BaselineStackReg, ICStackValueOffset);

    // Convert int32 values to double if convertDoubleElements is set. In this
    // case the heap typeset is guaranteed to contain both int32 and double, so
    // it's okay to store a double.
    Label dontConvertDoubles;
    masm.branchTest32(Assembler::Zero, elementsFlags,
                      Imm32(ObjectElements::CONVERT_DOUBLE_ELEMENTS),
                      &dontConvertDoubles);
    // Note that double arrays are only created by IonMonkey, so if we have no
    // floating-point support Ion is disabled and there should be no double arrays.
    if (cx->runtime()->jitSupportsFloatingPoint)
        masm.convertInt32ValueToDouble(valueAddr, regs.getAny(), &dontConvertDoubles);
    else
        masm.assumeUnreachable("There shouldn't be double arrays when there is no FP support.");
    masm.bind(&dontConvertDoubles);

    // Write the value.  No need for pre-barrier since we're not overwriting an old value.
    ValueOperand tmpVal = regs.takeAnyValue();
    BaseIndex element(scratchReg, key, TimesEight);
    masm.loadValue(valueAddr, tmpVal);
    masm.storeValue(tmpVal, element);
    regs.add(key);
    if (cx->runtime()->gc.nursery.exists()) {
        Register r = regs.takeAny();
        GeneralRegisterSet saveRegs;
        emitPostWriteBarrierSlot(masm, obj, tmpVal, r, saveRegs);
        regs.add(r);
    }
    EmitReturnFromIC(masm);

    // Failure case - fail but first unstow R0 and R1
    masm.bind(&failureUnstow);
    EmitUnstowICValues(masm, 2);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// SetElem_TypedArray
//

// Write an arbitrary value to a typed array or typed object address at dest.
// If the value could not be converted to the appropriate format, jump to
// failure or failureModifiedScratch.
template <typename T>
static void
StoreToTypedArray(JSContext* cx, MacroAssembler& masm, Scalar::Type type, Address value, T dest,
                  Register scratch, Label* failure, Label* failureModifiedScratch)
{
    Label done;

    if (type == Scalar::Float32 || type == Scalar::Float64) {
        masm.ensureDouble(value, FloatReg0, failure);
        if (type == Scalar::Float32) {
            masm.convertDoubleToFloat32(FloatReg0, ScratchFloat32Reg);
            masm.storeToTypedFloatArray(type, ScratchFloat32Reg, dest);
        } else {
            masm.storeToTypedFloatArray(type, FloatReg0, dest);
        }
    } else if (type == Scalar::Uint8Clamped) {
        Label notInt32;
        masm.branchTestInt32(Assembler::NotEqual, value, &notInt32);
        masm.unboxInt32(value, scratch);
        masm.clampIntToUint8(scratch);

        Label clamped;
        masm.bind(&clamped);
        masm.storeToTypedIntArray(type, scratch, dest);
        masm.jump(&done);

        // If the value is a double, clamp to uint8 and jump back.
        // Else, jump to failure.
        masm.bind(&notInt32);
        if (cx->runtime()->jitSupportsFloatingPoint) {
            masm.branchTestDouble(Assembler::NotEqual, value, failure);
            masm.unboxDouble(value, FloatReg0);
            masm.clampDoubleToUint8(FloatReg0, scratch);
            masm.jump(&clamped);
        } else {
            masm.jump(failure);
        }
    } else {
        Label notInt32;
        masm.branchTestInt32(Assembler::NotEqual, value, &notInt32);
        masm.unboxInt32(value, scratch);

        Label isInt32;
        masm.bind(&isInt32);
        masm.storeToTypedIntArray(type, scratch, dest);
        masm.jump(&done);

        // If the value is a double, truncate and jump back.
        // Else, jump to failure.
        masm.bind(&notInt32);
        if (cx->runtime()->jitSupportsFloatingPoint) {
            masm.branchTestDouble(Assembler::NotEqual, value, failure);
            masm.unboxDouble(value, FloatReg0);
            masm.branchTruncateDouble(FloatReg0, scratch, failureModifiedScratch);
            masm.jump(&isInt32);
        } else {
            masm.jump(failure);
        }
    }

    masm.bind(&done);
}

bool
ICSetElem_TypedArray::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    if (layout_ != Layout_TypedArray)
        CheckForNeuteredTypedObject(cx, masm, &failure);

    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox R0 and shape guard.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICSetElem_TypedArray::offsetOfShape()), scratchReg);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratchReg, &failure);

    // Ensure the index is an integer.
    if (cx->runtime()->jitSupportsFloatingPoint) {
        Label isInt32;
        masm.branchTestInt32(Assembler::Equal, R1, &isInt32);
        {
            // If the index is a double, try to convert it to int32. It's okay
            // to convert -0 to 0: the shape check ensures the object is a typed
            // array so the difference is not observable.
            masm.branchTestDouble(Assembler::NotEqual, R1, &failure);
            masm.unboxDouble(R1, FloatReg0);
            masm.convertDoubleToInt32(FloatReg0, scratchReg, &failure, /* negZeroCheck = */false);
            masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R1);
        }
        masm.bind(&isInt32);
    } else {
        masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
    }

    // Unbox key.
    Register key = masm.extractInt32(R1, ExtractTemp1);

    // Bounds check.
    Label oobWrite;
    LoadTypedThingLength(masm, layout_, obj, scratchReg);
    masm.branch32(Assembler::BelowOrEqual, scratchReg, key,
                  expectOutOfBounds_ ? &oobWrite : &failure);

    // Load the elements vector.
    LoadTypedThingData(masm, layout_, obj, scratchReg);

    BaseIndex dest(scratchReg, key, ScaleFromElemWidth(Scalar::byteSize(type_)));
    Address value(BaselineStackReg, ICStackValueOffset);

    // We need a second scratch register. It's okay to clobber the type tag of
    // R0 or R1, as long as it's restored before jumping to the next stub.
    regs = availableGeneralRegs(0);
    regs.takeUnchecked(obj);
    regs.takeUnchecked(key);
    regs.take(scratchReg);
    Register secondScratch = regs.takeAny();

    Label failureModifiedSecondScratch;
    StoreToTypedArray(cx, masm, type_, value, dest,
                      secondScratch, &failure, &failureModifiedSecondScratch);
    EmitReturnFromIC(masm);

    if (failureModifiedSecondScratch.used()) {
        // Writing to secondScratch may have clobbered R0 or R1, restore them
        // first.
        masm.bind(&failureModifiedSecondScratch);
        masm.tagValue(JSVAL_TYPE_OBJECT, obj, R0);
        masm.tagValue(JSVAL_TYPE_INT32, key, R1);
    }

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    if (expectOutOfBounds_) {
        MOZ_ASSERT(layout_ == Layout_TypedArray);
        masm.bind(&oobWrite);
        EmitReturnFromIC(masm);
    }
    return true;
}

//
// In_Fallback
//

static bool
DoInFallback(JSContext* cx, ICIn_Fallback* stub, HandleValue key, HandleValue objValue,
             MutableHandleValue res)
{
    FallbackICSpew(cx, stub, "In");

    if (!objValue.isObject()) {
        js_ReportValueError(cx, JSMSG_IN_NOT_OBJECT, -1, objValue, NullPtr());
        return false;
    }

    RootedObject obj(cx, &objValue.toObject());

    bool cond = false;
    if (!OperatorIn(cx, key, obj, &cond))
        return false;

    res.setBoolean(cond);
    return true;
}

typedef bool (*DoInFallbackFn)(JSContext*, ICIn_Fallback*, HandleValue, HandleValue,
                               MutableHandleValue);
static const VMFunction DoInFallbackInfo =
    FunctionInfo<DoInFallbackFn>(DoInFallback, TailCall, PopValues(2));

bool
ICIn_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    // Sync for the decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(BaselineStubReg);

    return tailCallVM(DoInFallbackInfo, masm);
}

// Try to update all existing GetProp/GetName getter call stubs that match the
// given holder in place with a new shape and getter.  fallbackStub can be
// either an ICGetProp_Fallback or an ICGetName_Fallback.
//
// When kind == ICStub::GetProp_CallNative, callers should pass a null receiver,
// since in that case receiver and holder are the same thing.
static bool
UpdateExistingGetPropCallStubs(ICFallbackStub* fallbackStub,
                               ICStub::Kind kind,
                               HandleObject holder,
                               HandleObject receiver,
                               HandleFunction getter)
{
    MOZ_ASSERT(kind == ICStub::GetProp_CallScripted ||
               kind == ICStub::GetProp_CallNative ||
               kind == ICStub::GetProp_CallNativePrototype);
    MOZ_ASSERT(fallbackStub->isGetName_Fallback() ||
               fallbackStub->isGetProp_Fallback());
    bool foundMatchingStub = false;
    Shape* receiverShape = receiver ? receiver->lastProperty() : nullptr;
    for (ICStubConstIterator iter = fallbackStub->beginChainConst(); !iter.atEnd(); iter++) {
        if (iter->kind() == kind) {
            ICGetPropCallGetter* getPropStub = static_cast<ICGetPropCallGetter*>(*iter);
            if (getPropStub->holder() == holder) {
                // We want to update the holder shape to match the new one no
                // matter what, even if the receiver shape is different.
                Shape* cachedReceiverShape;
                if (kind == ICStub::GetProp_CallNative) {
                    cachedReceiverShape = nullptr;
                } else {
                    ICGetPropCallPrototypeGetter* stubWithReceiver =
                        static_cast<ICGetPropCallPrototypeGetter*>(getPropStub);
                    cachedReceiverShape = stubWithReceiver->receiverShape();
                }
                // We would like to assert that either
                // getPropStub->holderShape() != holder->lastProperty() or
                // receiverShape != cachedReceiverShape, but that assertion can
                // fail if there is something in a getter that changes something
                // that we guard on in our stub but don't check for before/after
                // differences across the set during stub generation.  For
                // example, a getter mutating the shape of the proto the getter
                // lives on would cause us to create an IC stub that never
                // matches as protos with the old shape flow into it, but always
                // matches post-get, which is where we are now.
                getPropStub->holderShape() = holder->lastProperty();
                // Make sure to update the getter, since a shape change might
                // have changed which getter we want to use.
                getPropStub->getter() = getter;
                if (receiverShape == cachedReceiverShape)
                    foundMatchingStub = true;
            }
        }
    }

    return foundMatchingStub;
}

// Try to update existing SetProp setter call stubs for the given holder in
// place with a new shape and setter.
static bool
UpdateExistingSetPropCallStubs(ICSetProp_Fallback* fallbackStub,
                              ICStub::Kind kind,
                              HandleObject holder,
                              HandleShape receiverShape,
                              HandleFunction setter)
{
    MOZ_ASSERT(kind == ICStub::SetProp_CallScripted ||
               kind == ICStub::SetProp_CallNative);
    bool foundMatchingStub = false;
    for (ICStubConstIterator iter = fallbackStub->beginChainConst(); !iter.atEnd(); iter++) {
        if (iter->kind() == kind) {
            ICSetPropCallSetter* setPropStub = static_cast<ICSetPropCallSetter*>(*iter);
            if (setPropStub->holder() == holder) {
                // We want to update the holder shape to match the new one no
                // matter what, even if the receiver shape is different.
                MOZ_ASSERT(setPropStub->holderShape() != holder->lastProperty() ||
                           setPropStub->shape() != receiverShape,
                           "Why didn't we end up using this stub?");
                setPropStub->holderShape() = holder->lastProperty();
                // Make sure to update the setter, since a shape change might
                // have changed which setter we want to use.
                setPropStub->setter() = setter;
                if (receiverShape == setPropStub->shape())
                    foundMatchingStub = true;
            }
        }
    }

    return foundMatchingStub;
}

// Attach an optimized stub for a GETGNAME/CALLGNAME op.
static bool
TryAttachGlobalNameStub(JSContext* cx, HandleScript script, jsbytecode* pc,
                        ICGetName_Fallback* stub, Handle<GlobalObject*> global,
                        HandlePropertyName name, bool* attached, bool* isTemporarilyUnoptimizable)
{
    MOZ_ASSERT(global->is<GlobalObject>());
    MOZ_ASSERT(!*attached);

    RootedId id(cx, NameToId(name));

    // The property must be found, and it must be found as a normal data property.
    RootedShape shape(cx);
    RootedNativeObject current(cx, global);
    while (true) {
        shape = current->lookup(cx, id);
        if (shape)
            break;
        JSObject* proto = current->getProto();
        if (!proto || !proto->is<NativeObject>())
            return true;
        current = &proto->as<NativeObject>();
    }

    // Instantiate this global property, for use during Ion compilation.
    if (IsIonEnabled(cx))
        EnsureTrackPropertyTypes(cx, current, id);

    if (shape->hasDefaultGetter() && shape->hasSlot()) {

        // TODO: if there's a previous stub discard it, or just update its Shape + slot?

        ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();
        ICStub* newStub;
        if (current == global) {
            MOZ_ASSERT(shape->slot() >= current->numFixedSlots());
            uint32_t slot = shape->slot() - current->numFixedSlots();

            JitSpew(JitSpew_BaselineIC, "  Generating GetName(GlobalName) stub");
            ICGetName_Global::Compiler compiler(cx, monitorStub, current->lastProperty(), slot);
            newStub = compiler.getStub(compiler.getStubSpace(script));
        } else {
            bool isFixedSlot;
            uint32_t offset;
            GetFixedOrDynamicSlotOffset(current, shape->slot(), &isFixedSlot, &offset);
            if (!IsCacheableGetPropReadSlot(global, current, shape))
                return true;

            JitSpew(JitSpew_BaselineIC, "  Generating GetName(GlobalName prototype) stub");
            ICGetPropNativeCompiler compiler(cx, ICStub::GetProp_NativePrototype, false, monitorStub,
                                             global, current, name, isFixedSlot, offset,
                                             /* inputDefinitelyObject = */ true);
            newStub = compiler.getStub(compiler.getStubSpace(script));
        }
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    // Try to add a getter stub. We don't handle scripted getters yet; if this
    // changes we need to make sure IonBuilder::getPropTryCommonGetter (which
    // requires a Baseline stub) handles non-outerized this objects correctly.
    bool isScripted;
    if (IsCacheableGetPropCall(cx, global, current, shape, &isScripted, isTemporarilyUnoptimizable) &&
        !isScripted)
    {
        ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();
        RootedFunction getter(cx, &shape->getterObject()->as<JSFunction>());
        ICStub* newStub;
        if (current == global) {
            JitSpew(JitSpew_BaselineIC, "  Generating GetName(GlobalName/NativeGetter) stub");
            if (UpdateExistingGetPropCallStubs(stub, ICStub::GetProp_CallNative,
                                               global, JS::NullPtr(), getter)) {
                return true;
            }
            ICGetProp_CallNative::Compiler compiler(cx, monitorStub, current,
                                                    getter, script->pcToOffset(pc),
                                                    /* outerClass = */ nullptr,
                                                    /* inputDefinitelyObject = */ true);
            newStub = compiler.getStub(compiler.getStubSpace(script));
        } else {
            JitSpew(JitSpew_BaselineIC, "  Generating GetName(GlobalName prototype/NativeGetter) stub");
            if (UpdateExistingGetPropCallStubs(stub, ICStub::GetProp_CallNativePrototype,
                                               current, global, getter)) {
                return true;
            }
            ICGetProp_CallNativePrototype::Compiler compiler(cx, monitorStub, global, current,
                                                             getter, script->pcToOffset(pc),
                                                             /* outerClass = */ nullptr,
                                                             /* inputDefinitelyObject = */ true);
            newStub = compiler.getStub(compiler.getStubSpace(script));
        }

        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    return true;
}

static bool
TryAttachScopeNameStub(JSContext* cx, HandleScript script, ICGetName_Fallback* stub,
                       HandleObject initialScopeChain, HandlePropertyName name, bool* attached)
{
    MOZ_ASSERT(!*attached);

    AutoShapeVector shapes(cx);
    RootedId id(cx, NameToId(name));
    RootedObject scopeChain(cx, initialScopeChain);

    Shape* shape = nullptr;
    while (scopeChain) {
        if (!shapes.append(scopeChain->lastProperty()))
            return false;

        if (scopeChain->is<GlobalObject>()) {
            shape = scopeChain->as<GlobalObject>().lookup(cx, id);
            if (shape)
                break;
            return true;
        }

        if (!scopeChain->is<ScopeObject>() || scopeChain->is<DynamicWithObject>())
            return true;

        // Check for an 'own' property on the scope. There is no need to
        // check the prototype as non-with scopes do not inherit properties
        // from any prototype.
        shape = scopeChain->as<NativeObject>().lookup(cx, id);
        if (shape)
            break;

        scopeChain = scopeChain->enclosingScope();
    }

    // We don't handle getters here. When this changes, we need to make sure
    // IonBuilder::getPropTryCommonGetter (which requires a Baseline stub to
    // work) handles non-outerized this objects correctly.

    if (!IsCacheableGetPropReadSlot(scopeChain, scopeChain, shape))
        return true;

    bool isFixedSlot;
    uint32_t offset;
    GetFixedOrDynamicSlotOffset(&scopeChain->as<NativeObject>(),
                                shape->slot(), &isFixedSlot, &offset);

    ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();
    ICStub* newStub;

    switch (shapes.length()) {
      case 1: {
        ICGetName_Scope<0>::Compiler compiler(cx, monitorStub, &shapes, isFixedSlot, offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 2: {
        ICGetName_Scope<1>::Compiler compiler(cx, monitorStub, &shapes, isFixedSlot, offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 3: {
        ICGetName_Scope<2>::Compiler compiler(cx, monitorStub, &shapes, isFixedSlot, offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 4: {
        ICGetName_Scope<3>::Compiler compiler(cx, monitorStub, &shapes, isFixedSlot, offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 5: {
        ICGetName_Scope<4>::Compiler compiler(cx, monitorStub, &shapes, isFixedSlot, offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 6: {
        ICGetName_Scope<5>::Compiler compiler(cx, monitorStub, &shapes, isFixedSlot, offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 7: {
        ICGetName_Scope<6>::Compiler compiler(cx, monitorStub, &shapes, isFixedSlot, offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      default:
        return true;
    }

    if (!newStub)
        return false;

    stub->addNewStub(newStub);
    *attached = true;
    return true;
}

static bool
DoGetNameFallback(JSContext* cx, BaselineFrame* frame, ICGetName_Fallback* stub_,
                  HandleObject scopeChain, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICGetName_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    mozilla::DebugOnly<JSOp> op = JSOp(*pc);
    FallbackICSpew(cx, stub, "GetName(%s)", js_CodeName[JSOp(*pc)]);

    MOZ_ASSERT(op == JSOP_GETNAME || op == JSOP_GETGNAME);

    RootedPropertyName name(cx, script->getName(pc));

    if (JSOp(pc[JSOP_GETGNAME_LENGTH]) == JSOP_TYPEOF) {
        if (!GetScopeNameForTypeOf(cx, scopeChain, name, res))
            return false;
    } else {
        if (!GetScopeName(cx, scopeChain, name, res))
            return false;
    }

    TypeScript::Monitor(cx, script, pc, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, script, res))
        return false;

    // Attach new stub.
    if (stub->numOptimizedStubs() >= ICGetName_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with generic stub.
        return true;
    }

    bool attached = false;
    bool isTemporarilyUnoptimizable = false;
    if (js_CodeSpec[*pc].format & JOF_GNAME) {
        Handle<GlobalObject*> global = scopeChain.as<GlobalObject>();
        if (!TryAttachGlobalNameStub(cx, script, pc, stub, global, name, &attached,
                                     &isTemporarilyUnoptimizable))
        {
            return false;
        }
    } else {
        if (!TryAttachScopeNameStub(cx, script, stub, scopeChain, name, &attached))
            return false;
    }

    if (!attached && !isTemporarilyUnoptimizable)
        stub->noteUnoptimizableAccess();
    return true;
}

typedef bool (*DoGetNameFallbackFn)(JSContext*, BaselineFrame*, ICGetName_Fallback*,
                                    HandleObject, MutableHandleValue);
static const VMFunction DoGetNameFallbackInfo = FunctionInfo<DoGetNameFallbackFn>(DoGetNameFallback, TailCall);

bool
ICGetName_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    masm.push(R0.scratchReg());
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoGetNameFallbackInfo, masm);
}

bool
ICGetName_Global::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    Register obj = R0.scratchReg();
    Register scratch = R1.scratchReg();

    // Shape guard.
    masm.loadPtr(Address(BaselineStubReg, ICGetName_Global::offsetOfShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratch, &failure);

    // Load dynamic slot.
    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), obj);
    masm.load32(Address(BaselineStubReg, ICGetName_Global::offsetOfSlot()), scratch);
    masm.loadValue(BaseIndex(obj, scratch, TimesEight), R0);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

template <size_t NumHops>
bool
ICGetName_Scope<NumHops>::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    GeneralRegisterSet regs(availableGeneralRegs(1));
    Register obj = R0.scratchReg();
    Register walker = regs.takeAny();
    Register scratch = regs.takeAny();

    // Use a local to silence Clang tautological-compare warning if NumHops is 0.
    size_t numHops = NumHops;

    for (size_t index = 0; index < NumHops + 1; index++) {
        Register scope = index ? walker : obj;

        // Shape guard.
        masm.loadPtr(Address(BaselineStubReg, ICGetName_Scope::offsetOfShape(index)), scratch);
        masm.branchTestObjShape(Assembler::NotEqual, scope, scratch, &failure);

        if (index < numHops)
            masm.extractObject(Address(scope, ScopeObject::offsetOfEnclosingScope()), walker);
    }

    Register scope = NumHops ? walker : obj;

    if (!isFixedSlot_) {
        masm.loadPtr(Address(scope, NativeObject::offsetOfSlots()), walker);
        scope = walker;
    }

    masm.load32(Address(BaselineStubReg, ICGetName_Scope::offsetOfOffset()), scratch);
    masm.loadValue(BaseIndex(scope, scratch, TimesOne), R0);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// BindName_Fallback
//

static bool
DoBindNameFallback(JSContext* cx, BaselineFrame* frame, ICBindName_Fallback* stub,
                   HandleObject scopeChain, MutableHandleValue res)
{
    jsbytecode* pc = stub->icEntry()->pc(frame->script());
    mozilla::DebugOnly<JSOp> op = JSOp(*pc);
    FallbackICSpew(cx, stub, "BindName(%s)", js_CodeName[JSOp(*pc)]);

    MOZ_ASSERT(op == JSOP_BINDNAME);

    RootedPropertyName name(cx, frame->script()->getName(pc));

    RootedObject scope(cx);
    if (!LookupNameUnqualified(cx, name, scopeChain, &scope))
        return false;

    res.setObject(*scope);
    return true;
}

typedef bool (*DoBindNameFallbackFn)(JSContext*, BaselineFrame*, ICBindName_Fallback*,
                                     HandleObject, MutableHandleValue);
static const VMFunction DoBindNameFallbackInfo =
    FunctionInfo<DoBindNameFallbackFn>(DoBindNameFallback, TailCall);

bool
ICBindName_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    masm.push(R0.scratchReg());
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoBindNameFallbackInfo, masm);
}

//
// GetIntrinsic_Fallback
//

static bool
DoGetIntrinsicFallback(JSContext* cx, BaselineFrame* frame, ICGetIntrinsic_Fallback* stub_,
                       MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICGetIntrinsic_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    mozilla::DebugOnly<JSOp> op = JSOp(*pc);
    FallbackICSpew(cx, stub, "GetIntrinsic(%s)", js_CodeName[JSOp(*pc)]);

    MOZ_ASSERT(op == JSOP_GETINTRINSIC);

    if (!GetIntrinsicOperation(cx, pc, res))
        return false;

    // An intrinsic operation will always produce the same result, so only
    // needs to be monitored once. Attach a stub to load the resulting constant
    // directly.

    TypeScript::Monitor(cx, script, pc, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    JitSpew(JitSpew_BaselineIC, "  Generating GetIntrinsic optimized stub");
    ICGetIntrinsic_Constant::Compiler compiler(cx, res);
    ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;

    stub->addNewStub(newStub);
    return true;
}

typedef bool (*DoGetIntrinsicFallbackFn)(JSContext*, BaselineFrame*, ICGetIntrinsic_Fallback*,
                                         MutableHandleValue);
static const VMFunction DoGetIntrinsicFallbackInfo =
    FunctionInfo<DoGetIntrinsicFallbackFn>(DoGetIntrinsicFallback, TailCall);

bool
ICGetIntrinsic_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoGetIntrinsicFallbackInfo, masm);
}

bool
ICGetIntrinsic_Constant::Compiler::generateStubCode(MacroAssembler& masm)
{
    masm.loadValue(Address(BaselineStubReg, ICGetIntrinsic_Constant::offsetOfValue()), R0);

    EmitReturnFromIC(masm);
    return true;
}

//
// GetProp_Fallback
//

static bool
TryAttachMagicArgumentsGetPropStub(JSContext* cx, JSScript* script, ICGetProp_Fallback* stub,
                                   HandlePropertyName name, HandleValue val, HandleValue res,
                                   bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (!val.isMagic(JS_OPTIMIZED_ARGUMENTS))
        return true;

    // Try handling arguments.callee on optimized arguments.
    if (name == cx->names().callee) {
        MOZ_ASSERT(!script->strict());

        JitSpew(JitSpew_BaselineIC, "  Generating GetProp(MagicArgs.callee) stub");

        // Unlike ICGetProp_ArgumentsLength, only magic argument stubs are
        // supported at the moment.
        ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();

        // XXXshu the compiler really should be stack allocated, but stack
        // allocating it causes the test_temporary_storage indexedDB test to
        // fail on GCC 4.7-compiled ARMv6 optimized builds on Android 2.3 and
        // below with a NotFoundError, despite that test never exercising this
        // code.
        //
        // Instead of tracking down the GCC bug, I've opted to heap allocate
        // instead.
        ScopedJSDeletePtr<ICGetProp_ArgumentsCallee::Compiler> compiler;
        compiler = js_new<ICGetProp_ArgumentsCallee::Compiler>(cx, monitorStub);
        if (!compiler)
            return false;
        ICStub* newStub = compiler->getStub(compiler->getStubSpace(script));
        if (!newStub)
            return false;
        stub->addNewStub(newStub);

        *attached = true;
        return true;
    }

    return true;
}

static bool
TryAttachLengthStub(JSContext* cx, JSScript* script, ICGetProp_Fallback* stub, HandleValue val,
                    HandleValue res, bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (val.isString()) {
        MOZ_ASSERT(res.isInt32());
        JitSpew(JitSpew_BaselineIC, "  Generating GetProp(String.length) stub");
        ICGetProp_StringLength::Compiler compiler(cx);
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        *attached = true;
        stub->addNewStub(newStub);
        return true;
    }

    if (val.isMagic(JS_OPTIMIZED_ARGUMENTS) && res.isInt32()) {
        JitSpew(JitSpew_BaselineIC, "  Generating GetProp(MagicArgs.length) stub");
        ICGetProp_ArgumentsLength::Compiler compiler(cx, ICGetProp_ArgumentsLength::Magic);
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        *attached = true;
        stub->addNewStub(newStub);
        return true;
    }

    if (!val.isObject())
        return true;

    RootedObject obj(cx, &val.toObject());

    if (obj->is<ArrayObject>() && res.isInt32()) {
        JitSpew(JitSpew_BaselineIC, "  Generating GetProp(Array.length) stub");
        ICGetProp_ArrayLength::Compiler compiler(cx);
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        *attached = true;
        stub->addNewStub(newStub);
        return true;
    }

    if (obj->is<ArgumentsObject>() && res.isInt32()) {
        JitSpew(JitSpew_BaselineIC, "  Generating GetProp(ArgsObj.length %s) stub",
                obj->is<StrictArgumentsObject>() ? "Strict" : "Normal");
        ICGetProp_ArgumentsLength::Which which = ICGetProp_ArgumentsLength::Normal;
        if (obj->is<StrictArgumentsObject>())
            which = ICGetProp_ArgumentsLength::Strict;
        ICGetProp_ArgumentsLength::Compiler compiler(cx, which);
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        *attached = true;
        stub->addNewStub(newStub);
        return true;
    }

    return true;
}

static bool
UpdateExistingGenerationalDOMProxyStub(ICGetProp_Fallback* stub,
                                       HandleObject obj)
{
    Value expandoSlot = GetProxyExtra(obj, GetDOMProxyExpandoSlot());
    MOZ_ASSERT(!expandoSlot.isObject() && !expandoSlot.isUndefined());
    ExpandoAndGeneration* expandoAndGeneration = (ExpandoAndGeneration*)expandoSlot.toPrivate();
    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        if (iter->isGetProp_CallDOMProxyWithGenerationNative()) {
            ICGetProp_CallDOMProxyWithGenerationNative* updateStub =
                iter->toGetProp_CallDOMProxyWithGenerationNative();
            if (updateStub->expandoAndGeneration() == expandoAndGeneration) {
                // Update generation
                uint32_t generation = expandoAndGeneration->generation;
                JitSpew(JitSpew_BaselineIC,
                        "  Updating existing stub with generation, old value: %i, "
                        "new value: %i", updateStub->generation(),
                        generation);
                updateStub->setGeneration(generation);
                return true;
            }
        }
    }
    return false;
}

static bool
HasUnanalyzedNewScript(JSObject* obj)
{
    if (obj->isSingleton())
        return false;

    TypeNewScript* newScript = obj->group()->newScript();
    if (newScript && !newScript->analyzed())
        return true;

    return false;
}

static void
StripPreliminaryObjectStubs(JSContext* cx, ICFallbackStub* stub)
{
    // Before the new script properties analysis has been performed on a type,
    // all instances of that type have the maximum number of fixed slots.
    // Afterwards, the objects (even the preliminary ones) might be changed
    // to reduce the number of fixed slots they have. If we generate stubs for
    // both the old and new number of fixed slots, the stub will look
    // polymorphic to IonBuilder when it is actually monomorphic. To avoid
    // this, strip out any stubs for preliminary objects before attaching a new
    // stub which isn't on a preliminary object.

    for (ICStubIterator iter = stub->beginChain(); !iter.atEnd(); iter++) {
        if (iter->isGetProp_Native() && iter->toGetProp_Native()->hasPreliminaryObject())
            iter.unlink(cx);
        else if (iter->isSetProp_Native() && iter->toSetProp_Native()->hasPreliminaryObject())
            iter.unlink(cx);
    }
}

static bool
TryAttachNativeGetPropStub(JSContext* cx, HandleScript script, jsbytecode* pc,
                           ICGetProp_Fallback* stub, HandlePropertyName name,
                           HandleValue val, HandleShape oldShape,
                           HandleValue res, bool* attached,
                           bool* isTemporarilyUnoptimizable)
{
    MOZ_ASSERT(!*attached);
    MOZ_ASSERT(!*isTemporarilyUnoptimizable);

    if (!val.isObject())
        return true;

    RootedObject obj(cx, &val.toObject());

    if (oldShape != obj->lastProperty()) {
        // No point attaching anything, since we know the shape guard will fail
        return true;
    }

    bool isDOMProxy;
    bool domProxyHasGeneration;
    DOMProxyShadowsResult domProxyShadowsResult;
    RootedShape shape(cx);
    RootedObject holder(cx);
    if (!EffectlesslyLookupProperty(cx, obj, name, &holder, &shape, &isDOMProxy,
                                    &domProxyShadowsResult, &domProxyHasGeneration))
    {
        return false;
    }

    bool isCallProp = (JSOp(*pc) == JSOP_CALLPROP);

    ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();
    if (!isDOMProxy && IsCacheableGetPropReadSlot(obj, holder, shape)) {
        bool isFixedSlot;
        uint32_t offset;
        GetFixedOrDynamicSlotOffset(&holder->as<NativeObject>(), shape->slot(), &isFixedSlot, &offset);

        // Instantiate this property for singleton holders, for use during Ion compilation.
        if (IsIonEnabled(cx))
            EnsureTrackPropertyTypes(cx, holder, NameToId(name));

        ICStub::Kind kind;
        if (obj == holder)
            kind = ICStub::GetProp_Native;
        else if (obj->isNative())
            kind = ICStub::GetProp_NativePrototype;
        else if (obj->is<UnboxedPlainObject>())
            kind = ICStub::GetProp_UnboxedPrototype;
        else
            MOZ_CRASH("Bad kind");

        JitSpew(JitSpew_BaselineIC, "  Generating GetProp(%s %s) stub",
                    isDOMProxy ? "DOMProxy" : "Native",
                    (obj == holder) ? "direct" : "prototype");
        ICGetPropNativeCompiler compiler(cx, kind, isCallProp, monitorStub, obj, holder,
                                         name, isFixedSlot, offset);
        ICGetPropNativeStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        if (HasUnanalyzedNewScript(obj))
            newStub->notePreliminaryObject();
        else
            StripPreliminaryObjectStubs(cx, stub);

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    bool isScripted = false;
    bool cacheableCall = obj->isNative() && IsCacheableGetPropCall(cx, obj, holder, shape, &isScripted,
                                                                   isTemporarilyUnoptimizable);

    // Try handling scripted getters.
    if (cacheableCall && isScripted && !isDOMProxy) {
#if JS_HAS_NO_SUCH_METHOD
        // It's hard to keep the original object alive through a call, and it's unlikely
        // that a getter will be used to generate functions for calling in CALLPROP locations.
        // Just don't attach stubs in that case.
        if (isCallProp)
            return true;
#endif

        // Don't handle scripted own property getters
        if (obj == holder)
            return true;

        RootedFunction callee(cx, &shape->getterObject()->as<JSFunction>());
        MOZ_ASSERT(obj != holder);
        MOZ_ASSERT(callee->hasScript());

        if (UpdateExistingGetPropCallStubs(stub, ICStub::GetProp_CallScripted,
                                           holder, obj, callee)) {
            *attached = true;
            return true;
        }

        JitSpew(JitSpew_BaselineIC, "  Generating GetProp(NativeObj/ScriptedGetter %s:%d) stub",
                    callee->nonLazyScript()->filename(), callee->nonLazyScript()->lineno());

        ICGetProp_CallScripted::Compiler compiler(cx, monitorStub, obj, holder, callee,
                                                  script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    // If it's a shadowed listbase proxy property, attach stub to call Proxy::get instead.
    if (isDOMProxy && domProxyShadowsResult == Shadows) {
        MOZ_ASSERT(obj == holder);
#if JS_HAS_NO_SUCH_METHOD
        if (isCallProp)
            return true;
#endif

        JitSpew(JitSpew_BaselineIC, "  Generating GetProp(DOMProxyProxy) stub");
        Rooted<ProxyObject*> proxy(cx, &obj->as<ProxyObject>());
        ICGetProp_DOMProxyShadowed::Compiler compiler(cx, monitorStub, proxy, name,
                                                      script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;
        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    const Class* outerClass = nullptr;
    if (!isDOMProxy && !obj->isNative()) {
        outerClass = obj->getClass();
        DebugOnly<JSObject*> outer = obj.get();
        obj = GetInnerObject(obj);
        MOZ_ASSERT(script->global().isNative());
        if (obj != &script->global())
            return true;
        // ICGetProp_CallNative*::Compiler::generateStubCode depends on this.
        MOZ_ASSERT(&((GetProxyDataLayout(outer)->values->privateSlot).toObject()) == obj);
        if (!EffectlesslyLookupProperty(cx, obj, name, &holder, &shape, &isDOMProxy,
                                        &domProxyShadowsResult, &domProxyHasGeneration))
        {
            return false;
        }
        cacheableCall = IsCacheableGetPropCall(cx, obj, holder, shape, &isScripted,
                                               isTemporarilyUnoptimizable, isDOMProxy);
    }

    // Try handling JSNative getters.
    if (!cacheableCall || isScripted)
        return true;

    if (!shape || !shape->hasGetterValue() || !shape->getterValue().isObject() ||
        !shape->getterObject()->is<JSFunction>())
    {
        return true;
    }

    RootedFunction callee(cx, &shape->getterObject()->as<JSFunction>());
    MOZ_ASSERT(callee->isNative());

    if (outerClass && (!callee->jitInfo() || callee->jitInfo()->needsOuterizedThisObject()))
        return true;

#if JS_HAS_NO_SUCH_METHOD
    // It's unlikely that a getter function will be used to generate functions for calling
    // in CALLPROP locations.  Just don't attach stubs in that case to avoid issues with
    // __noSuchMethod__ handling.
    if (isCallProp)
        return true;
#endif

    JitSpew(JitSpew_BaselineIC, "  Generating GetProp(%s%s/NativeGetter %p) stub",
            isDOMProxy ? "DOMProxyObj" : "NativeObj",
            isDOMProxy && domProxyHasGeneration ? "WithGeneration" : "",
            callee->native());

    ICStub* newStub = nullptr;
    if (isDOMProxy) {
        MOZ_ASSERT(obj != holder);
        ICStub::Kind kind;
        if (domProxyHasGeneration) {
            if (UpdateExistingGenerationalDOMProxyStub(stub, obj)) {
                *attached = true;
                return true;
            }
            kind = ICStub::GetProp_CallDOMProxyWithGenerationNative;
        } else {
            kind = ICStub::GetProp_CallDOMProxyNative;
        }
        Rooted<ProxyObject*> proxy(cx, &obj->as<ProxyObject>());
        ICGetPropCallDOMProxyNativeCompiler compiler(cx, kind, monitorStub, proxy, holder, callee,
                                                     script->pcToOffset(pc));
        newStub = compiler.getStub(compiler.getStubSpace(script));
    } else if (obj == holder) {
        if (UpdateExistingGetPropCallStubs(stub, ICStub::GetProp_CallNative,
                                           obj, JS::NullPtr(), callee)) {
            *attached = true;
            return true;
        }

        ICGetProp_CallNative::Compiler compiler(cx, monitorStub, obj, callee,
                                                script->pcToOffset(pc), outerClass);
        newStub = compiler.getStub(compiler.getStubSpace(script));
    } else {
        if (UpdateExistingGetPropCallStubs(stub, ICStub::GetProp_CallNativePrototype,
                                           holder, obj, callee)) {
            *attached = true;
            return true;
        }

        ICGetProp_CallNativePrototype::Compiler compiler(cx, monitorStub, obj, holder, callee,
                                                         script->pcToOffset(pc), outerClass);
        newStub = compiler.getStub(compiler.getStubSpace(script));
    }
    if (!newStub)
        return false;
    stub->addNewStub(newStub);
    *attached = true;
    return true;
}

static bool
TryAttachUnboxedGetPropStub(JSContext* cx, HandleScript script,
                            ICGetProp_Fallback* stub, HandlePropertyName name, HandleValue val,
                            bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (!cx->runtime()->jitSupportsFloatingPoint)
        return true;

    if (!val.isObject() || !val.toObject().is<UnboxedPlainObject>())
        return true;
    Rooted<UnboxedPlainObject*> obj(cx, &val.toObject().as<UnboxedPlainObject>());

    const UnboxedLayout::Property* property = obj->layout().lookup(name);
    if (!property)
        return true;

    ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();

    ICGetProp_Unboxed::Compiler compiler(cx, monitorStub, obj->group(),
                                         property->offset + UnboxedPlainObject::offsetOfData(),
                                         property->type);
    ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;
    stub->addNewStub(newStub);

    StripPreliminaryObjectStubs(cx, stub);

    *attached = true;
    return true;
}

static bool
TryAttachTypedObjectGetPropStub(JSContext* cx, HandleScript script,
                                ICGetProp_Fallback* stub, HandlePropertyName name, HandleValue val,
                                bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (!cx->runtime()->jitSupportsFloatingPoint)
        return true;

    if (!val.isObject() || !val.toObject().is<TypedObject>())
        return true;
    Rooted<TypedObject*> obj(cx, &val.toObject().as<TypedObject>());

    if (!obj->typeDescr().is<StructTypeDescr>())
        return true;
    Rooted<StructTypeDescr*> structDescr(cx, &obj->typeDescr().as<StructTypeDescr>());

    size_t fieldIndex;
    if (!structDescr->fieldIndex(NameToId(name), &fieldIndex))
        return true;

    Rooted<TypeDescr*> fieldDescr(cx, &structDescr->fieldDescr(fieldIndex));
    if (!fieldDescr->is<SimpleTypeDescr>())
        return true;

    uint32_t fieldOffset = structDescr->fieldOffset(fieldIndex);
    ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();

    ICGetProp_TypedObject::Compiler compiler(cx, monitorStub, obj->lastProperty(),
                                             fieldOffset, &fieldDescr->as<SimpleTypeDescr>());
    ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;
    stub->addNewStub(newStub);

    *attached = true;
    return true;
}

static bool
TryAttachPrimitiveGetPropStub(JSContext* cx, HandleScript script, jsbytecode* pc,
                              ICGetProp_Fallback* stub, HandlePropertyName name, HandleValue val,
                              HandleValue res, bool* attached)
{
    MOZ_ASSERT(!*attached);

    JSValueType primitiveType;
    RootedNativeObject proto(cx);
    Rooted<GlobalObject*> global(cx, &script->global());
    if (val.isString()) {
        primitiveType = JSVAL_TYPE_STRING;
        proto = GlobalObject::getOrCreateStringPrototype(cx, global);
    } else if (val.isSymbol()) {
        primitiveType = JSVAL_TYPE_SYMBOL;
        proto = GlobalObject::getOrCreateSymbolPrototype(cx, global);
    } else if (val.isNumber()) {
        primitiveType = JSVAL_TYPE_DOUBLE;
        proto = GlobalObject::getOrCreateNumberPrototype(cx, global);
    } else {
        MOZ_ASSERT(val.isBoolean());
        primitiveType = JSVAL_TYPE_BOOLEAN;
        proto = GlobalObject::getOrCreateBooleanPrototype(cx, global);
    }
    if (!proto)
        return false;

    // Instantiate this property, for use during Ion compilation.
    RootedId id(cx, NameToId(name));
    if (IsIonEnabled(cx))
        EnsureTrackPropertyTypes(cx, proto, id);

    // For now, only look for properties directly set on the prototype.
    RootedShape shape(cx, proto->lookup(cx, id));
    if (!shape || !shape->hasSlot() || !shape->hasDefaultGetter())
        return true;

    bool isFixedSlot;
    uint32_t offset;
    GetFixedOrDynamicSlotOffset(proto, shape->slot(), &isFixedSlot, &offset);

    ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();

    JitSpew(JitSpew_BaselineIC, "  Generating GetProp_Primitive stub");
    ICGetProp_Primitive::Compiler compiler(cx, monitorStub, primitiveType, proto,
                                           isFixedSlot, offset);
    ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;

    stub->addNewStub(newStub);
    *attached = true;
    return true;
}

static bool
TryAttachNativeDoesNotExistStub(JSContext* cx, HandleScript script, jsbytecode* pc,
                                ICGetProp_Fallback* stub, HandlePropertyName name,
                                HandleValue val, bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (!val.isObject())
        return true;

    RootedObject obj(cx, &val.toObject());

    // Don't attach stubs for CALLPROP since those need NoSuchMethod handling.
    if (JSOp(*pc) == JSOP_CALLPROP)
        return true;

    // Check if does-not-exist can be confirmed on property.
    RootedObject lastProto(cx);
    size_t protoChainDepth = SIZE_MAX;
    if (!CheckHasNoSuchProperty(cx, obj, name, &lastProto, &protoChainDepth))
        return true;
    MOZ_ASSERT(protoChainDepth < SIZE_MAX);

    if (protoChainDepth > ICGetProp_NativeDoesNotExist::MAX_PROTO_CHAIN_DEPTH)
        return true;

    ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();
    RootedShape objShape(cx, obj->lastProperty());
    RootedShape lastShape(cx, lastProto->lastProperty());

    // Confirmed no-such-property.  Add stub.
    JitSpew(JitSpew_BaselineIC, "  Generating GetProp_NativeDoesNotExist stub");
    ICGetPropNativeDoesNotExistCompiler compiler(cx, monitorStub, obj, protoChainDepth);
    ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;

    stub->addNewStub(newStub);
    *attached = true;
    return true;
}

static bool
ComputeGetPropResult(JSContext* cx, BaselineFrame* frame, JSOp op, HandlePropertyName name,
                     MutableHandleValue val, MutableHandleValue res)
{
    // Handle arguments.length and arguments.callee on optimized arguments, as
    // it is not an object.
    if (val.isMagic(JS_OPTIMIZED_ARGUMENTS) && IsOptimizedArguments(frame, val)) {
        if (op == JSOP_LENGTH) {
            res.setInt32(frame->numActualArgs());
        } else {
            MOZ_ASSERT(name == cx->names().callee);
            MOZ_ASSERT(!frame->script()->strict());
            res.setObject(*frame->callee());
        }
    } else {
        // Handle when val is an object.
        RootedObject obj(cx, ToObjectFromStack(cx, val));
        if (!obj)
            return false;

        RootedId id(cx, NameToId(name));
        if (op == JSOP_GETXPROP) {
            if (!GetPropertyForNameLookup(cx, obj, id, res))
                return false;
        } else {
            if (!GetProperty(cx, obj, obj, id, res))
                return false;
        }

#if JS_HAS_NO_SUCH_METHOD
        // Handle objects with __noSuchMethod__.
        if (op == JSOP_CALLPROP && MOZ_UNLIKELY(res.isUndefined()) && val.isObject()) {
            if (!OnUnknownMethod(cx, obj, IdToValue(id), res))
                return false;
        }
#endif
    }

    return true;
}

static bool
DoGetPropFallback(JSContext* cx, BaselineFrame* frame, ICGetProp_Fallback* stub_,
                  MutableHandleValue val, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICGetProp_Fallback*> stub(frame, stub_);

    jsbytecode* pc = stub->icEntry()->pc(frame->script());
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "GetProp(%s)", js_CodeName[op]);

    MOZ_ASSERT(op == JSOP_GETPROP || op == JSOP_CALLPROP || op == JSOP_LENGTH || op == JSOP_GETXPROP);

    // Grab our old shape before it goes away.
    RootedShape oldShape(cx);
    if (val.isObject())
        oldShape = val.toObject().lastProperty();

    // After the  Genericstub was added, we should never reach the Fallbackstub again.
    MOZ_ASSERT(!stub->hasStub(ICStub::GetProp_Generic));

    RootedPropertyName name(cx, frame->script()->getName(pc));
    if (!ComputeGetPropResult(cx, frame, op, name, val, res))
        return false;

    TypeScript::Monitor(cx, frame->script(), pc, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, frame->script(), res))
        return false;

    if (stub->numOptimizedStubs() >= ICGetProp_Fallback::MAX_OPTIMIZED_STUBS) {
        // Discard all stubs in this IC and replace with generic getprop stub.
        for(ICStubIterator iter = stub->beginChain(); !iter.atEnd(); iter++)
            iter.unlink(cx);
        ICGetProp_Generic::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub());
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(frame->script()));
        if (!newStub)
            return false;
        stub->addNewStub(newStub);
        return true;
    }

    bool attached = false;
    // There are some reasons we can fail to attach a stub that are temporary.
    // We want to avoid calling noteUnoptimizableAccess() if the reason we
    // failed to attach a stub is one of those temporary reasons, since we might
    // end up attaching a stub for the exact same access later.
    bool isTemporarilyUnoptimizable = false;

    if (op == JSOP_LENGTH) {
        if (!TryAttachLengthStub(cx, frame->script(), stub, val, res, &attached))
            return false;
        if (attached)
            return true;
    }

    if (!TryAttachMagicArgumentsGetPropStub(cx, frame->script(), stub, name, val, res, &attached))
        return false;
    if (attached)
        return true;

    RootedScript script(cx, frame->script());

    if (!TryAttachNativeGetPropStub(cx, script, pc, stub, name, val, oldShape,
                                    res, &attached, &isTemporarilyUnoptimizable))
        return false;
    if (attached)
        return true;

    if (!TryAttachUnboxedGetPropStub(cx, script, stub, name, val, &attached))
        return false;
    if (attached)
        return true;

    if (!TryAttachTypedObjectGetPropStub(cx, script, stub, name, val, &attached))
        return false;
    if (attached)
        return true;

    if (val.isString() || val.isNumber() || val.isBoolean()) {
        if (!TryAttachPrimitiveGetPropStub(cx, script, pc, stub, name, val, res, &attached))
            return false;
        if (attached)
            return true;
    }

    if (res.isUndefined()) {
        // Try attaching property-not-found optimized stub for undefined results.
        if (!TryAttachNativeDoesNotExistStub(cx, script, pc, stub, name, val, &attached))
            return false;
        if (attached)
            return true;
    }

    MOZ_ASSERT(!attached);
    if (!isTemporarilyUnoptimizable)
        stub->noteUnoptimizableAccess();

    return true;
}

typedef bool (*DoGetPropFallbackFn)(JSContext*, BaselineFrame*, ICGetProp_Fallback*,
                                    MutableHandleValue, MutableHandleValue);
static const VMFunction DoGetPropFallbackInfo =
    FunctionInfo<DoGetPropFallbackFn>(DoGetPropFallback, TailCall, PopValues(1));

bool
ICGetProp_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);

    // Push arguments.
    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    if (!tailCallVM(DoGetPropFallbackInfo, masm))
        return false;

    // What follows is bailout for inlined scripted getters.
    // The return address pointed to by the baseline stack points here.
    returnOffset_ = masm.currentOffset();

    // Even though the fallback frame doesn't enter a stub frame, the CallScripted
    // frame that we are emulating does. Again, we lie.
#ifdef DEBUG
    entersStubFrame_ = true;
#endif

    leaveStubFrame(masm, true);

    // When we get here, BaselineStubReg contains the ICGetProp_Fallback stub,
    // which we can't use to enter the TypeMonitor IC, because it's a MonitoredFallbackStub
    // instead of a MonitoredStub. So, we cheat.
    masm.loadPtr(Address(BaselineStubReg, ICMonitoredFallbackStub::offsetOfFallbackMonitorStub()),
                 BaselineStubReg);
    EmitEnterTypeMonitorIC(masm, ICTypeMonitor_Fallback::offsetOfFirstMonitorStub());

    return true;
}

bool
ICGetProp_Fallback::Compiler::postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code)
{
    CodeOffsetLabel offset(returnOffset_);
    offset.fixup(&masm);
    cx->compartment()->jitCompartment()->initBaselineGetPropReturnAddr(code->raw() + offset.offset());
    return true;
}

bool
ICGetProp_ArrayLength::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    Register scratch = R1.scratchReg();

    // Unbox R0 and guard it's an array.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.branchTestObjClass(Assembler::NotEqual, obj, scratch, &ArrayObject::class_, &failure);

    // Load obj->elements->length.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);
    masm.load32(Address(scratch, ObjectElements::offsetOfLength()), scratch);

    // Guard length fits in an int32.
    masm.branchTest32(Assembler::Signed, scratch, scratch, &failure);

    masm.tagValue(JSVAL_TYPE_INT32, scratch, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICGetProp_StringLength::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestString(Assembler::NotEqual, R0, &failure);

    // Unbox string and load its length.
    Register string = masm.extractString(R0, ExtractTemp0);
    masm.loadStringLength(string, string);

    masm.tagValue(JSVAL_TYPE_INT32, string, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICGetProp_Primitive::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    switch (primitiveType_) {
      case JSVAL_TYPE_STRING:
        masm.branchTestString(Assembler::NotEqual, R0, &failure);
        break;
      case JSVAL_TYPE_SYMBOL:
        masm.branchTestSymbol(Assembler::NotEqual, R0, &failure);
        break;
      case JSVAL_TYPE_DOUBLE: // Also used for int32.
        masm.branchTestNumber(Assembler::NotEqual, R0, &failure);
        break;
      case JSVAL_TYPE_BOOLEAN:
        masm.branchTestBoolean(Assembler::NotEqual, R0, &failure);
        break;
      default:
        MOZ_CRASH("unexpected type");
    }

    GeneralRegisterSet regs(availableGeneralRegs(1));
    Register holderReg = regs.takeAny();
    Register scratchReg = regs.takeAny();

    // Verify the shape of the prototype.
    masm.movePtr(ImmGCPtr(prototype_.get()), holderReg);

    Address shapeAddr(BaselineStubReg, ICGetProp_Primitive::offsetOfProtoShape());
    masm.loadPtr(Address(holderReg, JSObject::offsetOfShape()), scratchReg);
    masm.branchPtr(Assembler::NotEqual, shapeAddr, scratchReg, &failure);

    if (!isFixedSlot_)
        masm.loadPtr(Address(holderReg, NativeObject::offsetOfSlots()), holderReg);

    masm.load32(Address(BaselineStubReg, ICGetProp_Primitive::offsetOfOffset()), scratchReg);
    masm.loadValue(BaseIndex(holderReg, scratchReg, TimesOne), R0);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

ICGetPropNativeStub*
ICGetPropNativeCompiler::getStub(ICStubSpace* space)
{
    switch (kind) {
      case ICStub::GetProp_Native: {
        MOZ_ASSERT(obj_ == holder_);
        RootedShape shape(cx, obj_->lastProperty());
        return ICStub::New<ICGetProp_Native>(space, getStubCode(), firstMonitorStub_, shape, offset_);
      }

      case ICStub::GetProp_NativePrototype: {
        MOZ_ASSERT(obj_ != holder_);
        RootedShape shape(cx, obj_->lastProperty());
        RootedShape holderShape(cx, holder_->lastProperty());
        return ICStub::New<ICGetProp_NativePrototype>(space, getStubCode(), firstMonitorStub_, shape,
                                                      offset_, holder_, holderShape);
      }

      case ICStub::GetProp_UnboxedPrototype: {
        MOZ_ASSERT(obj_ != holder_);
        RootedObjectGroup group(cx, obj_->group());
        RootedShape holderShape(cx, holder_->lastProperty());
        return ICStub::New<ICGetProp_UnboxedPrototype>(space, getStubCode(), firstMonitorStub_, group,
                                                       offset_, holder_, holderShape);
      }

      default:
        MOZ_CRASH("Bad stub kind");
    }
}

bool
ICGetPropNativeCompiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    GeneralRegisterSet regs(availableGeneralRegs(0));
    Register objReg = InvalidReg;

    if (inputDefinitelyObject_) {
        objReg = R0.scratchReg();
    } else {
        regs.take(R0);
        // Guard input is an object and unbox.
        masm.branchTestObject(Assembler::NotEqual, R0, &failure);
        objReg = masm.extractObject(R0, ExtractTemp0);
    }
    regs.takeUnchecked(objReg);

    Register scratch = regs.takeAnyExcluding(BaselineTailCallReg);

    // Shape/group guard.
    if (kind == ICStub::GetProp_UnboxedPrototype) {
        masm.loadPtr(Address(BaselineStubReg, ICGetProp_UnboxedPrototype::offsetOfGroup()), scratch);
        masm.branchPtr(Assembler::NotEqual, Address(objReg, JSObject::offsetOfGroup()), scratch,
                       &failure);
    } else {
        MOZ_ASSERT(ICGetProp_Native::offsetOfShape() ==
                   ICGetProp_NativePrototype::offsetOfShape());
        masm.loadPtr(Address(BaselineStubReg, ICGetProp_Native::offsetOfShape()), scratch);
        masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failure);
    }

    Register holderReg;
    if (obj_ == holder_) {
        holderReg = objReg;
    } else {
        // Shape guard holder.
        MOZ_ASSERT(ICGetProp_NativePrototype::offsetOfHolder() ==
                   ICGetProp_UnboxedPrototype::offsetOfHolder());
        MOZ_ASSERT(ICGetProp_NativePrototype::offsetOfHolderShape() ==
                   ICGetProp_UnboxedPrototype::offsetOfHolderShape());
        holderReg = regs.takeAny();
        masm.loadPtr(Address(BaselineStubReg, ICGetProp_NativePrototype::offsetOfHolder()),
                     holderReg);
        masm.loadPtr(Address(BaselineStubReg, ICGetProp_NativePrototype::offsetOfHolderShape()),
                     scratch);
        masm.branchTestObjShape(Assembler::NotEqual, holderReg, scratch, &failure);
    }

    if (!isFixedSlot_) {
        // Don't overwrite actual holderReg if we need to load a dynamic slots object.
        // May need to preserve object for noSuchMethod check later.
        Register nextHolder = regs.takeAny();
        masm.loadPtr(Address(holderReg, NativeObject::offsetOfSlots()), nextHolder);
        holderReg = nextHolder;
    }

    masm.load32(Address(BaselineStubReg, ICGetPropNativeStub::offsetOfOffset()), scratch);
    BaseIndex result(holderReg, scratch, TimesOne);

#if JS_HAS_NO_SUCH_METHOD
#ifdef DEBUG
    entersStubFrame_ = true;
#endif
    if (isCallProp_) {
        // Check for __noSuchMethod__ invocation.
        Label afterNoSuchMethod;
        Label skipNoSuchMethod;

        masm.push(objReg);
        masm.loadValue(result, R0);
        masm.branchTestUndefined(Assembler::NotEqual, R0, &skipNoSuchMethod);

        masm.pop(objReg);

        // Call __noSuchMethod__ checker.  Object pointer is in objReg.
        regs = availableGeneralRegs(0);
        regs.takeUnchecked(objReg);
        regs.takeUnchecked(BaselineTailCallReg);
        ValueOperand val = regs.takeValueOperand();

        // Box and push obj onto baseline frame stack for decompiler.
        EmitRestoreTailCallReg(masm);
        masm.tagValue(JSVAL_TYPE_OBJECT, objReg, val);
        masm.pushValue(val);
        EmitRepushTailCallReg(masm);

        enterStubFrame(masm, regs.getAnyExcluding(BaselineTailCallReg));

        masm.movePtr(ImmGCPtr(propName_.get()), val.scratchReg());
        masm.tagValue(JSVAL_TYPE_STRING, val.scratchReg(), val);
        masm.pushValue(val);
        masm.push(objReg);
        if (!callVM(LookupNoSuchMethodHandlerInfo, masm))
            return false;

        leaveStubFrame(masm);

        // Pop pushed obj from baseline stack.
        EmitUnstowICValues(masm, 1, /* discard = */ true);

        masm.jump(&afterNoSuchMethod);
        masm.bind(&skipNoSuchMethod);

        // Pop pushed objReg.
        masm.addPtr(Imm32(sizeof(void*)), BaselineStackReg);
        masm.bind(&afterNoSuchMethod);
    } else {
        masm.loadValue(result, R0);
    }
#else
    masm.loadValue(result, R0);
#endif

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

ICStub*
ICGetPropNativeDoesNotExistCompiler::getStub(ICStubSpace* space)
{
    AutoShapeVector shapes(cx);
    if (!shapes.append(obj_->lastProperty()))
        return nullptr;

    if (!GetProtoShapes(obj_, protoChainDepth_, &shapes))
        return nullptr;

    JS_STATIC_ASSERT(ICGetProp_NativeDoesNotExist::MAX_PROTO_CHAIN_DEPTH == 8);

    ICStub* stub = nullptr;
    switch(protoChainDepth_) {
      case 0: stub = getStubSpecific<0>(space, &shapes); break;
      case 1: stub = getStubSpecific<1>(space, &shapes); break;
      case 2: stub = getStubSpecific<2>(space, &shapes); break;
      case 3: stub = getStubSpecific<3>(space, &shapes); break;
      case 4: stub = getStubSpecific<4>(space, &shapes); break;
      case 5: stub = getStubSpecific<5>(space, &shapes); break;
      case 6: stub = getStubSpecific<6>(space, &shapes); break;
      case 7: stub = getStubSpecific<7>(space, &shapes); break;
      case 8: stub = getStubSpecific<8>(space, &shapes); break;
      default: MOZ_CRASH("ProtoChainDepth too high.");
    }
    if (!stub)
        return nullptr;
    return stub;
}

bool
ICGetPropNativeDoesNotExistCompiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    GeneralRegisterSet regs(availableGeneralRegs(1));
    Register scratch = regs.takeAny();

#ifdef DEBUG
    // Ensure that protoChainDepth_ matches the protoChainDepth stored on the stub.
    {
        Label ok;
        masm.load16ZeroExtend(Address(BaselineStubReg, ICStub::offsetOfExtra()), scratch);
        masm.branch32(Assembler::Equal, scratch, Imm32(protoChainDepth_), &ok);
        masm.assumeUnreachable("Non-matching proto chain depth on stub.");
        masm.bind(&ok);
    }
#endif // DEBUG

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Unbox and guard against old shape.
    Register objReg = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_NativeDoesNotExist::offsetOfShape(0)),
                 scratch);
    masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failure);

    Register protoReg = regs.takeAny();
    // Check the proto chain.
    for (size_t i = 0; i < protoChainDepth_; i++) {
        masm.loadObjProto(i == 0 ? objReg : protoReg, protoReg);
        masm.branchTestPtr(Assembler::Zero, protoReg, protoReg, &failure);
        size_t shapeOffset = ICGetProp_NativeDoesNotExistImpl<0>::offsetOfShape(i + 1);
        masm.loadPtr(Address(BaselineStubReg, shapeOffset), scratch);
        masm.branchTestObjShape(Assembler::NotEqual, protoReg, scratch, &failure);
    }

    // Shape and type checks succeeded, ok to proceed.
    masm.moveValue(UndefinedValue(), R0);

    // Normally for this op, the result would have to be monitored by TI.
    // However, since this stub ALWAYS returns UndefinedValue(), and we can be sure
    // that undefined is already registered with the type-set, this can be avoided.
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICGetProp_CallScripted::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    Label failureLeaveStubFrame;
    GeneralRegisterSet regs(availableGeneralRegs(1));
    Register scratch = regs.takeAnyExcluding(BaselineTailCallReg);

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Unbox and shape guard.
    Register objReg = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallScripted::offsetOfReceiverShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failure);

    Register holderReg = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallScripted::offsetOfHolder()), holderReg);
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallScripted::offsetOfHolderShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, holderReg, scratch, &failure);
    regs.add(holderReg);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, scratch);

    // Load callee function and code.  To ensure that |code| doesn't end up being
    // ArgumentsRectifierReg, if it's available we assign it to |callee| instead.
    Register callee;
    if (regs.has(ArgumentsRectifierReg)) {
        callee = ArgumentsRectifierReg;
        regs.take(callee);
    } else {
        callee = regs.takeAny();
    }
    Register code = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallScripted::offsetOfGetter()), callee);
    masm.branchIfFunctionHasNoScript(callee, &failureLeaveStubFrame);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), code);
    masm.loadBaselineOrIonRaw(code, code, &failureLeaveStubFrame);

    // Align the stack such that the JitFrameLayout is aligned on
    // JitStackAlignment.
    masm.alignJitStackBasedOnNArgs(0);

    // Getter is called with 0 arguments, just |obj| as thisv.
    // Note that we use Push, not push, so that callJit will align the stack
    // properly on ARM.
    masm.Push(R0);
    EmitCreateStubFrameDescriptor(masm, scratch);
    masm.Push(Imm32(0));  // ActualArgc is 0
    masm.Push(callee);
    masm.Push(scratch);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), scratch);
    masm.branch32(Assembler::Equal, scratch, Imm32(0), &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != code);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), code);
        masm.loadPtr(Address(code, JitCode::offsetOfCode()), code);
        masm.mov(ImmWord(0), ArgumentsRectifierReg);
    }

    masm.bind(&noUnderflow);
    masm.callJit(code);

    leaveStubFrame(masm, true);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Leave stub frame and go to next stub.
    masm.bind(&failureLeaveStubFrame);
    leaveStubFrame(masm, false);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICGetProp_CallNative::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    GeneralRegisterSet regs(availableGeneralRegs(0));
    Register obj = InvalidReg;

    MOZ_ASSERT(!(inputDefinitelyObject_ && outerClass_));
    if (inputDefinitelyObject_) {
        obj = R0.scratchReg();
    } else {
        regs.take(R0);
        masm.branchTestObject(Assembler::NotEqual, R0, &failure);
        obj = masm.extractObject(R0, ExtractTemp0);
        if (outerClass_) {
            ValueOperand val = regs.takeAnyValue();
            Register tmp = regs.takeAny();
            masm.branchTestObjClass(Assembler::NotEqual, obj, tmp, outerClass_, &failure);
            masm.loadPtr(Address(obj, ProxyDataOffset + offsetof(ProxyDataLayout, values)), tmp);
            masm.loadValue(Address(tmp, offsetof(ProxyValueArray, privateSlot)), val);
            obj = masm.extractObject(val, ExtractTemp0);
            regs.add(val);
            regs.add(tmp);
        }
    }
    regs.takeUnchecked(obj);

    Register scratch = regs.takeAnyExcluding(BaselineTailCallReg);

    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallNative::offsetOfHolderShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratch, &failure);

    enterStubFrame(masm, scratch);

    masm.Push(obj);

    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallNative::offsetOfGetter()), scratch);
    masm.Push(scratch);

    regs.add(scratch);
    if (!inputDefinitelyObject_)
        regs.add(R0);

    if (!callVM(DoCallNativeGetterInfo, masm))
        return false;
    leaveStubFrame(masm);

    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    return true;
}

bool
ICGetProp_CallNativePrototype::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    GeneralRegisterSet regs(availableGeneralRegs(0));
    Register objReg = InvalidReg;

    MOZ_ASSERT(!(inputDefinitelyObject_ && outerClass_));
    if (inputDefinitelyObject_) {
        objReg = R0.scratchReg();
    } else {
        regs.take(R0);
        // Guard input is an object and unbox.
        masm.branchTestObject(Assembler::NotEqual, R0, &failure);
        objReg = masm.extractObject(R0, ExtractTemp0);
        if (outerClass_) {
            ValueOperand val = regs.takeAnyValue();
            Register tmp = regs.takeAny();
            masm.branchTestObjClass(Assembler::NotEqual, objReg, tmp, outerClass_, &failure);
            masm.loadPtr(Address(objReg, ProxyDataOffset + offsetof(ProxyDataLayout, values)), tmp);
            masm.loadValue(Address(tmp, offsetof(ProxyValueArray, privateSlot)), val);
            objReg = masm.extractObject(val, ExtractTemp0);
            regs.add(val);
            regs.add(tmp);
        }
    }
    regs.takeUnchecked(objReg);

    Register scratch = regs.takeAnyExcluding(BaselineTailCallReg);

    // Shape guard.
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallNativePrototype::offsetOfReceiverShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failure);

    Register holderReg = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallNativePrototype::offsetOfHolder()), holderReg);
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallNativePrototype::offsetOfHolderShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, holderReg, scratch, &failure);
    regs.add(holderReg);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, scratch);

    // Load callee function.
    Register callee = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallNativePrototype::offsetOfGetter()), callee);

    // Push args for vm call.
    masm.push(objReg);
    masm.push(callee);

    if (!inputDefinitelyObject_)
        regs.add(R0);
    else
        regs.add(objReg);

    if (!callVM(DoCallNativeGetterInfo, masm))
        return false;
    leaveStubFrame(masm);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICGetPropCallDOMProxyNativeCompiler::generateStubCode(MacroAssembler& masm,
                                                      Address* expandoAndGenerationAddr,
                                                      Address* generationAddr)
{
    Label failure;
    GeneralRegisterSet regs(availableGeneralRegs(1));
    Register scratch = regs.takeAnyExcluding(BaselineTailCallReg);

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Unbox.
    Register objReg = masm.extractObject(R0, ExtractTemp0);

    // Shape guard.
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallDOMProxyNative::offsetOfShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failure);

    // Guard that our expando object hasn't started shadowing this property.
    {
        GeneralRegisterSet domProxyRegSet(GeneralRegisterSet::All());
        domProxyRegSet.take(BaselineStubReg);
        domProxyRegSet.take(objReg);
        domProxyRegSet.take(scratch);
        Address expandoShapeAddr(BaselineStubReg, ICGetProp_CallDOMProxyNative::offsetOfExpandoShape());
        CheckDOMProxyExpandoDoesNotShadow(
                cx, masm, objReg,
                expandoShapeAddr, expandoAndGenerationAddr, generationAddr,
                scratch,
                domProxyRegSet,
                &failure);
    }

    Register holderReg = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallDOMProxyNative::offsetOfHolder()),
                 holderReg);
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallDOMProxyNative::offsetOfHolderShape()),
                 scratch);
    masm.branchTestObjShape(Assembler::NotEqual, holderReg, scratch, &failure);
    regs.add(holderReg);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, scratch);

    // Load callee function.
    Register callee = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_CallDOMProxyNative::offsetOfGetter()), callee);

    // Push args for vm call.
    masm.push(objReg);
    masm.push(callee);

    // Don't have to preserve R0 anymore.
    regs.add(R0);

    if (!callVM(DoCallNativeGetterInfo, masm))
        return false;
    leaveStubFrame(masm);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICGetPropCallDOMProxyNativeCompiler::generateStubCode(MacroAssembler& masm)
{
    if (kind == ICStub::GetProp_CallDOMProxyNative)
        return generateStubCode(masm, nullptr, nullptr);

    Address internalStructAddress(BaselineStubReg,
        ICGetProp_CallDOMProxyWithGenerationNative::offsetOfInternalStruct());
    Address generationAddress(BaselineStubReg,
        ICGetProp_CallDOMProxyWithGenerationNative::offsetOfGeneration());
    return generateStubCode(masm, &internalStructAddress, &generationAddress);
}

ICStub*
ICGetPropCallDOMProxyNativeCompiler::getStub(ICStubSpace* space)
{
    RootedShape shape(cx, proxy_->lastProperty());
    RootedShape holderShape(cx, holder_->lastProperty());

    Value expandoSlot = GetProxyExtra(proxy_, GetDOMProxyExpandoSlot());
    RootedShape expandoShape(cx, nullptr);
    ExpandoAndGeneration* expandoAndGeneration;
    int32_t generation;
    Value expandoVal;
    if (kind == ICStub::GetProp_CallDOMProxyNative) {
        expandoVal = expandoSlot;
        expandoAndGeneration = nullptr;  // initialize to silence GCC warning
        generation = 0;  // initialize to silence GCC warning
    } else {
        MOZ_ASSERT(kind == ICStub::GetProp_CallDOMProxyWithGenerationNative);
        MOZ_ASSERT(!expandoSlot.isObject() && !expandoSlot.isUndefined());
        expandoAndGeneration = (ExpandoAndGeneration*)expandoSlot.toPrivate();
        expandoVal = expandoAndGeneration->expando;
        generation = expandoAndGeneration->generation;
    }

    if (expandoVal.isObject())
        expandoShape = expandoVal.toObject().lastProperty();

    if (kind == ICStub::GetProp_CallDOMProxyNative) {
        return ICStub::New<ICGetProp_CallDOMProxyNative>(
            space, getStubCode(), firstMonitorStub_, shape,
            expandoShape, holder_, holderShape, getter_, pcOffset_);
    }

    return ICStub::New<ICGetProp_CallDOMProxyWithGenerationNative>(
        space, getStubCode(), firstMonitorStub_, shape,
        expandoAndGeneration, generation, expandoShape, holder_, holderShape, getter_,
        pcOffset_);
}

ICStub*
ICGetProp_DOMProxyShadowed::Compiler::getStub(ICStubSpace* space)
{
    RootedShape shape(cx, proxy_->lastProperty());
    return New<ICGetProp_DOMProxyShadowed>(space, getStubCode(), firstMonitorStub_, shape,
                                           proxy_->handler(), name_, pcOffset_);
}

static bool
ProxyGet(JSContext* cx, HandleObject proxy, HandlePropertyName name, MutableHandleValue vp)
{
    RootedId id(cx, NameToId(name));
    return Proxy::get(cx, proxy, proxy, id, vp);
}

typedef bool (*ProxyGetFn)(JSContext* cx, HandleObject proxy, HandlePropertyName name,
                           MutableHandleValue vp);
static const VMFunction ProxyGetInfo = FunctionInfo<ProxyGetFn>(ProxyGet);

bool
ICGetProp_DOMProxyShadowed::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    GeneralRegisterSet regs(availableGeneralRegs(1));
    // Need to reserve a scratch register, but the scratch register should not be
    // BaselineTailCallReg, because it's used for |enterStubFrame| which needs a
    // non-BaselineTailCallReg scratch reg.
    Register scratch = regs.takeAnyExcluding(BaselineTailCallReg);

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Unbox.
    Register objReg = masm.extractObject(R0, ExtractTemp0);

    // Shape guard.
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_DOMProxyShadowed::offsetOfShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failure);

    // No need to do any more guards; it's safe to call ProxyGet even
    // if we've since stopped shadowing.

    // Call ProxyGet(JSContext* cx, HandleObject proxy, HandlePropertyName name, MutableHandleValue vp);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, scratch);

    // Push property name and proxy object.
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_DOMProxyShadowed::offsetOfName()), scratch);
    masm.push(scratch);
    masm.push(objReg);

    // Don't have to preserve R0 anymore.
    regs.add(R0);

    if (!callVM(ProxyGetInfo, masm))
        return false;
    leaveStubFrame(masm);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICGetProp_ArgumentsLength::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    if (which_ == ICGetProp_ArgumentsLength::Magic) {
        // Ensure that this is lazy arguments.
        masm.branchTestMagicValue(Assembler::NotEqual, R0, JS_OPTIMIZED_ARGUMENTS, &failure);

        // Ensure that frame has not loaded different arguments object since.
        masm.branchTest32(Assembler::NonZero,
                          Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFlags()),
                          Imm32(BaselineFrame::HAS_ARGS_OBJ),
                          &failure);

        Address actualArgs(BaselineFrameReg, BaselineFrame::offsetOfNumActualArgs());
        masm.loadPtr(actualArgs, R0.scratchReg());
        masm.tagValue(JSVAL_TYPE_INT32, R0.scratchReg(), R0);
        EmitReturnFromIC(masm);

        masm.bind(&failure);
        EmitStubGuardFailure(masm);
        return true;
    }
    MOZ_ASSERT(which_ == ICGetProp_ArgumentsLength::Strict ||
               which_ == ICGetProp_ArgumentsLength::Normal);

    bool isStrict = which_ == ICGetProp_ArgumentsLength::Strict;
    const Class* clasp = isStrict ? &StrictArgumentsObject::class_ : &NormalArgumentsObject::class_;

    Register scratchReg = R1.scratchReg();

    // Guard on input being an arguments object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    Register objReg = masm.extractObject(R0, ExtractTemp0);
    masm.branchTestObjClass(Assembler::NotEqual, objReg, scratchReg, clasp, &failure);

    // Get initial length value.
    masm.unboxInt32(Address(objReg, ArgumentsObject::getInitialLengthSlotOffset()), scratchReg);

    // Test if length has been overridden.
    masm.branchTest32(Assembler::NonZero,
                      scratchReg,
                      Imm32(ArgumentsObject::LENGTH_OVERRIDDEN_BIT),
                      &failure);

    // Nope, shift out arguments length and return it.
    // No need to type monitor because this stub always returns Int32.
    masm.rshiftPtr(Imm32(ArgumentsObject::PACKED_BITS_COUNT), scratchReg);
    masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

ICGetProp_ArgumentsCallee::ICGetProp_ArgumentsCallee(JitCode* stubCode, ICStub* firstMonitorStub)
  : ICMonitoredStub(GetProp_ArgumentsCallee, stubCode, firstMonitorStub)
{ }

bool
ICGetProp_ArgumentsCallee::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    // Ensure that this is lazy arguments.
    masm.branchTestMagicValue(Assembler::NotEqual, R0, JS_OPTIMIZED_ARGUMENTS, &failure);

    // Ensure that frame has not loaded different arguments object since.
    masm.branchTest32(Assembler::NonZero,
                      Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFlags()),
                      Imm32(BaselineFrame::HAS_ARGS_OBJ),
                      &failure);

    Address callee(BaselineFrameReg, BaselineFrame::offsetOfCalleeToken());
    masm.loadFunctionFromCalleeToken(callee, R0.scratchReg());
    masm.tagValue(JSVAL_TYPE_OBJECT, R0.scratchReg(), R0);

    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

/* static */ ICGetProp_Generic*
ICGetProp_Generic::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                         ICGetProp_Generic& other)
{
    return New<ICGetProp_Generic>(space, other.jitCode(), firstMonitorStub);
}

static bool
DoGetPropGeneric(JSContext* cx, BaselineFrame* frame, ICGetProp_Generic* stub, MutableHandleValue val, MutableHandleValue res)
{
    jsbytecode* pc = stub->getChainFallback()->icEntry()->pc(frame->script());
    JSOp op = JSOp(*pc);
    RootedPropertyName name(cx, frame->script()->getName(pc));
    return ComputeGetPropResult(cx, frame, op, name, val, res);
}

typedef bool (*DoGetPropGenericFn)(JSContext*, BaselineFrame*, ICGetProp_Generic*, MutableHandleValue, MutableHandleValue);
static const VMFunction DoGetPropGenericInfo = FunctionInfo<DoGetPropGenericFn>(DoGetPropGeneric);

bool
ICGetProp_Generic::Compiler::generateStubCode(MacroAssembler& masm)
{
    GeneralRegisterSet regs(availableGeneralRegs(1));

    Register scratch = regs.takeAnyExcluding(BaselineTailCallReg);

    // Sync for the decompiler.
    EmitStowICValues(masm, 1);

    enterStubFrame(masm, scratch);

    // Push arguments.
    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.loadPtr(Address(BaselineFrameReg, 0), R0.scratchReg());
    masm.pushBaselineFramePtr(R0.scratchReg(), R0.scratchReg());

    if(!callVM(DoGetPropGenericInfo, masm))
        return false;

    leaveStubFrame(masm);
    EmitUnstowICValues(masm, 1, /* discard = */ true);
    EmitEnterTypeMonitorIC(masm);
    return true;
}

bool
ICGetProp_Unboxed::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    GeneralRegisterSet regs(availableGeneralRegs(1));

    Register scratch = regs.takeAnyExcluding(BaselineTailCallReg);

    // Object and group guard.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    Register object = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_Unboxed::offsetOfGroup()), scratch);
    masm.branchPtr(Assembler::NotEqual, Address(object, JSObject::offsetOfGroup()), scratch,
                   &failure);

    // Get the address being read from.
    masm.load32(Address(BaselineStubReg, ICGetProp_Unboxed::offsetOfFieldOffset()), scratch);

    masm.loadUnboxedProperty(BaseIndex(object, scratch, TimesOne), fieldType_, TypedOrValueRegister(R0));

    // Only monitor the result if its type might change.
    if (fieldType_ == JSVAL_TYPE_OBJECT)
        EmitEnterTypeMonitorIC(masm);
    else
        EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    return true;
}

bool
ICGetProp_TypedObject::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    CheckForNeuteredTypedObject(cx, masm, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(1));

    Register scratch1 = regs.takeAnyExcluding(BaselineTailCallReg);
    Register scratch2 = regs.takeAnyExcluding(BaselineTailCallReg);

    // Object and shape guard.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    Register object = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICGetProp_TypedObject::offsetOfShape()), scratch1);
    masm.branchTestObjShape(Assembler::NotEqual, object, scratch1, &failure);

    // Get the object's data pointer.
    LoadTypedThingData(masm, layout_, object, scratch1);

    // Get the address being written to.
    masm.load32(Address(BaselineStubReg, ICGetProp_TypedObject::offsetOfFieldOffset()), scratch2);
    masm.addPtr(scratch2, scratch1);

    // Only monitor the result if the type produced by this stub might vary.
    bool monitorLoad;

    if (fieldDescr_->is<ScalarTypeDescr>()) {
        Scalar::Type type = fieldDescr_->as<ScalarTypeDescr>().type();
        monitorLoad = type == Scalar::Uint32;

        masm.loadFromTypedArray(type, Address(scratch1, 0), R0, /* allowDouble = */ true,
                                scratch2, nullptr);
    } else {
        ReferenceTypeDescr::Type type = fieldDescr_->as<ReferenceTypeDescr>().type();
        monitorLoad = type != ReferenceTypeDescr::TYPE_STRING;

        switch (type) {
          case ReferenceTypeDescr::TYPE_ANY:
            masm.loadValue(Address(scratch1, 0), R0);
            break;

          case ReferenceTypeDescr::TYPE_OBJECT: {
            Label notNull, done;
            masm.loadPtr(Address(scratch1, 0), scratch1);
            masm.branchTestPtr(Assembler::NonZero, scratch1, scratch1, &notNull);
            masm.moveValue(NullValue(), R0);
            masm.jump(&done);
            masm.bind(&notNull);
            masm.tagValue(JSVAL_TYPE_OBJECT, scratch1, R0);
            masm.bind(&done);
            break;
          }

          case ReferenceTypeDescr::TYPE_STRING:
            masm.loadPtr(Address(scratch1, 0), scratch1);
            masm.tagValue(JSVAL_TYPE_STRING, scratch1, R0);
            break;

          default:
            MOZ_CRASH();
        }
    }

    if (monitorLoad)
        EmitEnterTypeMonitorIC(masm);
    else
        EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    return true;
}

void
BaselineScript::noteAccessedGetter(uint32_t pcOffset)
{
    ICEntry& entry = icEntryFromPCOffset(pcOffset);
    ICFallbackStub* stub = entry.fallbackStub();

    if (stub->isGetProp_Fallback())
        stub->toGetProp_Fallback()->noteAccessedGetter();
}

//
// SetProp_Fallback
//

// Attach an optimized property set stub for a SETPROP/SETGNAME/SETNAME op on a
// value property.
static bool
TryAttachSetValuePropStub(JSContext* cx, HandleScript script, jsbytecode* pc, ICSetProp_Fallback* stub,
                          HandleObject obj, HandleShape oldShape, HandleObjectGroup oldGroup, uint32_t oldSlots,
                          HandlePropertyName name, HandleId id, HandleValue rhs, bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (!obj->isNative() || obj->watched())
        return true;

    RootedShape shape(cx);
    RootedObject holder(cx);
    if (!EffectlesslyLookupProperty(cx, obj, name, &holder, &shape))
        return false;

    size_t chainDepth;
    if (IsCacheableSetPropAddSlot(cx, obj, oldShape, oldSlots, id, holder, shape, &chainDepth)) {
        // Don't attach if proto chain depth is too high.
        if (chainDepth > ICSetProp_NativeAdd::MAX_PROTO_CHAIN_DEPTH)
            return true;

        // Don't attach if we are adding a property to an object which the new
        // script properties analysis hasn't been performed for yet, as there
        // may be a shape change required here afterwards. Pretend we attached
        // a stub, though, so the access is not marked as unoptimizable.
        if (oldGroup->newScript() && !oldGroup->newScript()->analyzed()) {
            *attached = true;
            return true;
        }

        bool isFixedSlot;
        uint32_t offset;
        GetFixedOrDynamicSlotOffset(&obj->as<NativeObject>(), shape->slot(), &isFixedSlot, &offset);

        JitSpew(JitSpew_BaselineIC, "  Generating SetProp(NativeObject.ADD) stub");
        ICSetPropNativeAddCompiler compiler(cx, obj, oldShape, oldGroup,
                                            chainDepth, isFixedSlot, offset);
        ICUpdatedStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;
        if (!newStub->addUpdateStubForValue(cx, script, obj, id, rhs))
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    if (IsCacheableSetPropWriteSlot(obj, oldShape, holder, shape)) {
        // For some property writes, such as the initial overwrite of global
        // properties, TI will not mark the property as having been
        // overwritten. Don't attach a stub in this case, so that we don't
        // execute another write to the property without TI seeing that write.
        EnsureTrackPropertyTypes(cx, obj, id);
        if (!PropertyHasBeenMarkedNonConstant(obj, id)) {
            *attached = true;
            return true;
        }

        bool isFixedSlot;
        uint32_t offset;
        GetFixedOrDynamicSlotOffset(&obj->as<NativeObject>(), shape->slot(), &isFixedSlot, &offset);

        JitSpew(JitSpew_BaselineIC, "  Generating SetProp(NativeObject.PROP) stub");
        MOZ_ASSERT(obj->lastProperty() == oldShape,
                   "Should this really be a SetPropWriteSlot?");
        ICSetProp_Native::Compiler compiler(cx, obj, isFixedSlot, offset);
        ICSetProp_Native* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;
        if (!newStub->addUpdateStubForValue(cx, script, obj, id, rhs))
            return false;

        if (HasUnanalyzedNewScript(obj))
            newStub->notePreliminaryObject();
        else
            StripPreliminaryObjectStubs(cx, stub);

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    return true;
}

// Attach an optimized property set stub for a SETPROP/SETGNAME/SETNAME op on
// an accessor property.
static bool
TryAttachSetAccessorPropStub(JSContext* cx, HandleScript script, jsbytecode* pc, ICSetProp_Fallback* stub,
                             HandleObject obj, HandleShape receiverShape, HandlePropertyName name,
                             HandleId id, HandleValue rhs, bool* attached,
                             bool* isTemporarilyUnoptimizable)
{
    MOZ_ASSERT(!*attached);
    MOZ_ASSERT(!*isTemporarilyUnoptimizable);

    if (!obj->isNative() || obj->watched())
        return true;

    RootedShape shape(cx);
    RootedObject holder(cx);
    if (!EffectlesslyLookupProperty(cx, obj, name, &holder, &shape))
        return false;

    bool isScripted = false;
    bool cacheableCall = IsCacheableSetPropCall(cx, obj, holder, shape,
                                                &isScripted, isTemporarilyUnoptimizable);

    // Try handling scripted setters.
    if (cacheableCall && isScripted) {
        RootedFunction callee(cx, &shape->setterObject()->as<JSFunction>());
        MOZ_ASSERT(obj != holder);
        MOZ_ASSERT(callee->hasScript());

        if (UpdateExistingSetPropCallStubs(stub, ICStub::SetProp_CallScripted,
                                           holder, receiverShape, callee)) {
            *attached = true;
            return true;
        }

        JitSpew(JitSpew_BaselineIC, "  Generating SetProp(NativeObj/ScriptedSetter %s:%d) stub",
                    callee->nonLazyScript()->filename(), callee->nonLazyScript()->lineno());

        ICSetProp_CallScripted::Compiler compiler(cx, obj, holder, callee, script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    // Try handling JSNative setters.
    if (cacheableCall && !isScripted) {
        RootedFunction callee(cx, &shape->setterObject()->as<JSFunction>());
        MOZ_ASSERT(obj != holder);
        MOZ_ASSERT(callee->isNative());

        if (UpdateExistingSetPropCallStubs(stub, ICStub::SetProp_CallNative,
                                           holder, receiverShape, callee)) {
            *attached = true;
            return true;
        }

        JitSpew(JitSpew_BaselineIC, "  Generating SetProp(NativeObj/NativeSetter %p) stub",
                    callee->native());

        ICSetProp_CallNative::Compiler compiler(cx, obj, holder, callee, script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    return true;
}

static bool
TryAttachUnboxedSetPropStub(JSContext* cx, HandleScript script,
                            ICSetProp_Fallback* stub, HandleId id,
                            HandleObject obj, HandleValue rhs, bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (!cx->runtime()->jitSupportsFloatingPoint)
        return true;

    if (!obj->is<UnboxedPlainObject>())
        return true;

    const UnboxedLayout::Property* property = obj->as<UnboxedPlainObject>().layout().lookup(id);
    if (!property)
        return true;

    ICSetProp_Unboxed::Compiler compiler(cx, obj->group(),
                                         property->offset + UnboxedPlainObject::offsetOfData(),
                                         property->type);
    ICUpdatedStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;
    if (compiler.needsUpdateStubs() && !newStub->addUpdateStubForValue(cx, script, obj, id, rhs))
        return false;

    stub->addNewStub(newStub);

    StripPreliminaryObjectStubs(cx, stub);

    *attached = true;
    return true;
}

static bool
TryAttachTypedObjectSetPropStub(JSContext* cx, HandleScript script,
                                ICSetProp_Fallback* stub, HandleId id,
                                HandleObject obj, HandleValue rhs, bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (!cx->runtime()->jitSupportsFloatingPoint)
        return true;

    if (!obj->is<TypedObject>())
        return true;

    if (!obj->as<TypedObject>().typeDescr().is<StructTypeDescr>())
        return true;
    Rooted<StructTypeDescr*> structDescr(cx);
    structDescr = &obj->as<TypedObject>().typeDescr().as<StructTypeDescr>();

    size_t fieldIndex;
    if (!structDescr->fieldIndex(id, &fieldIndex))
        return true;

    Rooted<TypeDescr*> fieldDescr(cx, &structDescr->fieldDescr(fieldIndex));
    if (!fieldDescr->is<SimpleTypeDescr>())
        return true;

    uint32_t fieldOffset = structDescr->fieldOffset(fieldIndex);

    ICSetProp_TypedObject::Compiler compiler(cx, obj->lastProperty(), obj->group(), fieldOffset,
                                             &fieldDescr->as<SimpleTypeDescr>());
    ICUpdatedStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;
    if (compiler.needsUpdateStubs() && !newStub->addUpdateStubForValue(cx, script, obj, id, rhs))
        return false;

    stub->addNewStub(newStub);

    *attached = true;
    return true;
}

static bool
DoSetPropFallback(JSContext* cx, BaselineFrame* frame, ICSetProp_Fallback* stub_,
                  HandleValue lhs, HandleValue rhs, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICSetProp_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "SetProp(%s)", js_CodeName[op]);

    MOZ_ASSERT(op == JSOP_SETPROP ||
               op == JSOP_STRICTSETPROP ||
               op == JSOP_SETNAME ||
               op == JSOP_STRICTSETNAME ||
               op == JSOP_SETGNAME ||
               op == JSOP_STRICTSETGNAME ||
               op == JSOP_INITPROP ||
               op == JSOP_SETALIASEDVAR ||
               op == JSOP_INITALIASEDLEXICAL);

    RootedPropertyName name(cx);
    if (op == JSOP_SETALIASEDVAR || op == JSOP_INITALIASEDLEXICAL)
        name = ScopeCoordinateName(cx->runtime()->scopeCoordinateNameCache, script, pc);
    else
        name = script->getName(pc);
    RootedId id(cx, NameToId(name));

    RootedObject obj(cx, ToObjectFromStack(cx, lhs));
    if (!obj)
        return false;
    RootedShape oldShape(cx, obj->lastProperty());
    RootedObjectGroup oldGroup(cx, obj->getGroup(cx));
    if (!oldGroup)
        return false;
    uint32_t oldSlots = obj->isNative() ? obj->as<NativeObject>().numDynamicSlots() : 0;

    bool attached = false;
    // There are some reasons we can fail to attach a stub that are temporary.
    // We want to avoid calling noteUnoptimizableAccess() if the reason we
    // failed to attach a stub is one of those temporary reasons, since we might
    // end up attaching a stub for the exact same access later.
    bool isTemporarilyUnoptimizable = false;
    if (stub->numOptimizedStubs() < ICSetProp_Fallback::MAX_OPTIMIZED_STUBS &&
        lhs.isObject() &&
        !TryAttachSetAccessorPropStub(cx, script, pc, stub, obj, oldShape, name, id,
                                      rhs, &attached, &isTemporarilyUnoptimizable))
    {
        return false;
    }

    if (op == JSOP_INITPROP) {
        MOZ_ASSERT(obj->is<PlainObject>());
        if (!NativeDefineProperty(cx, obj.as<PlainObject>(), id, rhs,
                                  nullptr, nullptr, JSPROP_ENUMERATE))
        {
            return false;
        }
    } else if (op == JSOP_SETNAME ||
               op == JSOP_STRICTSETNAME ||
               op == JSOP_SETGNAME ||
               op == JSOP_STRICTSETGNAME)
    {
        if (!SetNameOperation(cx, script, pc, obj, rhs))
            return false;
    } else if (op == JSOP_SETALIASEDVAR || op == JSOP_INITALIASEDLEXICAL) {
        obj->as<ScopeObject>().setAliasedVar(cx, ScopeCoordinate(pc), name, rhs);
    } else {
        MOZ_ASSERT(op == JSOP_SETPROP || op == JSOP_STRICTSETPROP);

        RootedValue v(cx, rhs);
        if (!SetProperty(cx, obj, obj, id, &v, op == JSOP_STRICTSETPROP))
            return false;
    }

    // Leave the RHS on the stack.
    res.set(rhs);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (stub->numOptimizedStubs() >= ICSetProp_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with generic setprop stub.
        return true;
    }

    if (!attached &&
        lhs.isObject() &&
        !TryAttachSetValuePropStub(cx, script, pc, stub, obj, oldShape,
                                   oldGroup, oldSlots, name, id, rhs, &attached))
    {
        return false;
    }
    if (attached)
        return true;

    if (!attached &&
        lhs.isObject() &&
        !TryAttachUnboxedSetPropStub(cx, script, stub, id, obj, rhs, &attached))
    {
        return false;
    }
    if (attached)
        return true;

    if (!attached &&
        lhs.isObject() &&
        !TryAttachTypedObjectSetPropStub(cx, script, stub, id, obj, rhs, &attached))
    {
        return false;
    }
    if (attached)
        return true;

    MOZ_ASSERT(!attached);
    if (!isTemporarilyUnoptimizable)
        stub->noteUnoptimizableAccess();

    return true;
}

typedef bool (*DoSetPropFallbackFn)(JSContext*, BaselineFrame*, ICSetProp_Fallback*,
                                    HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoSetPropFallbackInfo =
    FunctionInfo<DoSetPropFallbackFn>(DoSetPropFallback, TailCall, PopValues(2));

bool
ICSetProp_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    if (!tailCallVM(DoSetPropFallbackInfo, masm))
        return false;

    // What follows is bailout-only code for inlined script getters.
    // The return address pointed to by the baseline stack points here.
    returnOffset_ = masm.currentOffset();

    // Even though the fallback frame doesn't enter a stub frame, the CallScripted
    // frame that we are emulating does. Again, we lie.
#ifdef DEBUG
    entersStubFrame_ = true;
#endif

    leaveStubFrame(masm, true);

    // Retrieve the stashed initial argument from the caller's frame before returning
    EmitUnstowICValues(masm, 1);
    EmitReturnFromIC(masm);

    return true;
}

bool
ICSetProp_Fallback::Compiler::postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code)
{
    CodeOffsetLabel offset(returnOffset_);
    offset.fixup(&masm);
    cx->compartment()->jitCompartment()->initBaselineSetPropReturnAddr(code->raw() + offset.offset());
    return true;
}

bool
ICSetProp_Native::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratch = regs.takeAny();

    // Unbox and shape guard.
    Register objReg = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_Native::offsetOfShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failure);

    // Guard that the object group matches.
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_Native::offsetOfGroup()), scratch);
    masm.branchPtr(Assembler::NotEqual, Address(objReg, JSObject::offsetOfGroup()), scratch,
                   &failure);

    // Stow both R0 and R1 (object and value).
    EmitStowICValues(masm, 2);

    // Type update stub expects the value to check in R0.
    masm.moveValue(R1, R0);

    // Call the type-update stub.
    if (!callTypeUpdateIC(masm, sizeof(Value)))
        return false;

    // Unstow R0 and R1 (object and key)
    EmitUnstowICValues(masm, 2);

    regs.add(R0);
    regs.takeUnchecked(objReg);

    Register holderReg;
    if (isFixedSlot_) {
        holderReg = objReg;
    } else {
        holderReg = regs.takeAny();
        masm.loadPtr(Address(objReg, NativeObject::offsetOfSlots()), holderReg);
    }

    // Perform the store.
    masm.load32(Address(BaselineStubReg, ICSetProp_Native::offsetOfOffset()), scratch);
    EmitPreBarrier(masm, BaseIndex(holderReg, scratch, TimesOne), MIRType_Value);
    masm.storeValue(R1, BaseIndex(holderReg, scratch, TimesOne));
    if (holderReg != objReg)
        regs.add(holderReg);
    if (cx->runtime()->gc.nursery.exists()) {
        Register scr = regs.takeAny();
        GeneralRegisterSet saveRegs;
        saveRegs.add(R1);
        emitPostWriteBarrierSlot(masm, objReg, R1, scr, saveRegs);
        regs.add(scr);
    }

    // The RHS has to be in R0.
    masm.moveValue(R1, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

ICUpdatedStub*
ICSetPropNativeAddCompiler::getStub(ICStubSpace* space)
{
    AutoShapeVector shapes(cx);
    if (!shapes.append(oldShape_))
        return nullptr;

    if (!GetProtoShapes(obj_, protoChainDepth_, &shapes))
        return nullptr;

    JS_STATIC_ASSERT(ICSetProp_NativeAdd::MAX_PROTO_CHAIN_DEPTH == 4);

    ICUpdatedStub* stub = nullptr;
    switch(protoChainDepth_) {
      case 0: stub = getStubSpecific<0>(space, &shapes); break;
      case 1: stub = getStubSpecific<1>(space, &shapes); break;
      case 2: stub = getStubSpecific<2>(space, &shapes); break;
      case 3: stub = getStubSpecific<3>(space, &shapes); break;
      case 4: stub = getStubSpecific<4>(space, &shapes); break;
      default: MOZ_CRASH("ProtoChainDepth too high.");
    }
    if (!stub || !stub->initUpdatingChain(cx, space))
        return nullptr;
    return stub;
}

bool
ICSetPropNativeAddCompiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    Label failureUnstow;

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratch = regs.takeAny();

    // Unbox and guard against old shape.
    Register objReg = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_NativeAddImpl<0>::offsetOfShape(0)), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failure);

    // Guard that the object group matches.
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_NativeAdd::offsetOfGroup()), scratch);
    masm.branchPtr(Assembler::NotEqual, Address(objReg, JSObject::offsetOfGroup()), scratch,
                   &failure);

    // Stow both R0 and R1 (object and value).
    EmitStowICValues(masm, 2);

    regs = availableGeneralRegs(1);
    scratch = regs.takeAny();
    Register protoReg = regs.takeAny();
    // Check the proto chain.
    for (size_t i = 0; i < protoChainDepth_; i++) {
        masm.loadObjProto(i == 0 ? objReg : protoReg, protoReg);
        masm.branchTestPtr(Assembler::Zero, protoReg, protoReg, &failureUnstow);
        masm.loadPtr(Address(BaselineStubReg, ICSetProp_NativeAddImpl<0>::offsetOfShape(i + 1)),
                     scratch);
        masm.branchTestObjShape(Assembler::NotEqual, protoReg, scratch, &failureUnstow);
    }

    // Shape and type checks succeeded, ok to proceed.

    // Load RHS into R0 for TypeUpdate check.
    // Stack is currently: [..., ObjValue, RHSValue, MaybeReturnAddr? ]
    masm.loadValue(Address(BaselineStackReg, ICStackValueOffset), R0);

    // Call the type-update stub.
    if (!callTypeUpdateIC(masm, sizeof(Value)))
        return false;

    // Unstow R0 and R1 (object and key)
    EmitUnstowICValues(masm, 2);
    regs = availableGeneralRegs(2);
    scratch = regs.takeAny();

    // Changing object shape.  Write the object's new shape.
    Address shapeAddr(objReg, JSObject::offsetOfShape());
    EmitPreBarrier(masm, shapeAddr, MIRType_Shape);
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_NativeAdd::offsetOfNewShape()), scratch);
    masm.storePtr(scratch, shapeAddr);

    // Try to change the object's group.
    Label noGroupChange;

    // Check if the cache has a new group to change to.
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_NativeAdd::offsetOfNewGroup()), scratch);
    masm.branchTestPtr(Assembler::Zero, scratch, scratch, &noGroupChange);

    // Check if the old group still has a newScript.
    masm.loadPtr(Address(objReg, JSObject::offsetOfGroup()), scratch);
    masm.branchPtr(Assembler::Equal,
                   Address(scratch, ObjectGroup::offsetOfAddendum()),
                   ImmWord(0),
                   &noGroupChange);

    // Reload the new group from the cache.
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_NativeAdd::offsetOfNewGroup()), scratch);

    // Change the object's group.
    Address groupAddr(objReg, JSObject::offsetOfGroup());
    EmitPreBarrier(masm, groupAddr, MIRType_ObjectGroup);
    masm.storePtr(scratch, groupAddr);

    masm.bind(&noGroupChange);

    Register holderReg;
    regs.add(R0);
    regs.takeUnchecked(objReg);
    if (isFixedSlot_) {
        holderReg = objReg;
    } else {
        holderReg = regs.takeAny();
        masm.loadPtr(Address(objReg, NativeObject::offsetOfSlots()), holderReg);
    }

    // Perform the store.  No write barrier required since this is a new
    // initialization.
    masm.load32(Address(BaselineStubReg, ICSetProp_NativeAdd::offsetOfOffset()), scratch);
    masm.storeValue(R1, BaseIndex(holderReg, scratch, TimesOne));

    if (holderReg != objReg)
        regs.add(holderReg);

    if (cx->runtime()->gc.nursery.exists()) {
        Register scr = regs.takeAny();
        GeneralRegisterSet saveRegs;
        saveRegs.add(R1);
        emitPostWriteBarrierSlot(masm, objReg, R1, scr, saveRegs);
    }

    // The RHS has to be in R0.
    masm.moveValue(R1, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failureUnstow);
    EmitUnstowICValues(masm, 2);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICSetProp_Unboxed::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratch = regs.takeAny();

    // Unbox and group guard.
    Register object = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_Unboxed::offsetOfGroup()), scratch);
    masm.branchPtr(Assembler::NotEqual, Address(object, JSObject::offsetOfGroup()), scratch,
                   &failure);

    if (needsUpdateStubs()) {
        // Stow both R0 and R1 (object and value).
        masm.push(object);
        masm.push(BaselineStubReg);
        EmitStowICValues(masm, 2);

        // Move RHS into R0 for TypeUpdate check.
        masm.moveValue(R1, R0);

        // Call the type update stub.
        if (!callTypeUpdateIC(masm, sizeof(Value)))
            return false;

        // Unstow R0 and R1 (object and key)
        EmitUnstowICValues(masm, 2);
        masm.pop(BaselineStubReg);
        masm.pop(object);

        // Trigger post barriers here on the values being written. Fields which
        // objects can be written to also need update stubs.
        GeneralRegisterSet saveRegs;
        saveRegs.add(R0);
        saveRegs.add(R1);
        saveRegs.addUnchecked(object);
        saveRegs.add(BaselineStubReg);
        emitPostWriteBarrierSlot(masm, object, R1, scratch, saveRegs);
    }

    // Compute the address being written to.
    masm.load32(Address(BaselineStubReg, ICSetProp_Unboxed::offsetOfFieldOffset()), scratch);
    BaseIndex address(object, scratch, TimesOne);

    if (fieldType_ == JSVAL_TYPE_OBJECT)
        EmitPreBarrier(masm, address, MIRType_Object);
    else if (fieldType_ == JSVAL_TYPE_STRING)
        EmitPreBarrier(masm, address, MIRType_String);
    else
        MOZ_ASSERT(!UnboxedTypeNeedsPreBarrier(fieldType_));

    masm.storeUnboxedProperty(address, fieldType_,
                              ConstantOrRegister(TypedOrValueRegister(R1)), &failure);

    // The RHS has to be in R0.
    masm.moveValue(R1, R0);

    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICSetProp_TypedObject::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    CheckForNeuteredTypedObject(cx, masm, &failure);

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    GeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratch = regs.takeAny();

    // Unbox and shape guard.
    Register object = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_TypedObject::offsetOfShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, object, scratch, &failure);

    // Guard that the object group matches.
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_TypedObject::offsetOfGroup()), scratch);
    masm.branchPtr(Assembler::NotEqual, Address(object, JSObject::offsetOfGroup()), scratch,
                   &failure);

    if (needsUpdateStubs()) {
        // Stow both R0 and R1 (object and value).
        masm.push(object);
        masm.push(BaselineStubReg);
        EmitStowICValues(masm, 2);

        // Move RHS into R0 for TypeUpdate check.
        masm.moveValue(R1, R0);

        // Call the type update stub.
        if (!callTypeUpdateIC(masm, sizeof(Value)))
            return false;

        // Unstow R0 and R1 (object and key)
        EmitUnstowICValues(masm, 2);
        masm.pop(BaselineStubReg);
        masm.pop(object);

        // Trigger post barriers here on the values being written. Descriptors
        // which can write objects also need update stubs.
        GeneralRegisterSet saveRegs;
        saveRegs.add(R0);
        saveRegs.add(R1);
        saveRegs.addUnchecked(object);
        saveRegs.add(BaselineStubReg);
        emitPostWriteBarrierSlot(masm, object, R1, scratch, saveRegs);
    }

    // Save the rhs on the stack so we can get a second scratch register.
    Label failurePopRHS;
    masm.pushValue(R1);
    regs = availableGeneralRegs(1);
    regs.takeUnchecked(object);
    regs.take(scratch);
    Register secondScratch = regs.takeAny();

    // Get the object's data pointer.
    LoadTypedThingData(masm, layout_, object, scratch);

    // Compute the address being written to.
    masm.load32(Address(BaselineStubReg, ICSetProp_TypedObject::offsetOfFieldOffset()), secondScratch);
    masm.addPtr(secondScratch, scratch);

    Address dest(scratch, 0);
    Address value(BaselineStackReg, 0);

    if (fieldDescr_->is<ScalarTypeDescr>()) {
        Scalar::Type type = fieldDescr_->as<ScalarTypeDescr>().type();
        StoreToTypedArray(cx, masm, type, value, dest,
                          secondScratch, &failurePopRHS, &failurePopRHS);
        masm.popValue(R1);
        EmitReturnFromIC(masm);
    } else {
        ReferenceTypeDescr::Type type = fieldDescr_->as<ReferenceTypeDescr>().type();

        masm.popValue(R1);

        switch (type) {
          case ReferenceTypeDescr::TYPE_ANY:
            EmitPreBarrier(masm, dest, MIRType_Value);
            masm.storeValue(R1, dest);
            break;

          case ReferenceTypeDescr::TYPE_OBJECT: {
            EmitPreBarrier(masm, dest, MIRType_Object);
            Label notObject;
            masm.branchTestObject(Assembler::NotEqual, R1, &notObject);
            Register rhsObject = masm.extractObject(R1, ExtractTemp0);
            masm.storePtr(rhsObject, dest);
            EmitReturnFromIC(masm);
            masm.bind(&notObject);
            masm.branchTestNull(Assembler::NotEqual, R1, &failure);
            masm.storePtr(ImmWord(0), dest);
            break;
          }

          case ReferenceTypeDescr::TYPE_STRING: {
            EmitPreBarrier(masm, dest, MIRType_String);
            masm.branchTestString(Assembler::NotEqual, R1, &failure);
            Register rhsString = masm.extractString(R1, ExtractTemp0);
            masm.storePtr(rhsString, dest);
            break;
          }

          default:
            MOZ_CRASH();
        }

        EmitReturnFromIC(masm);
    }

    masm.bind(&failurePopRHS);
    masm.popValue(R1);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICSetProp_CallScripted::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    Label failureUnstow;
    Label failureLeaveStubFrame;

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Stow R0 and R1 to free up registers.
    EmitStowICValues(masm, 2);

    GeneralRegisterSet regs(availableGeneralRegs(1));
    Register scratch = regs.takeAnyExcluding(BaselineTailCallReg);

    // Unbox and shape guard.
    Register objReg = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_CallScripted::offsetOfShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failureUnstow);

    Register holderReg = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_CallScripted::offsetOfHolder()), holderReg);
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_CallScripted::offsetOfHolderShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, holderReg, scratch, &failureUnstow);
    regs.add(holderReg);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, scratch);

    // Load callee function and code.  To ensure that |code| doesn't end up being
    // ArgumentsRectifierReg, if it's available we assign it to |callee| instead.
    Register callee;
    if (regs.has(ArgumentsRectifierReg)) {
        callee = ArgumentsRectifierReg;
        regs.take(callee);
    } else {
        callee = regs.takeAny();
    }
    Register code = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_CallScripted::offsetOfSetter()), callee);
    masm.branchIfFunctionHasNoScript(callee, &failureLeaveStubFrame);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), code);
    masm.loadBaselineOrIonRaw(code, code, &failureLeaveStubFrame);

    // Align the stack such that the JitFrameLayout is aligned on
    // JitStackAlignment.
    masm.alignJitStackBasedOnNArgs(1);

    // Setter is called with the new value as the only argument, and |obj| as thisv.
    // Note that we use Push, not push, so that callJit will align the stack
    // properly on ARM.

    // To Push R1, read it off of the stowed values on stack.
    // Stack: [ ..., R0, R1, ..STUBFRAME-HEADER.., padding? ]
    masm.PushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE));
    masm.Push(R0);
    EmitCreateStubFrameDescriptor(masm, scratch);
    masm.Push(Imm32(1));  // ActualArgc is 1
    masm.Push(callee);
    masm.Push(scratch);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), scratch);
    masm.branch32(Assembler::BelowOrEqual, scratch, Imm32(1), &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != code);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), code);
        masm.loadPtr(Address(code, JitCode::offsetOfCode()), code);
        masm.mov(ImmWord(1), ArgumentsRectifierReg);
    }

    masm.bind(&noUnderflow);
    masm.callJit(code);

    leaveStubFrame(masm, true);
    // Do not care about return value from function. The original RHS should be returned
    // as the result of this operation.
    EmitUnstowICValues(masm, 2);
    masm.moveValue(R1, R0);
    EmitReturnFromIC(masm);

    // Leave stub frame and go to next stub.
    masm.bind(&failureLeaveStubFrame);
    leaveStubFrame(masm, false);

    // Unstow R0 and R1
    masm.bind(&failureUnstow);
    EmitUnstowICValues(masm, 2);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

static bool
DoCallNativeSetter(JSContext* cx, HandleFunction callee, HandleObject obj, HandleValue val)
{
    MOZ_ASSERT(callee->isNative());
    JSNative natfun = callee->native();

    JS::AutoValueArray<3> vp(cx);
    vp[0].setObject(*callee.get());
    vp[1].setObject(*obj.get());
    vp[2].set(val);

    return natfun(cx, 1, vp.begin());
}

typedef bool (*DoCallNativeSetterFn)(JSContext*, HandleFunction, HandleObject, HandleValue);
static const VMFunction DoCallNativeSetterInfo =
    FunctionInfo<DoCallNativeSetterFn>(DoCallNativeSetter);

bool
ICSetProp_CallNative::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    Label failureUnstow;

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Stow R0 and R1 to free up registers.
    EmitStowICValues(masm, 2);

    GeneralRegisterSet regs(availableGeneralRegs(1));
    Register scratch = regs.takeAnyExcluding(BaselineTailCallReg);

    // Unbox and shape guard.
    Register objReg = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_CallNative::offsetOfShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failureUnstow);

    Register holderReg = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_CallNative::offsetOfHolder()), holderReg);
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_CallNative::offsetOfHolderShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, holderReg, scratch, &failureUnstow);
    regs.add(holderReg);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, scratch);

    // Load callee function and code.  To ensure that |code| doesn't end up being
    // ArgumentsRectifierReg, if it's available we assign it to |callee| instead.
    Register callee = regs.takeAny();
    masm.loadPtr(Address(BaselineStubReg, ICSetProp_CallNative::offsetOfSetter()), callee);

    // To Push R1, read it off of the stowed values on stack.
    // Stack: [ ..., R0, R1, ..STUBFRAME-HEADER.. ]
    masm.movePtr(BaselineStackReg, scratch);
    masm.pushValue(Address(scratch, STUB_FRAME_SIZE));
    masm.push(objReg);
    masm.push(callee);

    // Don't need to preserve R0 anymore.
    regs.add(R0);

    if (!callVM(DoCallNativeSetterInfo, masm))
        return false;
    leaveStubFrame(masm);

    // Do not care about return value from function. The original RHS should be returned
    // as the result of this operation.
    EmitUnstowICValues(masm, 2);
    masm.moveValue(R1, R0);
    EmitReturnFromIC(masm);

    // Unstow R0 and R1
    masm.bind(&failureUnstow);
    EmitUnstowICValues(masm, 2);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Call_Fallback
//

static bool
TryAttachFunApplyStub(JSContext* cx, ICCall_Fallback* stub, HandleScript script, jsbytecode* pc,
                      HandleValue thisv, uint32_t argc, Value* argv, bool* attached)
{
    if (argc != 2)
        return true;

    if (!thisv.isObject() || !thisv.toObject().is<JSFunction>())
        return true;
    RootedFunction target(cx, &thisv.toObject().as<JSFunction>());

    bool isScripted = target->hasJITCode();

    // right now, only handle situation where second argument is |arguments|
    if (argv[1].isMagic(JS_OPTIMIZED_ARGUMENTS) && !script->needsArgsObj()) {
        if (isScripted && !stub->hasStub(ICStub::Call_ScriptedApplyArguments)) {
            JitSpew(JitSpew_BaselineIC, "  Generating Call_ScriptedApplyArguments stub");

            ICCall_ScriptedApplyArguments::Compiler compiler(
                cx, stub->fallbackMonitorStub()->firstMonitorStub(), script->pcToOffset(pc));
            ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
            if (!newStub)
                return false;

            stub->addNewStub(newStub);
            *attached = true;
            return true;
        }

        // TODO: handle FUNAPPLY for native targets.
    }

    if (argv[1].isObject() && argv[1].toObject().is<ArrayObject>()) {
        if (isScripted && !stub->hasStub(ICStub::Call_ScriptedApplyArray)) {
            JitSpew(JitSpew_BaselineIC, "  Generating Call_ScriptedApplyArray stub");

            ICCall_ScriptedApplyArray::Compiler compiler(
                cx, stub->fallbackMonitorStub()->firstMonitorStub(), script->pcToOffset(pc));
            ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
            if (!newStub)
                return false;

            stub->addNewStub(newStub);
            *attached = true;
            return true;
        }
    }
    return true;
}

static bool
TryAttachFunCallStub(JSContext* cx, ICCall_Fallback* stub, HandleScript script, jsbytecode* pc,
                     HandleValue thisv, bool* attached)
{
    // Try to attach a stub for Function.prototype.call with scripted |this|.

    *attached = false;
    if (!thisv.isObject() || !thisv.toObject().is<JSFunction>())
        return true;
    RootedFunction target(cx, &thisv.toObject().as<JSFunction>());

    // Attach a stub if the script can be Baseline-compiled. We do this also
    // if the script is not yet compiled to avoid attaching a CallNative stub
    // that handles everything, even after the callee becomes hot.
    if (target->hasScript() && target->nonLazyScript()->canBaselineCompile() &&
        !stub->hasStub(ICStub::Call_ScriptedFunCall))
    {
        JitSpew(JitSpew_BaselineIC, "  Generating Call_ScriptedFunCall stub");

        ICCall_ScriptedFunCall::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                                  script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        *attached = true;
        stub->addNewStub(newStub);
        return true;
    }

    return true;
}

static bool
GetTemplateObjectForNative(JSContext* cx, HandleScript script, jsbytecode* pc,
                           Native native, const CallArgs& args, MutableHandleObject res)
{
    // Check for natives to which template objects can be attached. This is
    // done to provide templates to Ion for inlining these natives later on.

    if (native == js_Array) {
        // Note: the template array won't be used if its length is inaccurately
        // computed here.  (We allocate here because compilation may occur on a
        // separate thread where allocation is impossible.)
        size_t count = 0;
        if (args.length() != 1)
            count = args.length();
        else if (args.length() == 1 && args[0].isInt32() && args[0].toInt32() >= 0)
            count = args[0].toInt32();
        res.set(NewDenseUnallocatedArray(cx, count, NullPtr(), TenuredObject));
        if (!res)
            return false;

        ObjectGroup* group = ObjectGroup::allocationSiteGroup(cx, script, pc, JSProto_Array);
        if (!group)
            return false;
        res->setGroup(group);
        return true;
    }

    if (native == intrinsic_NewDenseArray) {
        res.set(NewDenseUnallocatedArray(cx, 0, NullPtr(), TenuredObject));
        if (!res)
            return false;

        ObjectGroup* group = ObjectGroup::allocationSiteGroup(cx, script, pc, JSProto_Array);
        if (!group)
            return false;
        res->setGroup(group);
        return true;
    }

    if (native == js::array_concat) {
        if (args.thisv().isObject() && args.thisv().toObject().is<ArrayObject>() &&
            !args.thisv().toObject().isSingleton())
        {
            RootedObject proto(cx, args.thisv().toObject().getProto());
            res.set(NewDenseEmptyArray(cx, proto, TenuredObject));
            if (!res)
                return false;
            res->setGroup(args.thisv().toObject().group());
            return true;
        }
    }

    if (native == js::str_split && args.length() == 1 && args[0].isString()) {
        res.set(NewDenseUnallocatedArray(cx, 0, NullPtr(), TenuredObject));
        if (!res)
            return false;

        ObjectGroup* group = ObjectGroup::allocationSiteGroup(cx, script, pc, JSProto_Array);
        if (!group)
            return false;
        res->setGroup(group);
        return true;
    }

    if (native == js_String) {
        RootedString emptyString(cx, cx->runtime()->emptyString);
        res.set(StringObject::create(cx, emptyString, TenuredObject));
        return !!res;
    }

    if (native == obj_create && args.length() == 1 && args[0].isObjectOrNull()) {
        RootedObject proto(cx, args[0].toObjectOrNull());
        res.set(ObjectCreateImpl(cx, proto, TenuredObject));
        return !!res;
    }

    if (JitSupportsSimd()) {
#define ADD_INT32X4_SIMD_OP_NAME_(OP) || native == js::simd_int32x4_##OP
       if (false
           ARITH_COMMONX4_SIMD_OP(ADD_INT32X4_SIMD_OP_NAME_)
           BITWISE_COMMONX4_SIMD_OP(ADD_INT32X4_SIMD_OP_NAME_))
       {
            Rooted<SimdTypeDescr*> descr(cx, &cx->global()->int32x4TypeDescr().as<SimdTypeDescr>());
            res.set(cx->compartment()->jitCompartment()->getSimdTemplateObjectFor(cx, descr));
            return !!res;
       }
#undef ADD_INT32X4_SIMD_OP_NAME_
    }

    return true;
}

static bool
GetTemplateObjectForClassHook(JSContext* cx, JSNative hook, CallArgs& args,
                              MutableHandleObject templateObject)
{
    if (hook == TypedObject::construct) {
        Rooted<TypeDescr*> descr(cx, &args.callee().as<TypeDescr>());
        templateObject.set(TypedObject::createZeroed(cx, descr, 1, gc::TenuredHeap));
        return !!templateObject;
    }

    if (hook == SimdTypeDescr::call && JitSupportsSimd()) {
        Rooted<SimdTypeDescr*> descr(cx, &args.callee().as<SimdTypeDescr>());
        templateObject.set(cx->compartment()->jitCompartment()->getSimdTemplateObjectFor(cx, descr));
        return !!templateObject;
    }

    return true;
}

static bool
IsOptimizableCallStringSplit(Value callee, Value thisv, int argc, Value* args)
{
    if (argc != 1 || !thisv.isString() || !args[0].isString())
        return false;

    if (!thisv.toString()->isAtom() || !args[0].toString()->isAtom())
        return false;

    if (!callee.isObject() || !callee.toObject().is<JSFunction>())
        return false;

    JSFunction& calleeFun = callee.toObject().as<JSFunction>();
    if (!calleeFun.isNative() || calleeFun.native() != js::str_split)
        return false;

    return true;
}

static bool
TryAttachCallStub(JSContext* cx, ICCall_Fallback* stub, HandleScript script, jsbytecode* pc,
                  JSOp op, uint32_t argc, Value* vp, bool constructing, bool isSpread,
                  bool createSingleton, bool* handled)
{
    if (createSingleton || op == JSOP_EVAL || op == JSOP_STRICTEVAL)
        return true;

    if (stub->numOptimizedStubs() >= ICCall_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with inert megamorphic stub.
        // But for now we just bail.
        return true;
    }

    RootedValue callee(cx, vp[0]);
    RootedValue thisv(cx, vp[1]);

    // Don't attach an optimized call stub if we could potentially attach an
    // optimized StringSplit stub.
    if (stub->numOptimizedStubs() == 0 && IsOptimizableCallStringSplit(callee, thisv, argc, vp + 2))
        return true;

    MOZ_ASSERT_IF(stub->hasStub(ICStub::Call_StringSplit), stub->numOptimizedStubs() == 1);

    stub->unlinkStubsWithKind(cx, ICStub::Call_StringSplit);

    if (!callee.isObject())
        return true;

    RootedObject obj(cx, &callee.toObject());
    if (!obj->is<JSFunction>()) {
        // Try to attach a stub for a call/construct hook on the object.
        // Ignore proxies, which are special cased by callHook/constructHook.
        if (obj->is<ProxyObject>())
            return true;
        if (JSNative hook = constructing ? obj->constructHook() : obj->callHook()) {
            if (op != JSOP_FUNAPPLY && !isSpread && !createSingleton) {
                RootedObject templateObject(cx);
                CallArgs args = CallArgsFromVp(argc, vp);
                if (!GetTemplateObjectForClassHook(cx, hook, args, &templateObject))
                    return false;

                JitSpew(JitSpew_BaselineIC, "  Generating Call_ClassHook stub");
                ICCall_ClassHook::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                                    obj->getClass(), hook, templateObject,
                                                    script->pcToOffset(pc), constructing);
                ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
                if (!newStub)
                    return false;

                stub->addNewStub(newStub);
                *handled = true;
                return true;
            }
        }
        return true;
    }

    RootedFunction fun(cx, &obj->as<JSFunction>());

    if (fun->hasScript()) {
        // Never attach optimized scripted call stubs for JSOP_FUNAPPLY.
        // MagicArguments may escape the frame through them.
        if (op == JSOP_FUNAPPLY)
            return true;

        // If callee is not an interpreted constructor, we have to throw.
        if (constructing && !fun->isInterpretedConstructor())
            return true;

        if (!fun->hasJITCode()) {
            // Don't treat this as an unoptimizable case, as we'll add a stub
            // when the callee becomes hot.
            *handled = true;
            return true;
        }

        // Check if this stub chain has already generalized scripted calls.
        if (stub->scriptedStubsAreGeneralized()) {
            JitSpew(JitSpew_BaselineIC, "  Chain already has generalized scripted call stub!");
            return true;
        }

        if (stub->scriptedStubCount() >= ICCall_Fallback::MAX_SCRIPTED_STUBS) {
            // Create a Call_AnyScripted stub.
            JitSpew(JitSpew_BaselineIC, "  Generating Call_AnyScripted stub (cons=%s, spread=%s)",
                    constructing ? "yes" : "no", isSpread ? "yes" : "no");
            ICCallScriptedCompiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                            constructing, isSpread, script->pcToOffset(pc));
            ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
            if (!newStub)
                return false;

            // Before adding new stub, unlink all previous Call_Scripted.
            stub->unlinkStubsWithKind(cx, ICStub::Call_Scripted);

            // Add new generalized stub.
            stub->addNewStub(newStub);
            *handled = true;
            return true;
        }

        // Keep track of the function's |prototype| property in type
        // information, for use during Ion compilation.
        if (IsIonEnabled(cx))
            EnsureTrackPropertyTypes(cx, fun, NameToId(cx->names().prototype));

        // Remember the template object associated with any script being called
        // as a constructor, for later use during Ion compilation.
        RootedObject templateObject(cx);
        if (constructing) {
            // If we are calling a constructor for which the new script
            // properties analysis has not been performed yet, don't attach a
            // stub. After the analysis is performed, CreateThisForFunction may
            // start returning objects with a different type, and the Ion
            // compiler will get confused.

            // Only attach a stub if the function already has a prototype and
            // we can look it up without causing side effects.
            RootedValue protov(cx);
            if (!GetPropertyPure(cx, fun, NameToId(cx->names().prototype), protov.address())) {
                JitSpew(JitSpew_BaselineIC, "  Can't purely lookup function prototype");
                return true;
            }

            if (protov.isObject()) {
                TaggedProto proto(&protov.toObject());
                ObjectGroup* group = ObjectGroup::defaultNewGroup(cx, nullptr, proto, fun);
                if (!group)
                    return false;

                if (group->newScript() && !group->newScript()->analyzed()) {
                    JitSpew(JitSpew_BaselineIC, "  Function newScript has not been analyzed");

                    // This is temporary until the analysis is perfomed, so
                    // don't treat this as unoptimizable.
                    *handled = true;
                    return true;
                }
            }

            JSObject* thisObject = CreateThisForFunction(cx, fun, MaybeSingletonObject);
            if (!thisObject)
                return false;

            if (thisObject->is<PlainObject>() || thisObject->is<UnboxedPlainObject>())
                templateObject = thisObject;
        }

        JitSpew(JitSpew_BaselineIC,
                "  Generating Call_Scripted stub (fun=%p, %s:%d, cons=%s, spread=%s)",
                fun.get(), fun->nonLazyScript()->filename(), fun->nonLazyScript()->lineno(),
                constructing ? "yes" : "no", isSpread ? "yes" : "no");
        ICCallScriptedCompiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                        fun, templateObject,
                                        constructing, isSpread, script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *handled = true;
        return true;
    }

    if (fun->isNative() && (!constructing || (constructing && fun->isNativeConstructor()))) {
        // Generalized native call stubs are not here yet!
        MOZ_ASSERT(!stub->nativeStubsAreGeneralized());

        // Check for JSOP_FUNAPPLY
        if (op == JSOP_FUNAPPLY) {
            if (fun->native() == js_fun_apply)
                return TryAttachFunApplyStub(cx, stub, script, pc, thisv, argc, vp + 2, handled);

            // Don't try to attach a "regular" optimized call stubs for FUNAPPLY ops,
            // since MagicArguments may escape through them.
            return true;
        }

        if (op == JSOP_FUNCALL && fun->native() == js_fun_call) {
            if (!TryAttachFunCallStub(cx, stub, script, pc, thisv, handled))
                return false;
            if (*handled)
                return true;
        }

        if (stub->nativeStubCount() >= ICCall_Fallback::MAX_NATIVE_STUBS) {
            JitSpew(JitSpew_BaselineIC,
                    "  Too many Call_Native stubs. TODO: add Call_AnyNative!");
            return true;
        }

        if (fun->native() == intrinsic_IsSuspendedStarGenerator) {
            // This intrinsic only appears in self-hosted code.
            MOZ_ASSERT(argc == 1);
            JitSpew(JitSpew_BaselineIC, "  Generating Call_IsSuspendedStarGenerator stub");

            ICCall_IsSuspendedStarGenerator::Compiler compiler(cx);
            ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
            if (!newStub)
                return false;

            stub->addNewStub(newStub);
            *handled = true;
            return true;
        }

        RootedObject templateObject(cx);
        if (MOZ_LIKELY(!isSpread)) {
            CallArgs args = CallArgsFromVp(argc, vp);
            if (!GetTemplateObjectForNative(cx, script, pc, fun->native(), args, &templateObject))
                return false;
        }

        JitSpew(JitSpew_BaselineIC, "  Generating Call_Native stub (fun=%p, cons=%s, spread=%s)",
                fun.get(), constructing ? "yes" : "no", isSpread ? "yes" : "no");
        ICCall_Native::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                         fun, templateObject, constructing, isSpread,
                                         script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *handled = true;
        return true;
    }

    return true;
}

static bool
CopyArray(JSContext* cx, HandleArrayObject obj, MutableHandleValue result)
{
    MOZ_ASSERT(obj->is<ArrayObject>());
    uint32_t length = obj->as<ArrayObject>().length();
    MOZ_ASSERT(obj->getDenseInitializedLength() == length);

    RootedObjectGroup group(cx, obj->getGroup(cx));
    if (!group)
        return false;

    RootedArrayObject newObj(cx, NewDenseFullyAllocatedArray(cx, length, NullPtr(), TenuredObject));
    if (!newObj)
        return false;

    newObj->setGroup(group);
    newObj->setDenseInitializedLength(length);
    newObj->initDenseElements(0, obj->getDenseElements(), length);
    result.setObject(*newObj);
    return true;
}

static bool
TryAttachStringSplit(JSContext* cx, ICCall_Fallback* stub, HandleScript script,
                     uint32_t argc, Value* vp, jsbytecode* pc, HandleValue res,
                     bool* attached)
{
    if (stub->numOptimizedStubs() != 0)
        return true;

    RootedValue callee(cx, vp[0]);
    RootedValue thisv(cx, vp[1]);
    Value* args = vp + 2;

    if (!IsOptimizableCallStringSplit(callee, thisv, argc, args))
        return true;

    MOZ_ASSERT(callee.isObject());
    MOZ_ASSERT(callee.toObject().is<JSFunction>());

    RootedString thisString(cx, thisv.toString());
    RootedString argString(cx, args[0].toString());
    RootedArrayObject obj(cx, &res.toObject().as<ArrayObject>());
    RootedValue arr(cx);

    // Copy the array before storing in stub.
    if (!CopyArray(cx, obj, &arr))
        return false;

    // Atomize all elements of the array.
    RootedArrayObject arrObj(cx, &arr.toObject().as<ArrayObject>());
    uint32_t initLength = arrObj->length();
    for (uint32_t i = 0; i < initLength; i++) {
        JSAtom* str = js::AtomizeString(cx, arrObj->getDenseElement(i).toString());
        if (!str)
            return false;

        arrObj->setDenseElement(i, StringValue(str));
    }

    ICCall_StringSplit::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                          script->pcToOffset(pc), thisString, argString,
                                          arr);
    ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;

    stub->addNewStub(newStub);
    *attached = true;
    return true;
}

static bool
DoCallFallback(JSContext* cx, BaselineFrame* frame, ICCall_Fallback* stub_, uint32_t argc,
               Value* vp, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICCall_Fallback*> stub(frame, stub_);

    // Ensure vp array is rooted - we may GC in here.
    AutoArrayRooter vpRoot(cx, argc + 2, vp);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "Call(%s)", js_CodeName[op]);

    MOZ_ASSERT(argc == GET_ARGC(pc));

    RootedValue callee(cx, vp[0]);
    RootedValue thisv(cx, vp[1]);

    Value* args = vp + 2;

    // Handle funapply with JSOP_ARGUMENTS
    if (op == JSOP_FUNAPPLY && argc == 2 && args[1].isMagic(JS_OPTIMIZED_ARGUMENTS)) {
        CallArgs callArgs = CallArgsFromVp(argc, vp);
        if (!GuardFunApplyArgumentsOptimization(cx, frame, callArgs))
            return false;
    }

    // Compute construcing and useNewGroup flags.
    bool constructing = (op == JSOP_NEW);
    bool createSingleton = ObjectGroup::useSingletonForNewObject(cx, script, pc);

    // Try attaching a call stub.
    bool handled = false;
    if (!TryAttachCallStub(cx, stub, script, pc, op, argc, vp, constructing, false,
                           createSingleton, &handled))
    {
        return false;
    }

    if (op == JSOP_NEW) {
        if (!InvokeConstructor(cx, callee, argc, args, res))
            return false;
    } else if ((op == JSOP_EVAL || op == JSOP_STRICTEVAL) &&
               frame->scopeChain()->global().valueIsEval(callee))
    {
        if (!DirectEval(cx, CallArgsFromVp(argc, vp)))
            return false;
        res.set(vp[0]);
    } else {
        MOZ_ASSERT(op == JSOP_CALL ||
                   op == JSOP_FUNCALL ||
                   op == JSOP_FUNAPPLY ||
                   op == JSOP_EVAL ||
                   op == JSOP_STRICTEVAL);
        if (!Invoke(cx, thisv, callee, argc, args, res))
            return false;
    }

    TypeScript::Monitor(cx, script, pc, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Attach a new TypeMonitor stub for this value.
    ICTypeMonitor_Fallback* typeMonFbStub = stub->fallbackMonitorStub();
    if (!typeMonFbStub->addMonitorStubForValue(cx, script, res))
        return false;
    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, script, res))
        return false;

    // If 'callee' is a potential Call_StringSplit, try to attach an
    // optimized StringSplit stub.
    if (!TryAttachStringSplit(cx, stub, script, argc, vp, pc, res, &handled))
        return false;

    if (!handled)
        stub->noteUnoptimizableCall();
    return true;
}

static bool
DoSpreadCallFallback(JSContext* cx, BaselineFrame* frame, ICCall_Fallback* stub_, Value* vp,
                     MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICCall_Fallback*> stub(frame, stub_);

    // Ensure vp array is rooted - we may GC in here.
    AutoArrayRooter vpRoot(cx, 3, vp);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "SpreadCall(%s)", js_CodeName[op]);

    RootedValue callee(cx, vp[0]);
    RootedValue thisv(cx, vp[1]);
    RootedValue arr(cx, vp[2]);

    bool constructing = (op == JSOP_SPREADNEW);

    // Try attaching a call stub.
    bool handled = false;
    if (op != JSOP_SPREADEVAL && op != JSOP_STRICTSPREADEVAL &&
        !TryAttachCallStub(cx, stub, script, pc, op, 1, vp, constructing, true, false,
                           &handled))
    {
        return false;
    }

    if (!SpreadCallOperation(cx, script, pc, thisv, callee, arr, res))
        return false;

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Attach a new TypeMonitor stub for this value.
    ICTypeMonitor_Fallback* typeMonFbStub = stub->fallbackMonitorStub();
    if (!typeMonFbStub->addMonitorStubForValue(cx, script, res))
        return false;
    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, script, res))
        return false;

    if (!handled)
        stub->noteUnoptimizableCall();
    return true;
}

void
ICCallStubCompiler::pushCallArguments(MacroAssembler& masm, GeneralRegisterSet regs,
                                      Register argcReg, bool isJitCall)
{
    MOZ_ASSERT(!regs.has(argcReg));

    // Push the callee and |this| too.
    Register count = regs.takeAny();
    masm.mov(argcReg, count);
    masm.add32(Imm32(2), count);

    // argPtr initially points to the last argument.
    Register argPtr = regs.takeAny();
    masm.mov(BaselineStackReg, argPtr);

    // Skip 4 pointers pushed on top of the arguments: the frame descriptor,
    // return address, old frame pointer and stub reg.
    masm.addPtr(Imm32(STUB_FRAME_SIZE), argPtr);

    // Align the stack such that the JitFrameLayout is aligned on the
    // JitStackAlignment.
    if (isJitCall)
        masm.alignJitStackBasedOnNArgs(argcReg);

    // Push all values, starting at the last one.
    Label loop, done;
    masm.bind(&loop);
    masm.branchTest32(Assembler::Zero, count, count, &done);
    {
        masm.pushValue(Address(argPtr, 0));
        masm.addPtr(Imm32(sizeof(Value)), argPtr);

        masm.sub32(Imm32(1), count);
        masm.jump(&loop);
    }
    masm.bind(&done);
}

void
ICCallStubCompiler::guardSpreadCall(MacroAssembler& masm, Register argcReg, Label* failure)
{
    masm.unboxObject(Address(BaselineStackReg, ICStackValueOffset), argcReg);
    masm.loadPtr(Address(argcReg, NativeObject::offsetOfElements()), argcReg);
    masm.load32(Address(argcReg, ObjectElements::offsetOfLength()), argcReg);

    // Limit actual argc to something reasonable (huge number of arguments can
    // blow the stack limit).
    static_assert(ICCall_Scripted::MAX_ARGS_SPREAD_LENGTH <= ARGS_LENGTH_MAX,
                  "maximum arguments length for optimized stub should be <= ARGS_LENGTH_MAX");
    masm.branch32(Assembler::Above, argcReg, Imm32(ICCall_Scripted::MAX_ARGS_SPREAD_LENGTH),
                  failure);
}

void
ICCallStubCompiler::pushSpreadCallArguments(MacroAssembler& masm, GeneralRegisterSet regs,
                                            Register argcReg, bool isJitCall)
{
    // Push arguments
    Register startReg = regs.takeAny();
    Register endReg = regs.takeAny();
    masm.unboxObject(Address(BaselineStackReg, STUB_FRAME_SIZE), startReg);
    masm.loadPtr(Address(startReg, NativeObject::offsetOfElements()), startReg);
    masm.mov(argcReg, endReg);
    static_assert(sizeof(Value) == 8, "Value must be 8 bytes");
    masm.lshiftPtr(Imm32(3), endReg);
    masm.addPtr(startReg, endReg);

    // Align the stack such that the JitFrameLayout is aligned on the
    // JitStackAlignment.
    if (isJitCall)
        masm.alignJitStackBasedOnNArgs(argcReg);

    // Copying pre-decrements endReg by 8 until startReg is reached
    Label copyDone;
    Label copyStart;
    masm.bind(&copyStart);
    masm.branchPtr(Assembler::Equal, endReg, startReg, &copyDone);
    masm.subPtr(Imm32(sizeof(Value)), endReg);
    masm.pushValue(Address(endReg, 0));
    masm.jump(&copyStart);
    masm.bind(&copyDone);

    regs.add(startReg);
    regs.add(endReg);

    // Push the callee and |this|.
    masm.pushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE + 1 * sizeof(Value)));
    masm.pushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE + 2 * sizeof(Value)));
}

Register
ICCallStubCompiler::guardFunApply(MacroAssembler& masm, GeneralRegisterSet regs, Register argcReg,
                                  bool checkNative, FunApplyThing applyThing, Label* failure)
{
    // Ensure argc == 2
    masm.branch32(Assembler::NotEqual, argcReg, Imm32(2), failure);

    // Stack looks like:
    //      [..., CalleeV, ThisV, Arg0V, Arg1V <MaybeReturnReg>]

    Address secondArgSlot(BaselineStackReg, ICStackValueOffset);
    if (applyThing == FunApply_MagicArgs) {
        // Ensure that the second arg is magic arguments.
        masm.branchTestMagic(Assembler::NotEqual, secondArgSlot, failure);

        // Ensure that this frame doesn't have an arguments object.
        masm.branchTest32(Assembler::NonZero,
                          Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFlags()),
                          Imm32(BaselineFrame::HAS_ARGS_OBJ),
                          failure);
    } else {
        MOZ_ASSERT(applyThing == FunApply_Array);

        GeneralRegisterSet regsx = regs;

        // Ensure that the second arg is an array.
        ValueOperand secondArgVal = regsx.takeAnyValue();
        masm.loadValue(secondArgSlot, secondArgVal);

        masm.branchTestObject(Assembler::NotEqual, secondArgVal, failure);
        Register secondArgObj = masm.extractObject(secondArgVal, ExtractTemp1);

        regsx.add(secondArgVal);
        regsx.takeUnchecked(secondArgObj);

        masm.branchTestObjClass(Assembler::NotEqual, secondArgObj, regsx.getAny(),
                                &ArrayObject::class_, failure);

        // Get the array elements and ensure that initializedLength == length
        masm.loadPtr(Address(secondArgObj, NativeObject::offsetOfElements()), secondArgObj);

        Register lenReg = regsx.takeAny();
        masm.load32(Address(secondArgObj, ObjectElements::offsetOfLength()), lenReg);

        masm.branch32(Assembler::NotEqual,
                      Address(secondArgObj, ObjectElements::offsetOfInitializedLength()),
                      lenReg, failure);

        // Limit the length to something reasonable (huge number of arguments can
        // blow the stack limit).
        masm.branch32(Assembler::Above, lenReg,
                      Imm32(ICCall_ScriptedApplyArray::MAX_ARGS_ARRAY_LENGTH),
                      failure);

        // Ensure no holes.  Loop through values in array and make sure none are magic.
        // Start address is secondArgObj, end address is secondArgObj + (lenReg * sizeof(Value))
        JS_STATIC_ASSERT(sizeof(Value) == 8);
        masm.lshiftPtr(Imm32(3), lenReg);
        masm.addPtr(secondArgObj, lenReg);

        Register start = secondArgObj;
        Register end = lenReg;
        Label loop;
        Label endLoop;
        masm.bind(&loop);
        masm.branchPtr(Assembler::AboveOrEqual, start, end, &endLoop);
        masm.branchTestMagic(Assembler::Equal, Address(start, 0), failure);
        masm.addPtr(Imm32(sizeof(Value)), start);
        masm.jump(&loop);
        masm.bind(&endLoop);
    }

    // Stack now confirmed to be like:
    //      [..., CalleeV, ThisV, Arg0V, MagicValue(Arguments), <MaybeReturnAddr>]

    // Load the callee, ensure that it's js_fun_apply
    ValueOperand val = regs.takeAnyValue();
    Address calleeSlot(BaselineStackReg, ICStackValueOffset + (3 * sizeof(Value)));
    masm.loadValue(calleeSlot, val);

    masm.branchTestObject(Assembler::NotEqual, val, failure);
    Register callee = masm.extractObject(val, ExtractTemp1);

    masm.branchTestObjClass(Assembler::NotEqual, callee, regs.getAny(), &JSFunction::class_,
                            failure);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);

    masm.branchPtr(Assembler::NotEqual, callee, ImmPtr(js_fun_apply), failure);

    // Load the |thisv|, ensure that it's a scripted function with a valid baseline or ion
    // script, or a native function.
    Address thisSlot(BaselineStackReg, ICStackValueOffset + (2 * sizeof(Value)));
    masm.loadValue(thisSlot, val);

    masm.branchTestObject(Assembler::NotEqual, val, failure);
    Register target = masm.extractObject(val, ExtractTemp1);
    regs.add(val);
    regs.takeUnchecked(target);

    masm.branchTestObjClass(Assembler::NotEqual, target, regs.getAny(), &JSFunction::class_,
                            failure);

    if (checkNative) {
        masm.branchIfInterpreted(target, failure);
    } else {
        masm.branchIfFunctionHasNoScript(target, failure);
        Register temp = regs.takeAny();
        masm.loadPtr(Address(target, JSFunction::offsetOfNativeOrScript()), temp);
        masm.loadBaselineOrIonRaw(temp, temp, failure);
        regs.add(temp);
    }
    return target;
}

void
ICCallStubCompiler::pushCallerArguments(MacroAssembler& masm, GeneralRegisterSet regs)
{
    // Initialize copyReg to point to start caller arguments vector.
    // Initialize argcReg to poitn to the end of it.
    Register startReg = regs.takeAny();
    Register endReg = regs.takeAny();
    masm.loadPtr(Address(BaselineFrameReg, 0), startReg);
    masm.loadPtr(Address(startReg, BaselineFrame::offsetOfNumActualArgs()), endReg);
    masm.addPtr(Imm32(BaselineFrame::offsetOfArg(0)), startReg);
    masm.alignJitStackBasedOnNArgs(endReg);
    masm.lshiftPtr(Imm32(ValueShift), endReg);
    masm.addPtr(startReg, endReg);

    // Copying pre-decrements endReg by 8 until startReg is reached
    Label copyDone;
    Label copyStart;
    masm.bind(&copyStart);
    masm.branchPtr(Assembler::Equal, endReg, startReg, &copyDone);
    masm.subPtr(Imm32(sizeof(Value)), endReg);
    masm.pushValue(Address(endReg, 0));
    masm.jump(&copyStart);
    masm.bind(&copyDone);
}

void
ICCallStubCompiler::pushArrayArguments(MacroAssembler& masm, Address arrayVal,
                                       GeneralRegisterSet regs)
{
    // Load start and end address of values to copy.
    // guardFunApply has already gauranteed that the array is packed and contains
    // no holes.
    Register startReg = regs.takeAny();
    Register endReg = regs.takeAny();
    masm.extractObject(arrayVal, startReg);
    masm.loadPtr(Address(startReg, NativeObject::offsetOfElements()), startReg);
    masm.load32(Address(startReg, ObjectElements::offsetOfInitializedLength()), endReg);
    masm.alignJitStackBasedOnNArgs(endReg);
    masm.lshiftPtr(Imm32(ValueShift), endReg);
    masm.addPtr(startReg, endReg);

    // Copying pre-decrements endReg by 8 until startReg is reached
    Label copyDone;
    Label copyStart;
    masm.bind(&copyStart);
    masm.branchPtr(Assembler::Equal, endReg, startReg, &copyDone);
    masm.subPtr(Imm32(sizeof(Value)), endReg);
    masm.pushValue(Address(endReg, 0));
    masm.jump(&copyStart);
    masm.bind(&copyDone);
}

typedef bool (*DoCallFallbackFn)(JSContext*, BaselineFrame*, ICCall_Fallback*,
                                 uint32_t, Value*, MutableHandleValue);
static const VMFunction DoCallFallbackInfo = FunctionInfo<DoCallFallbackFn>(DoCallFallback);

typedef bool (*DoSpreadCallFallbackFn)(JSContext*, BaselineFrame*, ICCall_Fallback*,
                                       Value*, MutableHandleValue);
static const VMFunction DoSpreadCallFallbackInfo =
    FunctionInfo<DoSpreadCallFallbackFn>(DoSpreadCallFallback);

bool
ICCall_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, R1.scratchReg());

    // Values are on the stack left-to-right. Calling convention wants them
    // right-to-left so duplicate them on the stack in reverse order.
    // |this| and callee are pushed last.

    GeneralRegisterSet regs(availableGeneralRegs(0));

    if (MOZ_UNLIKELY(isSpread_)) {
        // Use BaselineFrameReg instead of BaselineStackReg, because
        // BaselineFrameReg and BaselineStackReg hold the same value just after
        // calling enterStubFrame.
        masm.pushValue(Address(BaselineFrameReg, 0 * sizeof(Value) + STUB_FRAME_SIZE)); // array
        masm.pushValue(Address(BaselineFrameReg, 1 * sizeof(Value) + STUB_FRAME_SIZE)); // this
        masm.pushValue(Address(BaselineFrameReg, 2 * sizeof(Value) + STUB_FRAME_SIZE)); // callee

        masm.push(BaselineStackReg);
        masm.push(BaselineStubReg);

        masm.loadPtr(Address(BaselineFrameReg, 0), R0.scratchReg());
        masm.pushBaselineFramePtr(R0.scratchReg(), R0.scratchReg());

        if (!callVM(DoSpreadCallFallbackInfo, masm))
            return false;

        leaveStubFrame(masm);
        EmitReturnFromIC(masm);

        // SPREADCALL is not yet supported in Ion, so do not generate asmcode for
        // bailout.
        return true;
    }

    regs.take(R0.scratchReg()); // argc.

    pushCallArguments(masm, regs, R0.scratchReg(), /* isJitCall = */ false);

    masm.push(BaselineStackReg);
    masm.push(R0.scratchReg());
    masm.push(BaselineStubReg);

    // Load previous frame pointer, push BaselineFrame*.
    masm.loadPtr(Address(BaselineFrameReg, 0), R0.scratchReg());
    masm.pushBaselineFramePtr(R0.scratchReg(), R0.scratchReg());

    if (!callVM(DoCallFallbackInfo, masm))
        return false;

    leaveStubFrame(masm);
    EmitReturnFromIC(masm);

    // The following asmcode is only used when an Ion inlined frame bails out
    // into into baseline jitcode. The return address pushed onto the
    // reconstructed baseline stack points here.
    returnOffset_ = masm.currentOffset();

    // Load passed-in ThisV into R1 just in case it's needed.  Need to do this before
    // we leave the stub frame since that info will be lost.
    // Current stack:  [...., ThisV, ActualArgc, CalleeToken, Descriptor ]
    masm.loadValue(Address(BaselineStackReg, 3 * sizeof(size_t)), R1);

    leaveStubFrame(masm, true);

    // R1 and R0 are taken.
    regs = availableGeneralRegs(2);
    Register scratch = regs.takeAny();

    // If this is a |constructing| call, if the callee returns a non-object, we replace it with
    // the |this| object passed in.
    MOZ_ASSERT(JSReturnOperand == R0);
    Label skipThisReplace;
    masm.load16ZeroExtend(Address(BaselineStubReg, ICStub::offsetOfExtra()), scratch);
    masm.branchTest32(Assembler::Zero, scratch, Imm32(ICCall_Fallback::CONSTRUCTING_FLAG),
                      &skipThisReplace);
    masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);
    masm.moveValue(R1, R0);
#ifdef DEBUG
    masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);
    masm.assumeUnreachable("Failed to return object in constructing call.");
#endif
    masm.bind(&skipThisReplace);

    // At this point, BaselineStubReg points to the ICCall_Fallback stub, which is NOT
    // a MonitoredStub, but rather a MonitoredFallbackStub.  To use EmitEnterTypeMonitorIC,
    // first load the ICTypeMonitor_Fallback stub into BaselineStubReg.  Then, use
    // EmitEnterTypeMonitorIC with a custom struct offset.
    masm.loadPtr(Address(BaselineStubReg, ICMonitoredFallbackStub::offsetOfFallbackMonitorStub()),
                 BaselineStubReg);
    EmitEnterTypeMonitorIC(masm, ICTypeMonitor_Fallback::offsetOfFirstMonitorStub());

    return true;
}

bool
ICCall_Fallback::Compiler::postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code)
{
    if (MOZ_UNLIKELY(isSpread_))
        return true;

    CodeOffsetLabel offset(returnOffset_);
    offset.fixup(&masm);
    cx->compartment()->jitCompartment()->initBaselineCallReturnAddr(code->raw() + offset.offset());
    return true;
}

typedef bool (*CreateThisFn)(JSContext* cx, HandleObject callee, MutableHandleValue rval);
static const VMFunction CreateThisInfoBaseline = FunctionInfo<CreateThisFn>(CreateThis);

bool
ICCallScriptedCompiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    GeneralRegisterSet regs(availableGeneralRegs(0));
    bool canUseTailCallReg = regs.has(BaselineTailCallReg);

    Register argcReg = R0.scratchReg();
    MOZ_ASSERT(argcReg != ArgumentsRectifierReg);

    regs.take(argcReg);
    regs.take(ArgumentsRectifierReg);
    regs.takeUnchecked(BaselineTailCallReg);

    if (isSpread_)
        guardSpreadCall(masm, argcReg, &failure);

    // Load the callee in R1.
    // Stack Layout: [ ..., CalleeVal, ThisVal, Arg0Val, ..., ArgNVal, +ICStackValueOffset+ ]
    if (isSpread_) {
        masm.loadValue(Address(BaselineStackReg, 2 * sizeof(Value) + ICStackValueOffset), R1);
    } else {
        BaseValueIndex calleeSlot(BaselineStackReg, argcReg, ICStackValueOffset + sizeof(Value));
        masm.loadValue(calleeSlot, R1);
    }
    regs.take(R1);

    // Ensure callee is an object.
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    // Ensure callee is a function.
    Register callee = masm.extractObject(R1, ExtractTemp0);

    // If calling a specific script, check if the script matches.  Otherwise, ensure that
    // callee function is scripted.  Leave calleeScript in |callee| reg.
    if (callee_) {
        MOZ_ASSERT(kind == ICStub::Call_Scripted);

        // Check if the object matches this callee.
        Address expectedCallee(BaselineStubReg, ICCall_Scripted::offsetOfCallee());
        masm.branchPtr(Assembler::NotEqual, expectedCallee, callee, &failure);

        // Guard against relazification.
        masm.branchIfFunctionHasNoScript(callee, &failure);
    } else {
        // Ensure the object is a function.
        masm.branchTestObjClass(Assembler::NotEqual, callee, regs.getAny(), &JSFunction::class_,
                                &failure);
        if (isConstructing_)
            masm.branchIfNotInterpretedConstructor(callee, regs.getAny(), &failure);
        else
            masm.branchIfFunctionHasNoScript(callee, &failure);
    }

    // Load the JSScript.
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);

    // Load the start of the target JitCode.
    Register code;
    if (!isConstructing_) {
        code = regs.takeAny();
        masm.loadBaselineOrIonRaw(callee, code, &failure);
    } else {
        Address scriptCode(callee, JSScript::offsetOfBaselineOrIonRaw());
        masm.branchPtr(Assembler::Equal, scriptCode, ImmPtr(nullptr), &failure);
    }

    // We no longer need R1.
    regs.add(R1);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, regs.getAny());
    if (canUseTailCallReg)
        regs.add(BaselineTailCallReg);

    Label failureLeaveStubFrame;

    if (isConstructing_) {
        // Save argc before call.
        masm.push(argcReg);

        // Stack now looks like:
        //      [..., Callee, ThisV, Arg0V, ..., ArgNV, StubFrameHeader, ArgC ]
        if (isSpread_) {
            masm.loadValue(Address(BaselineStackReg,
                                   2 * sizeof(Value) + STUB_FRAME_SIZE + sizeof(size_t)), R1);
        } else {
            BaseValueIndex calleeSlot2(BaselineStackReg, argcReg,
                                       sizeof(Value) + STUB_FRAME_SIZE + sizeof(size_t));
            masm.loadValue(calleeSlot2, R1);
        }
        masm.push(masm.extractObject(R1, ExtractTemp0));
        if (!callVM(CreateThisInfoBaseline, masm))
            return false;

        // Return of CreateThis must be an object.
#ifdef DEBUG
        Label createdThisIsObject;
        masm.branchTestObject(Assembler::Equal, JSReturnOperand, &createdThisIsObject);
        masm.assumeUnreachable("The return of CreateThis must be an object.");
        masm.bind(&createdThisIsObject);
#endif

        // Reset the register set from here on in.
        MOZ_ASSERT(JSReturnOperand == R0);
        regs = availableGeneralRegs(0);
        regs.take(R0);
        regs.take(ArgumentsRectifierReg);
        argcReg = regs.takeAny();

        // Restore saved argc so we can use it to calculate the address to save
        // the resulting this object to.
        masm.pop(argcReg);

        // Save "this" value back into pushed arguments on stack.  R0 can be clobbered after that.
        // Stack now looks like:
        //      [..., Callee, ThisV, Arg0V, ..., ArgNV, StubFrameHeader ]
        if (isSpread_) {
            masm.storeValue(R0, Address(BaselineStackReg, sizeof(Value) + STUB_FRAME_SIZE));
        } else {
            BaseValueIndex thisSlot(BaselineStackReg, argcReg, STUB_FRAME_SIZE);
            masm.storeValue(R0, thisSlot);
        }

        // Restore the stub register from the baseline stub frame.
        masm.loadPtr(Address(BaselineStackReg, STUB_FRAME_SAVED_STUB_OFFSET), BaselineStubReg);

        // Reload callee script. Note that a GC triggered by CreateThis may
        // have destroyed the callee BaselineScript and IonScript. CreateThis is
        // safely repeatable though, so in this case we just leave the stub frame
        // and jump to the next stub.

        // Just need to load the script now.
        if (isSpread_) {
            masm.loadValue(Address(BaselineStackReg, 2 * sizeof(Value) + STUB_FRAME_SIZE), R0);
        } else {
            BaseValueIndex calleeSlot3(BaselineStackReg, argcReg, sizeof(Value) + STUB_FRAME_SIZE);
            masm.loadValue(calleeSlot3, R0);
        }
        callee = masm.extractObject(R0, ExtractTemp0);
        regs.add(R0);
        regs.takeUnchecked(callee);
        masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);

        code = regs.takeAny();
        masm.loadBaselineOrIonRaw(callee, code, &failureLeaveStubFrame);

        // Release callee register, but don't add ExtractTemp0 back into the pool
        // ExtractTemp0 is used later, and if it's allocated to some other register at that
        // point, it will get clobbered when used.
        if (callee != ExtractTemp0)
            regs.add(callee);

        if (canUseTailCallReg)
            regs.addUnchecked(BaselineTailCallReg);
    }
    Register scratch = regs.takeAny();

    // Values are on the stack left-to-right. Calling convention wants them
    // right-to-left so duplicate them on the stack in reverse order.
    // |this| and callee are pushed last.
    if (isSpread_)
        pushSpreadCallArguments(masm, regs, argcReg, /* isJitCall = */ true);
    else
        pushCallArguments(masm, regs, argcReg, /* isJitCall = */ true);

    // The callee is on top of the stack. Pop and unbox it.
    ValueOperand val = regs.takeAnyValue();
    masm.popValue(val);
    callee = masm.extractObject(val, ExtractTemp0);

    EmitCreateStubFrameDescriptor(masm, scratch);

    // Note that we use Push, not push, so that callJit will align the stack
    // properly on ARM.
    masm.Push(argcReg);
    masm.PushCalleeToken(callee, isConstructing_);
    masm.Push(scratch);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), callee);
    masm.branch32(Assembler::AboveOrEqual, argcReg, callee, &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != code);
        MOZ_ASSERT(ArgumentsRectifierReg != argcReg);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), code);
        masm.loadPtr(Address(code, JitCode::offsetOfCode()), code);
        masm.mov(argcReg, ArgumentsRectifierReg);
    }

    masm.bind(&noUnderflow);
    masm.callJit(code);

    // If this is a constructing call, and the callee returns a non-object, replace it with
    // the |this| object passed in.
    if (isConstructing_) {
        Label skipThisReplace;
        masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);

        // Current stack: [ Padding?, ARGVALS..., ThisVal, ActualArgc, Callee, Descriptor ]
        // However, we can't use this ThisVal, because it hasn't been traced.  We need to use
        // The ThisVal higher up the stack:
        // Current stack: [ ThisVal, ARGVALS..., ...STUB FRAME...,
        //                  Padding?, ARGVALS..., ThisVal, ActualArgc, Callee, Descriptor ]

        // Restore the BaselineFrameReg based on the frame descriptor.
        //
        // BaselineFrameReg = BaselineStackReg
        //                  + sizeof(Descriptor) + sizeof(Callee) + sizeof(ActualArgc)
        //                  + stubFrameSize(Descriptor)
        //                  - sizeof(BaselineStubReg) - sizeof(BaselineFrameReg)
        Address descriptorAddr(BaselineStackReg, 0);
        masm.loadPtr(descriptorAddr, BaselineFrameReg);
        masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), BaselineFrameReg);
        masm.addPtr(Imm32((3 - 2) * sizeof(size_t)), BaselineFrameReg);
        masm.addPtr(BaselineStackReg, BaselineFrameReg);

        // Load the number of arguments present before the stub frame.
        Register argcReg = JSReturnOperand.scratchReg();
        if (isSpread_) {
            // Account for the Array object.
            masm.move32(Imm32(1), argcReg);
        } else {
            Address argcAddr(BaselineStackReg, 2 * sizeof(size_t));
            masm.loadPtr(argcAddr, argcReg);
        }

        // Current stack: [ ThisVal, ARGVALS..., ...STUB FRAME..., <-- BaselineFrameReg
        //                  Padding?, ARGVALS..., ThisVal, ActualArgc, Callee, Descriptor ]
        //
        // &ThisVal = BaselineFrameReg + argc * sizeof(Value) + STUB_FRAME_SIZE
        BaseValueIndex thisSlotAddr(BaselineFrameReg, argcReg, STUB_FRAME_SIZE);
        masm.loadValue(thisSlotAddr, JSReturnOperand);
#ifdef DEBUG
        masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);
        masm.assumeUnreachable("Return of constructing call should be an object.");
#endif
        masm.bind(&skipThisReplace);
    }

    leaveStubFrame(masm, true);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Leave stub frame and restore argc for the next stub.
    masm.bind(&failureLeaveStubFrame);
    leaveStubFrame(masm, false);
    if (argcReg != R0.scratchReg())
        masm.mov(argcReg, R0.scratchReg());

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

typedef bool (*CopyArrayFn)(JSContext*, HandleArrayObject, MutableHandleValue);
static const VMFunction CopyArrayInfo = FunctionInfo<CopyArrayFn>(CopyArray);

bool
ICCall_StringSplit::Compiler::generateStubCode(MacroAssembler& masm)
{
    // Stack Layout: [ ..., CalleeVal, ThisVal, Arg0Val, +ICStackValueOffset+ ]
    GeneralRegisterSet regs = availableGeneralRegs(0);
    Label failureRestoreArgc;
#ifdef DEBUG
    Label oneArg;
    Register argcReg = R0.scratchReg();
    masm.branch32(Assembler::Equal, argcReg, Imm32(1), &oneArg);
    masm.assumeUnreachable("Expected argc == 1");
    masm.bind(&oneArg);
#endif
    Register scratchReg = regs.takeAny();

    // Guard that callee is native function js::str_split.
    {
        Address calleeAddr(BaselineStackReg, ICStackValueOffset + (2 * sizeof(Value)));
        ValueOperand calleeVal = regs.takeAnyValue();

        // Ensure that callee is an object.
        masm.loadValue(calleeAddr, calleeVal);
        masm.branchTestObject(Assembler::NotEqual, calleeVal, &failureRestoreArgc);

        // Ensure that callee is a function.
        Register calleeObj = masm.extractObject(calleeVal, ExtractTemp0);
        masm.branchTestObjClass(Assembler::NotEqual, calleeObj, scratchReg,
                                &JSFunction::class_, &failureRestoreArgc);

        // Ensure that callee's function impl is the native str_split.
        masm.loadPtr(Address(calleeObj, JSFunction::offsetOfNativeOrScript()), scratchReg);
        masm.branchPtr(Assembler::NotEqual, scratchReg, ImmPtr(js::str_split), &failureRestoreArgc);

        regs.add(calleeVal);
    }

    // Guard argument.
    {
        // Ensure that arg is a string.
        Address argAddr(BaselineStackReg, ICStackValueOffset);
        ValueOperand argVal = regs.takeAnyValue();

        masm.loadValue(argAddr, argVal);
        masm.branchTestString(Assembler::NotEqual, argVal, &failureRestoreArgc);

        Register argString = masm.extractString(argVal, ExtractTemp0);
        masm.branchPtr(Assembler::NotEqual, Address(BaselineStubReg, offsetOfExpectedArg()),
                       argString, &failureRestoreArgc);
        regs.add(argVal);
    }

    // Guard this-value.
    {
        // Ensure that thisv is a string.
        Address thisvAddr(BaselineStackReg, ICStackValueOffset + sizeof(Value));
        ValueOperand thisvVal = regs.takeAnyValue();

        masm.loadValue(thisvAddr, thisvVal);
        masm.branchTestString(Assembler::NotEqual, thisvVal, &failureRestoreArgc);

        Register thisvString = masm.extractString(thisvVal, ExtractTemp0);
        masm.branchPtr(Assembler::NotEqual, Address(BaselineStubReg, offsetOfExpectedThis()),
                       thisvString, &failureRestoreArgc);
        regs.add(thisvVal);
    }

    // Main stub body.
    {
        Register paramReg = regs.takeAny();

        // Push arguments.
        enterStubFrame(masm, scratchReg);
        masm.loadPtr(Address(BaselineStubReg, offsetOfTemplateObject()), paramReg);
        masm.push(paramReg);

        if (!callVM(CopyArrayInfo, masm))
            return false;
        leaveStubFrame(masm);
        regs.add(paramReg);
    }

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Guard failure path.
    masm.bind(&failureRestoreArgc);
    masm.move32(Imm32(1), R0.scratchReg());
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICCall_IsSuspendedStarGenerator::Compiler::generateStubCode(MacroAssembler& masm)
{
    // The IsSuspendedStarGenerator intrinsic is only called in self-hosted
    // code, so it's safe to assume we have a single argument and the callee
    // is our intrinsic.

    GeneralRegisterSet regs = availableGeneralRegs(0);

    // Load the argument.
    Address argAddr(BaselineStackReg, ICStackValueOffset);
    ValueOperand argVal = regs.takeAnyValue();
    masm.loadValue(argAddr, argVal);

    // Check if it's an object.
    Label returnFalse;
    Register genObj = regs.takeAny();
    masm.branchTestObject(Assembler::NotEqual, argVal, &returnFalse);
    masm.unboxObject(argVal, genObj);

    // Check if it's a StarGeneratorObject.
    Register scratch = regs.takeAny();
    masm.branchTestObjClass(Assembler::NotEqual, genObj, scratch, &StarGeneratorObject::class_,
                            &returnFalse);

    // If the yield index slot holds an int32 value < YIELD_INDEX_CLOSING,
    // the generator is suspended.
    masm.loadValue(Address(genObj, GeneratorObject::offsetOfYieldIndexSlot()), argVal);
    masm.branchTestInt32(Assembler::NotEqual, argVal, &returnFalse);
    masm.unboxInt32(argVal, scratch);
    masm.branch32(Assembler::AboveOrEqual, scratch, Imm32(StarGeneratorObject::YIELD_INDEX_CLOSING),
                  &returnFalse);

    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    masm.bind(&returnFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);
    return true;
}

bool
ICCall_Native::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    GeneralRegisterSet regs(availableGeneralRegs(0));

    Register argcReg = R0.scratchReg();
    regs.take(argcReg);
    regs.takeUnchecked(BaselineTailCallReg);

    if (isSpread_)
        guardSpreadCall(masm, argcReg, &failure);

    // Load the callee in R1.
    if (isSpread_) {
        masm.loadValue(Address(BaselineStackReg, ICStackValueOffset + 2 * sizeof(Value)), R1);
    } else {
        BaseValueIndex calleeSlot(BaselineStackReg, argcReg, ICStackValueOffset + sizeof(Value));
        masm.loadValue(calleeSlot, R1);
    }
    regs.take(R1);

    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    // Ensure callee matches this stub's callee.
    Register callee = masm.extractObject(R1, ExtractTemp0);
    Address expectedCallee(BaselineStubReg, ICCall_Native::offsetOfCallee());
    masm.branchPtr(Assembler::NotEqual, expectedCallee, callee, &failure);

    regs.add(R1);
    regs.takeUnchecked(callee);

    // Push a stub frame so that we can perform a non-tail call.
    // Note that this leaves the return address in TailCallReg.
    enterStubFrame(masm, regs.getAny());

    // Values are on the stack left-to-right. Calling convention wants them
    // right-to-left so duplicate them on the stack in reverse order.
    // |this| and callee are pushed last.
    if (isSpread_)
        pushSpreadCallArguments(masm, regs, argcReg, /* isJitCall = */ false);
    else
        pushCallArguments(masm, regs, argcReg, /* isJitCall = */ false);

    if (isConstructing_) {
        // Stack looks like: [ ..., Arg0Val, ThisVal, CalleeVal ]
        // Replace ThisVal with MagicValue(JS_IS_CONSTRUCTING)
        masm.storeValue(MagicValue(JS_IS_CONSTRUCTING), Address(BaselineStackReg, sizeof(Value)));
    }

    masm.checkStackAlignment();

    // Native functions have the signature:
    //
    //    bool (*)(JSContext*, unsigned, Value* vp)
    //
    // Where vp[0] is space for callee/return value, vp[1] is |this|, and vp[2] onward
    // are the function arguments.

    // Initialize vp.
    Register vpReg = regs.takeAny();
    masm.movePtr(StackPointer, vpReg);

    // Construct a native exit frame.
    masm.push(argcReg);

    Register scratch = regs.takeAny();
    EmitCreateStubFrameDescriptor(masm, scratch);
    masm.push(scratch);
    masm.push(BaselineTailCallReg);
    masm.enterFakeExitFrame(NativeExitFrameLayout::Token());

    // Execute call.
    masm.setupUnalignedABICall(3, scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(argcReg);
    masm.passABIArg(vpReg);

#if defined(JS_ARM_SIMULATOR) || defined(JS_MIPS_SIMULATOR)
    // The simulator requires VM calls to be redirected to a special swi
    // instruction to handle them, so we store the redirected pointer in the
    // stub and use that instead of the original one.
    masm.callWithABI(Address(BaselineStubReg, ICCall_Native::offsetOfNative()));
#else
    masm.callWithABI(Address(callee, JSFunction::offsetOfNativeOrScript()));
#endif

    // Test for failure.
    masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

    // Load the return value into R0.
    masm.loadValue(Address(StackPointer, NativeExitFrameLayout::offsetOfResult()), R0);

    leaveStubFrame(masm);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICCall_ClassHook::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    GeneralRegisterSet regs(availableGeneralRegs(0));

    Register argcReg = R0.scratchReg();
    regs.take(argcReg);
    regs.takeUnchecked(BaselineTailCallReg);

    // Load the callee in R1.
    BaseValueIndex calleeSlot(BaselineStackReg, argcReg, ICStackValueOffset + sizeof(Value));
    masm.loadValue(calleeSlot, R1);
    regs.take(R1);

    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    // Ensure the callee's class matches the one in this stub.
    Register callee = masm.extractObject(R1, ExtractTemp0);
    Register scratch = regs.takeAny();
    masm.loadObjClass(callee, scratch);
    masm.branchPtr(Assembler::NotEqual,
                   Address(BaselineStubReg, ICCall_ClassHook::offsetOfClass()),
                   scratch, &failure);

    regs.add(R1);
    regs.takeUnchecked(callee);

    // Push a stub frame so that we can perform a non-tail call.
    // Note that this leaves the return address in TailCallReg.
    enterStubFrame(masm, regs.getAny());

    regs.add(scratch);
    pushCallArguments(masm, regs, argcReg, /* isJitCall = */ false);
    regs.take(scratch);

    if (isConstructing_) {
        // Stack looks like: [ ..., Arg0Val, ThisVal, CalleeVal ]
        // Replace ThisVal with MagicValue(JS_IS_CONSTRUCTING)
        masm.storeValue(MagicValue(JS_IS_CONSTRUCTING), Address(BaselineStackReg, sizeof(Value)));
    }

    masm.checkStackAlignment();

    // Native functions have the signature:
    //
    //    bool (*)(JSContext*, unsigned, Value* vp)
    //
    // Where vp[0] is space for callee/return value, vp[1] is |this|, and vp[2] onward
    // are the function arguments.

    // Initialize vp.
    Register vpReg = regs.takeAny();
    masm.movePtr(StackPointer, vpReg);

    // Construct a native exit frame.
    masm.push(argcReg);

    EmitCreateStubFrameDescriptor(masm, scratch);
    masm.push(scratch);
    masm.push(BaselineTailCallReg);
    masm.enterFakeExitFrame(NativeExitFrameLayout::Token());

    // Execute call.
    masm.setupUnalignedABICall(3, scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(argcReg);
    masm.passABIArg(vpReg);
    masm.callWithABI(Address(BaselineStubReg, ICCall_ClassHook::offsetOfNative()));

    // Test for failure.
    masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

    // Load the return value into R0.
    masm.loadValue(Address(StackPointer, NativeExitFrameLayout::offsetOfResult()), R0);

    leaveStubFrame(masm);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICCall_ScriptedApplyArray::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    GeneralRegisterSet regs(availableGeneralRegs(0));

    Register argcReg = R0.scratchReg();
    regs.take(argcReg);
    regs.takeUnchecked(BaselineTailCallReg);
    regs.takeUnchecked(ArgumentsRectifierReg);

    //
    // Validate inputs
    //

    Register target = guardFunApply(masm, regs, argcReg, /*checkNative=*/false,
                                    FunApply_Array, &failure);
    if (regs.has(target)) {
        regs.take(target);
    } else {
        // If target is already a reserved reg, take another register for it, because it's
        // probably currently an ExtractTemp, which might get clobbered later.
        Register targetTemp = regs.takeAny();
        masm.movePtr(target, targetTemp);
        target = targetTemp;
    }

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, regs.getAny());

    //
    // Push arguments
    //

    // Stack now looks like:
    //                                      BaselineFrameReg -------------------.
    //                                                                          v
    //      [..., js_fun_apply, TargetV, TargetThisV, ArgsArrayV, StubFrameHeader]

    // Push all array elements onto the stack:
    Address arrayVal(BaselineFrameReg, STUB_FRAME_SIZE);
    pushArrayArguments(masm, arrayVal, regs);

    // Stack now looks like:
    //                                      BaselineFrameReg -------------------.
    //                                                                          v
    //      [..., js_fun_apply, TargetV, TargetThisV, ArgsArrayV, StubFrameHeader,
    //       PushedArgN, ..., PushedArg0]
    // Can't fail after this, so it's ok to clobber argcReg.

    // Push actual argument 0 as |thisv| for call.
    masm.pushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE + sizeof(Value)));

    // All pushes after this use Push instead of push to make sure ARM can align
    // stack properly for call.
    Register scratch = regs.takeAny();
    EmitCreateStubFrameDescriptor(masm, scratch);

    // Reload argc from length of array.
    masm.extractObject(arrayVal, argcReg);
    masm.loadPtr(Address(argcReg, NativeObject::offsetOfElements()), argcReg);
    masm.load32(Address(argcReg, ObjectElements::offsetOfInitializedLength()), argcReg);

    masm.Push(argcReg);
    masm.Push(target);
    masm.Push(scratch);

    // Load nargs into scratch for underflow check, and then load jitcode pointer into target.
    masm.load16ZeroExtend(Address(target, JSFunction::offsetOfNargs()), scratch);
    masm.loadPtr(Address(target, JSFunction::offsetOfNativeOrScript()), target);
    masm.loadBaselineOrIonRaw(target, target, nullptr);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.branch32(Assembler::AboveOrEqual, argcReg, scratch, &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != target);
        MOZ_ASSERT(ArgumentsRectifierReg != argcReg);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), target);
        masm.loadPtr(Address(target, JitCode::offsetOfCode()), target);
        masm.mov(argcReg, ArgumentsRectifierReg);
    }
    masm.bind(&noUnderflow);
    regs.add(argcReg);

    // Do call
    masm.callJit(target);
    leaveStubFrame(masm, true);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICCall_ScriptedApplyArguments::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    GeneralRegisterSet regs(availableGeneralRegs(0));

    Register argcReg = R0.scratchReg();
    regs.take(argcReg);
    regs.takeUnchecked(BaselineTailCallReg);
    regs.takeUnchecked(ArgumentsRectifierReg);

    //
    // Validate inputs
    //

    Register target = guardFunApply(masm, regs, argcReg, /*checkNative=*/false,
                                    FunApply_MagicArgs, &failure);
    if (regs.has(target)) {
        regs.take(target);
    } else {
        // If target is already a reserved reg, take another register for it, because it's
        // probably currently an ExtractTemp, which might get clobbered later.
        Register targetTemp = regs.takeAny();
        masm.movePtr(target, targetTemp);
        target = targetTemp;
    }

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, regs.getAny());

    //
    // Push arguments
    //

    // Stack now looks like:
    //      [..., js_fun_apply, TargetV, TargetThisV, MagicArgsV, StubFrameHeader]

    // Push all arguments supplied to caller function onto the stack.
    pushCallerArguments(masm, regs);

    // Stack now looks like:
    //                                      BaselineFrameReg -------------------.
    //                                                                          v
    //      [..., js_fun_apply, TargetV, TargetThisV, MagicArgsV, StubFrameHeader,
    //       PushedArgN, ..., PushedArg0]
    // Can't fail after this, so it's ok to clobber argcReg.

    // Push actual argument 0 as |thisv| for call.
    masm.pushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE + sizeof(Value)));

    // All pushes after this use Push instead of push to make sure ARM can align
    // stack properly for call.
    Register scratch = regs.takeAny();
    EmitCreateStubFrameDescriptor(masm, scratch);

    masm.loadPtr(Address(BaselineFrameReg, 0), argcReg);
    masm.loadPtr(Address(argcReg, BaselineFrame::offsetOfNumActualArgs()), argcReg);
    masm.Push(argcReg);
    masm.Push(target);
    masm.Push(scratch);

    // Load nargs into scratch for underflow check, and then load jitcode pointer into target.
    masm.load16ZeroExtend(Address(target, JSFunction::offsetOfNargs()), scratch);
    masm.loadPtr(Address(target, JSFunction::offsetOfNativeOrScript()), target);
    masm.loadBaselineOrIonRaw(target, target, nullptr);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.branch32(Assembler::AboveOrEqual, argcReg, scratch, &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != target);
        MOZ_ASSERT(ArgumentsRectifierReg != argcReg);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), target);
        masm.loadPtr(Address(target, JitCode::offsetOfCode()), target);
        masm.mov(argcReg, ArgumentsRectifierReg);
    }
    masm.bind(&noUnderflow);
    regs.add(argcReg);

    // Do call
    masm.callJit(target);
    leaveStubFrame(masm, true);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICCall_ScriptedFunCall::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    GeneralRegisterSet regs(availableGeneralRegs(0));
    bool canUseTailCallReg = regs.has(BaselineTailCallReg);

    Register argcReg = R0.scratchReg();
    MOZ_ASSERT(argcReg != ArgumentsRectifierReg);

    regs.take(argcReg);
    regs.take(ArgumentsRectifierReg);
    regs.takeUnchecked(BaselineTailCallReg);

    // Load the callee in R1.
    // Stack Layout: [ ..., CalleeVal, ThisVal, Arg0Val, ..., ArgNVal, +ICStackValueOffset+ ]
    BaseValueIndex calleeSlot(BaselineStackReg, argcReg, ICStackValueOffset + sizeof(Value));
    masm.loadValue(calleeSlot, R1);
    regs.take(R1);

    // Ensure callee is js_fun_call.
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    Register callee = masm.extractObject(R1, ExtractTemp0);
    masm.branchTestObjClass(Assembler::NotEqual, callee, regs.getAny(), &JSFunction::class_,
                            &failure);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);
    masm.branchPtr(Assembler::NotEqual, callee, ImmPtr(js_fun_call), &failure);

    // Ensure |this| is a scripted function with JIT code.
    BaseIndex thisSlot(BaselineStackReg, argcReg, TimesEight, ICStackValueOffset);
    masm.loadValue(thisSlot, R1);

    masm.branchTestObject(Assembler::NotEqual, R1, &failure);
    callee = masm.extractObject(R1, ExtractTemp0);

    masm.branchTestObjClass(Assembler::NotEqual, callee, regs.getAny(), &JSFunction::class_,
                            &failure);
    masm.branchIfFunctionHasNoScript(callee, &failure);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);

    // Load the start of the target JitCode.
    Register code = regs.takeAny();
    masm.loadBaselineOrIonRaw(callee, code, &failure);

    // We no longer need R1.
    regs.add(R1);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, regs.getAny());
    if (canUseTailCallReg)
        regs.add(BaselineTailCallReg);

    // Decrement argc if argc > 0. If argc == 0, push |undefined| as |this|.
    Label zeroArgs, done;
    masm.branchTest32(Assembler::Zero, argcReg, argcReg, &zeroArgs);

    // Avoid the copy of the callee (function.call).
    masm.sub32(Imm32(1), argcReg);

    // Values are on the stack left-to-right. Calling convention wants them
    // right-to-left so duplicate them on the stack in reverse order.
    pushCallArguments(masm, regs, argcReg, /* isJitCall = */ true);

    // Pop scripted callee (the original |this|).
    ValueOperand val = regs.takeAnyValue();
    masm.popValue(val);

    masm.jump(&done);
    masm.bind(&zeroArgs);

    // Copy scripted callee (the original |this|).
    Address thisSlotFromStubFrame(BaselineFrameReg, STUB_FRAME_SIZE);
    masm.loadValue(thisSlotFromStubFrame, val);

    // Align the stack.
    masm.alignJitStackBasedOnNArgs(0);

    // Store the new |this|.
    masm.pushValue(UndefinedValue());

    masm.bind(&done);

    // Unbox scripted callee.
    callee = masm.extractObject(val, ExtractTemp0);

    Register scratch = regs.takeAny();
    EmitCreateStubFrameDescriptor(masm, scratch);

    // Note that we use Push, not push, so that callJit will align the stack
    // properly on ARM.
    masm.Push(argcReg);
    masm.Push(callee);
    masm.Push(scratch);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), callee);
    masm.branch32(Assembler::AboveOrEqual, argcReg, callee, &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != code);
        MOZ_ASSERT(ArgumentsRectifierReg != argcReg);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), code);
        masm.loadPtr(Address(code, JitCode::offsetOfCode()), code);
        masm.mov(argcReg, ArgumentsRectifierReg);
    }

    masm.bind(&noUnderflow);
    masm.callJit(code);

    leaveStubFrame(masm, true);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

static bool
DoubleValueToInt32ForSwitch(Value* v)
{
    double d = v->toDouble();
    int32_t truncated = int32_t(d);
    if (d != double(truncated))
        return false;

    v->setInt32(truncated);
    return true;
}

bool
ICTableSwitch::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label isInt32, notInt32, outOfRange;
    Register scratch = R1.scratchReg();

    masm.branchTestInt32(Assembler::NotEqual, R0, &notInt32);

    Register key = masm.extractInt32(R0, ExtractTemp0);

    masm.bind(&isInt32);

    masm.load32(Address(BaselineStubReg, offsetof(ICTableSwitch, min_)), scratch);
    masm.sub32(scratch, key);
    masm.branch32(Assembler::BelowOrEqual,
                  Address(BaselineStubReg, offsetof(ICTableSwitch, length_)), key, &outOfRange);

    masm.loadPtr(Address(BaselineStubReg, offsetof(ICTableSwitch, table_)), scratch);
    masm.loadPtr(BaseIndex(scratch, key, ScalePointer), scratch);

    EmitChangeICReturnAddress(masm, scratch);
    EmitReturnFromIC(masm);

    masm.bind(&notInt32);

    masm.branchTestDouble(Assembler::NotEqual, R0, &outOfRange);
    if (cx->runtime()->jitSupportsFloatingPoint) {
        masm.unboxDouble(R0, FloatReg0);

        // N.B. -0 === 0, so convert -0 to a 0 int32.
        masm.convertDoubleToInt32(FloatReg0, key, &outOfRange, /* negativeZeroCheck = */ false);
    } else {
        // Pass pointer to double value.
        masm.pushValue(R0);
        masm.movePtr(StackPointer, R0.scratchReg());

        masm.setupUnalignedABICall(1, scratch);
        masm.passABIArg(R0.scratchReg());
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, DoubleValueToInt32ForSwitch));

        // If the function returns |true|, the value has been converted to
        // int32.
        masm.mov(ReturnReg, scratch);
        masm.popValue(R0);
        masm.branchIfFalseBool(scratch, &outOfRange);
        masm.unboxInt32(R0, key);
    }
    masm.jump(&isInt32);

    masm.bind(&outOfRange);

    masm.loadPtr(Address(BaselineStubReg, offsetof(ICTableSwitch, defaultTarget_)), scratch);

    EmitChangeICReturnAddress(masm, scratch);
    EmitReturnFromIC(masm);
    return true;
}

ICStub*
ICTableSwitch::Compiler::getStub(ICStubSpace* space)
{
    JitCode* code = getStubCode();
    if (!code)
        return nullptr;

    jsbytecode* pc = pc_;
    pc += JUMP_OFFSET_LEN;
    int32_t low = GET_JUMP_OFFSET(pc);
    pc += JUMP_OFFSET_LEN;
    int32_t high = GET_JUMP_OFFSET(pc);
    int32_t length = high - low + 1;
    pc += JUMP_OFFSET_LEN;

    void** table = (void**) space->alloc(sizeof(void*) * length);
    if (!table)
        return nullptr;

    jsbytecode* defaultpc = pc_ + GET_JUMP_OFFSET(pc_);

    for (int32_t i = 0; i < length; i++) {
        int32_t off = GET_JUMP_OFFSET(pc);
        if (off)
            table[i] = pc_ + off;
        else
            table[i] = defaultpc;
        pc += JUMP_OFFSET_LEN;
    }

    return ICStub::New<ICTableSwitch>(space, code, table, low, length, defaultpc);
}

void
ICTableSwitch::fixupJumpTable(JSScript* script, BaselineScript* baseline)
{
    defaultTarget_ = baseline->nativeCodeForPC(script, (jsbytecode*) defaultTarget_);

    for (int32_t i = 0; i < length_; i++)
        table_[i] = baseline->nativeCodeForPC(script, (jsbytecode*) table_[i]);
}

//
// IteratorNew_Fallback
//

static bool
DoIteratorNewFallback(JSContext* cx, BaselineFrame* frame, ICIteratorNew_Fallback* stub,
                      HandleValue value, MutableHandleValue res)
{
    jsbytecode* pc = stub->icEntry()->pc(frame->script());
    FallbackICSpew(cx, stub, "IteratorNew");

    uint8_t flags = GET_UINT8(pc);
    res.set(value);
    return ValueToIterator(cx, flags, res);
}

typedef bool (*DoIteratorNewFallbackFn)(JSContext*, BaselineFrame*, ICIteratorNew_Fallback*,
                                        HandleValue, MutableHandleValue);
static const VMFunction DoIteratorNewFallbackInfo =
    FunctionInfo<DoIteratorNewFallbackFn>(DoIteratorNewFallback, TailCall, PopValues(1));

bool
ICIteratorNew_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    // Sync stack for the decompiler.
    masm.pushValue(R0);

    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoIteratorNewFallbackInfo, masm);
}

//
// IteratorMore_Fallback
//

static bool
DoIteratorMoreFallback(JSContext* cx, BaselineFrame* frame, ICIteratorMore_Fallback* stub_,
                       HandleObject iterObj, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICIteratorMore_Fallback*> stub(frame, stub_);

    FallbackICSpew(cx, stub, "IteratorMore");

    if (!IteratorMore(cx, iterObj, res))
        return false;

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (!res.isMagic(JS_NO_ITER_VALUE) && !res.isString())
        stub->setHasNonStringResult();

    if (iterObj->is<PropertyIteratorObject>() &&
        !stub->hasStub(ICStub::IteratorMore_Native))
    {
        ICIteratorMore_Native::Compiler compiler(cx);
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(frame->script()));
        if (!newStub)
            return false;
        stub->addNewStub(newStub);
    }

    return true;
}

typedef bool (*DoIteratorMoreFallbackFn)(JSContext*, BaselineFrame*, ICIteratorMore_Fallback*,
                                         HandleObject, MutableHandleValue);
static const VMFunction DoIteratorMoreFallbackInfo =
    FunctionInfo<DoIteratorMoreFallbackFn>(DoIteratorMoreFallback, TailCall);

bool
ICIteratorMore_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    masm.unboxObject(R0, R0.scratchReg());
    masm.push(R0.scratchReg());
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoIteratorMoreFallbackInfo, masm);
}

//
// IteratorMore_Native
//

bool
ICIteratorMore_Native::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    Register obj = masm.extractObject(R0, ExtractTemp0);

    GeneralRegisterSet regs(availableGeneralRegs(1));
    Register nativeIterator = regs.takeAny();
    Register scratch = regs.takeAny();

    masm.branchTestObjClass(Assembler::NotEqual, obj, scratch,
                            &PropertyIteratorObject::class_, &failure);
    masm.loadObjPrivate(obj, JSObject::ITER_CLASS_NFIXED_SLOTS, nativeIterator);

    masm.branchTest32(Assembler::NonZero, Address(nativeIterator, offsetof(NativeIterator, flags)),
                      Imm32(JSITER_FOREACH), &failure);

    // If props_cursor < props_end, load the next string and advance the cursor.
    // Else, return MagicValue(JS_NO_ITER_VALUE).
    Label iterDone;
    Address cursorAddr(nativeIterator, offsetof(NativeIterator, props_cursor));
    Address cursorEndAddr(nativeIterator, offsetof(NativeIterator, props_end));
    masm.loadPtr(cursorAddr, scratch);
    masm.branchPtr(Assembler::BelowOrEqual, cursorEndAddr, scratch, &iterDone);

    // Get next string.
    masm.loadPtr(Address(scratch, 0), scratch);

    // Increase the cursor.
    masm.addPtr(Imm32(sizeof(JSString*)), cursorAddr);

    masm.tagValue(JSVAL_TYPE_STRING, scratch, R0);
    EmitReturnFromIC(masm);

    masm.bind(&iterDone);
    masm.moveValue(MagicValue(JS_NO_ITER_VALUE), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// IteratorClose_Fallback
//

static bool
DoIteratorCloseFallback(JSContext* cx, ICIteratorClose_Fallback* stub, HandleValue iterValue)
{
    FallbackICSpew(cx, stub, "IteratorClose");

    RootedObject iteratorObject(cx, &iterValue.toObject());
    return CloseIterator(cx, iteratorObject);
}

typedef bool (*DoIteratorCloseFallbackFn)(JSContext*, ICIteratorClose_Fallback*, HandleValue);
static const VMFunction DoIteratorCloseFallbackInfo =
    FunctionInfo<DoIteratorCloseFallbackFn>(DoIteratorCloseFallback, TailCall);

bool
ICIteratorClose_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    masm.pushValue(R0);
    masm.push(BaselineStubReg);

    return tailCallVM(DoIteratorCloseFallbackInfo, masm);
}

//
// InstanceOf_Fallback
//

static bool
TryAttachInstanceOfStub(JSContext* cx, BaselineFrame* frame, ICInstanceOf_Fallback* stub,
                        HandleFunction fun, bool* attached)
{
    MOZ_ASSERT(!*attached);
    if (fun->isBoundFunction())
        return true;

    Shape* shape = fun->lookupPure(cx->names().prototype);
    if (!shape || !shape->hasSlot() || !shape->hasDefaultGetter())
        return true;

    uint32_t slot = shape->slot();
    MOZ_ASSERT(fun->numFixedSlots() == 0, "Stub code relies on this");

    if (!fun->getSlot(slot).isObject())
        return true;

    JSObject* protoObject = &fun->getSlot(slot).toObject();

    JitSpew(JitSpew_BaselineIC, "  Generating InstanceOf(Function) stub");
    ICInstanceOf_Function::Compiler compiler(cx, fun->lastProperty(), protoObject, slot);
    ICStub* newStub = compiler.getStub(compiler.getStubSpace(frame->script()));
    if (!newStub)
        return false;

    stub->addNewStub(newStub);
    *attached = true;
    return true;
}

static bool
DoInstanceOfFallback(JSContext* cx, BaselineFrame* frame, ICInstanceOf_Fallback* stub,
                     HandleValue lhs, HandleValue rhs, MutableHandleValue res)
{
    FallbackICSpew(cx, stub, "InstanceOf");

    if (!rhs.isObject()) {
        js_ReportValueError(cx, JSMSG_BAD_INSTANCEOF_RHS, -1, rhs, NullPtr());
        return false;
    }

    RootedObject obj(cx, &rhs.toObject());
    bool cond = false;
    if (!HasInstance(cx, obj, lhs, &cond))
        return false;

    res.setBoolean(cond);

    if (!obj->is<JSFunction>()) {
        stub->noteUnoptimizableAccess();
        return true;
    }

    // For functions, keep track of the |prototype| property in type information,
    // for use during Ion compilation.
    EnsureTrackPropertyTypes(cx, obj, NameToId(cx->names().prototype));

    if (stub->numOptimizedStubs() >= ICInstanceOf_Fallback::MAX_OPTIMIZED_STUBS)
        return true;

    RootedFunction fun(cx, &obj->as<JSFunction>());
    bool attached = false;
    if (!TryAttachInstanceOfStub(cx, frame, stub, fun, &attached))
        return false;
    if (!attached)
        stub->noteUnoptimizableAccess();
    return true;
}

typedef bool (*DoInstanceOfFallbackFn)(JSContext*, BaselineFrame*, ICInstanceOf_Fallback*,
                                       HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoInstanceOfFallbackInfo =
    FunctionInfo<DoInstanceOfFallbackFn>(DoInstanceOfFallback, TailCall, PopValues(2));

bool
ICInstanceOf_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    // Sync stack for the decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoInstanceOfFallbackInfo, masm);
}

bool
ICInstanceOf_Function::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;

    // Ensure RHS is an object.
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);
    Register rhsObj = masm.extractObject(R1, ExtractTemp0);

    // Allow using R1's type register as scratch. We have to restore it when
    // we want to jump to the next stub.
    Label failureRestoreR1;
    GeneralRegisterSet regs(availableGeneralRegs(1));
    regs.takeUnchecked(rhsObj);

    Register scratch1 = regs.takeAny();
    Register scratch2 = regs.takeAny();

    // Shape guard.
    masm.loadPtr(Address(BaselineStubReg, ICInstanceOf_Function::offsetOfShape()), scratch1);
    masm.branchTestObjShape(Assembler::NotEqual, rhsObj, scratch1, &failureRestoreR1);

    // Guard on the .prototype object.
    masm.loadPtr(Address(rhsObj, NativeObject::offsetOfSlots()), scratch1);
    masm.load32(Address(BaselineStubReg, ICInstanceOf_Function::offsetOfSlot()), scratch2);
    BaseValueIndex prototypeSlot(scratch1, scratch2);
    masm.branchTestObject(Assembler::NotEqual, prototypeSlot, &failureRestoreR1);
    masm.unboxObject(prototypeSlot, scratch1);
    masm.branchPtr(Assembler::NotEqual,
                   Address(BaselineStubReg, ICInstanceOf_Function::offsetOfPrototypeObject()),
                   scratch1, &failureRestoreR1);

    // If LHS is a primitive, return false.
    Label returnFalse, returnTrue;
    masm.branchTestObject(Assembler::NotEqual, R0, &returnFalse);

    // LHS is an object. Load its proto.
    masm.unboxObject(R0, scratch2);
    masm.loadObjProto(scratch2, scratch2);

    {
        // Walk the proto chain until we either reach the target object,
        // nullptr or LazyProto.
        Label loop;
        masm.bind(&loop);

        masm.branchPtr(Assembler::Equal, scratch2, scratch1, &returnTrue);
        masm.branchTestPtr(Assembler::Zero, scratch2, scratch2, &returnFalse);

        MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);
        masm.branchPtr(Assembler::Equal, scratch2, ImmWord(1), &failureRestoreR1);

        masm.loadObjProto(scratch2, scratch2);
        masm.jump(&loop);
    }

    EmitReturnFromIC(masm);

    masm.bind(&returnFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    masm.bind(&returnTrue);
    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    masm.bind(&failureRestoreR1);
    masm.tagValue(JSVAL_TYPE_OBJECT, rhsObj, R1);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// TypeOf_Fallback
//

static bool
DoTypeOfFallback(JSContext* cx, BaselineFrame* frame, ICTypeOf_Fallback* stub, HandleValue val,
                 MutableHandleValue res)
{
    FallbackICSpew(cx, stub, "TypeOf");
    JSType type = js::TypeOfValue(val);
    RootedString string(cx, TypeName(type, cx->names()));

    res.setString(string);

    MOZ_ASSERT(type != JSTYPE_NULL);
    if (type != JSTYPE_OBJECT && type != JSTYPE_FUNCTION) {
        // Create a new TypeOf stub.
        JitSpew(JitSpew_BaselineIC, "  Generating TypeOf stub for JSType (%d)", (int) type);
        ICTypeOf_Typed::Compiler compiler(cx, type, string);
        ICStub* typeOfStub = compiler.getStub(compiler.getStubSpace(frame->script()));
        if (!typeOfStub)
            return false;
        stub->addNewStub(typeOfStub);
    }

    return true;
}

typedef bool (*DoTypeOfFallbackFn)(JSContext*, BaselineFrame* frame, ICTypeOf_Fallback*,
                                   HandleValue, MutableHandleValue);
static const VMFunction DoTypeOfFallbackInfo =
    FunctionInfo<DoTypeOfFallbackFn>(DoTypeOfFallback, TailCall);

bool
ICTypeOf_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    masm.pushValue(R0);
    masm.push(BaselineStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoTypeOfFallbackInfo, masm);
}

bool
ICTypeOf_Typed::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(type_ != JSTYPE_NULL);
    MOZ_ASSERT(type_ != JSTYPE_FUNCTION);
    MOZ_ASSERT(type_ != JSTYPE_OBJECT);

    Label failure;
    switch(type_) {
      case JSTYPE_VOID:
        masm.branchTestUndefined(Assembler::NotEqual, R0, &failure);
        break;

      case JSTYPE_STRING:
        masm.branchTestString(Assembler::NotEqual, R0, &failure);
        break;

      case JSTYPE_NUMBER:
        masm.branchTestNumber(Assembler::NotEqual, R0, &failure);
        break;

      case JSTYPE_BOOLEAN:
        masm.branchTestBoolean(Assembler::NotEqual, R0, &failure);
        break;

      case JSTYPE_SYMBOL:
        masm.branchTestSymbol(Assembler::NotEqual, R0, &failure);
        break;

      default:
        MOZ_CRASH("Unexpected type");
    }

    masm.movePtr(ImmGCPtr(typeString_), R0.scratchReg());
    masm.tagValue(JSVAL_TYPE_STRING, R0.scratchReg(), R0);
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

static bool
DoRetSubFallback(JSContext* cx, BaselineFrame* frame, ICRetSub_Fallback* stub,
                 HandleValue val, uint8_t** resumeAddr)
{
    FallbackICSpew(cx, stub, "RetSub");

    // |val| is the bytecode offset where we should resume.

    MOZ_ASSERT(val.isInt32());
    MOZ_ASSERT(val.toInt32() >= 0);

    JSScript* script = frame->script();
    uint32_t offset = uint32_t(val.toInt32());

    *resumeAddr = script->baselineScript()->nativeCodeForPC(script, script->offsetToPC(offset));

    if (stub->numOptimizedStubs() >= ICRetSub_Fallback::MAX_OPTIMIZED_STUBS)
        return true;

    // Attach an optimized stub for this pc offset.
    JitSpew(JitSpew_BaselineIC, "  Generating RetSub stub for pc offset %u", offset);
    ICRetSub_Resume::Compiler compiler(cx, offset, *resumeAddr);
    ICStub* optStub = compiler.getStub(compiler.getStubSpace(script));
    if (!optStub)
        return false;

    stub->addNewStub(optStub);
    return true;
}

typedef bool(*DoRetSubFallbackFn)(JSContext* cx, BaselineFrame*, ICRetSub_Fallback*,
                                  HandleValue, uint8_t**);
static const VMFunction DoRetSubFallbackInfo = FunctionInfo<DoRetSubFallbackFn>(DoRetSubFallback);

typedef bool (*ThrowFn)(JSContext*, HandleValue);
static const VMFunction ThrowInfoBaseline = FunctionInfo<ThrowFn>(js::Throw, TailCall);

bool
ICRetSub_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    // If R0 is BooleanValue(true), rethrow R1.
    Label rethrow;
    masm.branchTestBooleanTruthy(true, R0, &rethrow);
    {
        // Call a stub to get the native code address for the pc offset in R1.
        GeneralRegisterSet regs(availableGeneralRegs(0));
        regs.take(R1);
        regs.takeUnchecked(BaselineTailCallReg);

        Register frame = regs.takeAny();
        masm.movePtr(BaselineFrameReg, frame);

        enterStubFrame(masm, regs.getAny());

        masm.pushValue(R1);
        masm.push(BaselineStubReg);
        masm.pushBaselineFramePtr(frame, frame);

        if (!callVM(DoRetSubFallbackInfo, masm))
            return false;

        leaveStubFrame(masm);

        EmitChangeICReturnAddress(masm, ReturnReg);
        EmitReturnFromIC(masm);
    }

    masm.bind(&rethrow);
    EmitRestoreTailCallReg(masm);
    masm.pushValue(R1);
    return tailCallVM(ThrowInfoBaseline, masm);
}

bool
ICRetSub_Resume::Compiler::generateStubCode(MacroAssembler& masm)
{
    // If R0 is BooleanValue(true), rethrow R1.
    Label fail, rethrow;
    masm.branchTestBooleanTruthy(true, R0, &rethrow);

    // R1 is the pc offset. Ensure it matches this stub's offset.
    Register offset = masm.extractInt32(R1, ExtractTemp0);
    masm.branch32(Assembler::NotEqual,
                  Address(BaselineStubReg, ICRetSub_Resume::offsetOfPCOffset()),
                  offset,
                  &fail);

    // pc offset matches, resume at the target pc.
    masm.loadPtr(Address(BaselineStubReg, ICRetSub_Resume::offsetOfAddr()), R0.scratchReg());
    EmitChangeICReturnAddress(masm, R0.scratchReg());
    EmitReturnFromIC(masm);

    // Rethrow the Value stored in R1.
    masm.bind(&rethrow);
    EmitRestoreTailCallReg(masm);
    masm.pushValue(R1);
    if (!tailCallVM(ThrowInfoBaseline, masm))
        return false;

    masm.bind(&fail);
    EmitStubGuardFailure(masm);
    return true;
}

ICTypeMonitor_SingleObject::ICTypeMonitor_SingleObject(JitCode* stubCode, JSObject* obj)
  : ICStub(TypeMonitor_SingleObject, stubCode),
    obj_(obj)
{ }

ICTypeMonitor_ObjectGroup::ICTypeMonitor_ObjectGroup(JitCode* stubCode, ObjectGroup* group)
  : ICStub(TypeMonitor_ObjectGroup, stubCode),
    group_(group)
{ }

ICTypeUpdate_SingleObject::ICTypeUpdate_SingleObject(JitCode* stubCode, JSObject* obj)
  : ICStub(TypeUpdate_SingleObject, stubCode),
    obj_(obj)
{ }

ICTypeUpdate_ObjectGroup::ICTypeUpdate_ObjectGroup(JitCode* stubCode, ObjectGroup* group)
  : ICStub(TypeUpdate_ObjectGroup, stubCode),
    group_(group)
{ }

ICGetElemNativeStub::ICGetElemNativeStub(ICStub::Kind kind, JitCode* stubCode,
                                         ICStub* firstMonitorStub,
                                         Shape* shape, PropertyName* name,
                                         AccessType acctype, bool needsAtomize)
  : ICMonitoredStub(kind, stubCode, firstMonitorStub),
    shape_(shape),
    name_(name)
{
    extra_ = (static_cast<uint16_t>(acctype) << ACCESSTYPE_SHIFT) |
             (static_cast<uint16_t>(needsAtomize) << NEEDS_ATOMIZE_SHIFT);
}

ICGetElemNativeStub::~ICGetElemNativeStub()
{ }

ICGetElemNativeGetterStub::ICGetElemNativeGetterStub(
                        ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                        Shape* shape, PropertyName* name, AccessType acctype,
                        bool needsAtomize, JSFunction* getter, uint32_t pcOffset)
  : ICGetElemNativeStub(kind, stubCode, firstMonitorStub, shape, name, acctype, needsAtomize),
    getter_(getter),
    pcOffset_(pcOffset)
{
    MOZ_ASSERT(kind == GetElem_NativePrototypeCallNative ||
               kind == GetElem_NativePrototypeCallScripted);
    MOZ_ASSERT(acctype == NativeGetter || acctype == ScriptedGetter);
}

ICGetElem_NativePrototypeSlot::ICGetElem_NativePrototypeSlot(
                            JitCode* stubCode, ICStub* firstMonitorStub,
                            Shape* shape, PropertyName* name,
                            AccessType acctype, bool needsAtomize, uint32_t offset,
                            JSObject* holder, Shape* holderShape)
  : ICGetElemNativeSlotStub(ICStub::GetElem_NativePrototypeSlot, stubCode, firstMonitorStub, shape,
                            name, acctype, needsAtomize, offset),
    holder_(holder),
    holderShape_(holderShape)
{ }

ICGetElemNativePrototypeCallStub::ICGetElemNativePrototypeCallStub(
                                ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                                Shape* shape, PropertyName* name,
                                AccessType acctype, bool needsAtomize, JSFunction* getter,
                                uint32_t pcOffset, JSObject* holder, Shape* holderShape)
  : ICGetElemNativeGetterStub(kind, stubCode, firstMonitorStub, shape, name, acctype, needsAtomize,
                              getter, pcOffset),
    holder_(holder),
    holderShape_(holderShape)
{}

/* static */ ICGetElem_NativePrototypeCallNative*
ICGetElem_NativePrototypeCallNative::Clone(ICStubSpace* space,
                                           ICStub* firstMonitorStub,
                                           ICGetElem_NativePrototypeCallNative& other)
{
    return New<ICGetElem_NativePrototypeCallNative>(space, other.jitCode(), firstMonitorStub,
                                                    other.shape(), other.name(), other.accessType(),
                                                    other.needsAtomize(), other.getter(), other.pcOffset_,
                                                    other.holder(), other.holderShape());
}

/* static */ ICGetElem_NativePrototypeCallScripted*
ICGetElem_NativePrototypeCallScripted::Clone(ICStubSpace* space,
                                             ICStub* firstMonitorStub,
                                             ICGetElem_NativePrototypeCallScripted& other)
{
    return New<ICGetElem_NativePrototypeCallScripted>(space, other.jitCode(), firstMonitorStub,
                                                      other.shape(), other.name(),
                                                      other.accessType(), other.needsAtomize(), other.getter(),
                                                      other.pcOffset_, other.holder(), other.holderShape());
}

ICGetElem_Dense::ICGetElem_Dense(JitCode* stubCode, ICStub* firstMonitorStub, Shape* shape)
    : ICMonitoredStub(GetElem_Dense, stubCode, firstMonitorStub),
      shape_(shape)
{ }

/* static */ ICGetElem_Dense*
ICGetElem_Dense::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                       ICGetElem_Dense& other)
{
    return New<ICGetElem_Dense>(space, other.jitCode(), firstMonitorStub, other.shape_);
}

ICGetElem_TypedArray::ICGetElem_TypedArray(JitCode* stubCode, Shape* shape, Scalar::Type type)
  : ICStub(GetElem_TypedArray, stubCode),
    shape_(shape)
{
    extra_ = uint16_t(type);
    MOZ_ASSERT(extra_ == type);
}

/* static */ ICGetElem_Arguments*
ICGetElem_Arguments::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                           ICGetElem_Arguments& other)
{
    return New<ICGetElem_Arguments>(space, other.jitCode(), firstMonitorStub, other.which());
}

ICSetElem_Dense::ICSetElem_Dense(JitCode* stubCode, Shape* shape, ObjectGroup* group)
  : ICUpdatedStub(SetElem_Dense, stubCode),
    shape_(shape),
    group_(group)
{ }

ICSetElem_DenseAdd::ICSetElem_DenseAdd(JitCode* stubCode, ObjectGroup* group,
                                       size_t protoChainDepth)
  : ICUpdatedStub(SetElem_DenseAdd, stubCode),
    group_(group)
{
    MOZ_ASSERT(protoChainDepth <= MAX_PROTO_CHAIN_DEPTH);
    extra_ = protoChainDepth;
}

template <size_t ProtoChainDepth>
ICUpdatedStub*
ICSetElemDenseAddCompiler::getStubSpecific(ICStubSpace* space, const AutoShapeVector* shapes)
{
    RootedObjectGroup group(cx, obj_->getGroup(cx));
    if (!group)
        return nullptr;
    Rooted<JitCode*> stubCode(cx, getStubCode());
    return ICStub::New<ICSetElem_DenseAddImpl<ProtoChainDepth>>(space, stubCode, group, shapes);
}

ICSetElem_TypedArray::ICSetElem_TypedArray(JitCode* stubCode, Shape* shape, Scalar::Type type,
                                           bool expectOutOfBounds)
  : ICStub(SetElem_TypedArray, stubCode),
    shape_(shape)
{
    extra_ = uint8_t(type);
    MOZ_ASSERT(extra_ == type);
    extra_ |= (static_cast<uint16_t>(expectOutOfBounds) << 8);
}

ICGetName_Global::ICGetName_Global(JitCode* stubCode, ICStub* firstMonitorStub, Shape* shape,
                                   uint32_t slot)
  : ICMonitoredStub(GetName_Global, stubCode, firstMonitorStub),
    shape_(shape),
    slot_(slot)
{ }

template <size_t NumHops>
ICGetName_Scope<NumHops>::ICGetName_Scope(JitCode* stubCode, ICStub* firstMonitorStub,
                                          AutoShapeVector* shapes, uint32_t offset)
  : ICMonitoredStub(GetStubKind(), stubCode, firstMonitorStub),
    offset_(offset)
{
    JS_STATIC_ASSERT(NumHops <= MAX_HOPS);
    MOZ_ASSERT(shapes->length() == NumHops + 1);
    for (size_t i = 0; i < NumHops + 1; i++)
        shapes_[i].init((*shapes)[i]);
}

ICGetIntrinsic_Constant::ICGetIntrinsic_Constant(JitCode* stubCode, const Value& value)
  : ICStub(GetIntrinsic_Constant, stubCode),
    value_(value)
{ }

ICGetIntrinsic_Constant::~ICGetIntrinsic_Constant()
{ }

ICGetProp_Primitive::ICGetProp_Primitive(JitCode* stubCode, ICStub* firstMonitorStub,
                                         Shape* protoShape, uint32_t offset)
  : ICMonitoredStub(GetProp_Primitive, stubCode, firstMonitorStub),
    protoShape_(protoShape),
    offset_(offset)
{ }

ICGetPropNativeStub::ICGetPropNativeStub(ICStub::Kind kind, JitCode* stubCode,
                                         ICStub* firstMonitorStub, uint32_t offset)
  : ICMonitoredStub(kind, stubCode, firstMonitorStub),
    offset_(offset)
{ }

/* static */ ICGetProp_Native*
ICGetProp_Native::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                        ICGetProp_Native& other)
{
    return New<ICGetProp_Native>(space, other.jitCode(), firstMonitorStub, other.shape(),
                                 other.offset());
}

ICGetProp_NativePrototype::ICGetProp_NativePrototype(JitCode* stubCode, ICStub* firstMonitorStub,
                                                     Shape* shape, uint32_t offset,
                                                     JSObject* holder, Shape* holderShape)
  : ICGetPropNativeStub(GetProp_NativePrototype, stubCode, firstMonitorStub, offset),
    shape_(shape),
    holder_(holder),
    holderShape_(holderShape)
{ }

/* static */ ICGetProp_NativePrototype*
ICGetProp_NativePrototype::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                                 ICGetProp_NativePrototype& other)
{
    return New<ICGetProp_NativePrototype>(space, other.jitCode(), firstMonitorStub, other.shape(),
                                          other.offset(), other.holder_, other.holderShape_);
}

ICGetProp_UnboxedPrototype::ICGetProp_UnboxedPrototype(JitCode* stubCode, ICStub* firstMonitorStub,
                                                       ObjectGroup* group, uint32_t offset,
                                                       JSObject* holder, Shape* holderShape)
  : ICGetPropNativeStub(GetProp_UnboxedPrototype, stubCode, firstMonitorStub, offset),
    group_(group),
    holder_(holder),
    holderShape_(holderShape)
{ }

/* static */ ICGetProp_UnboxedPrototype*
ICGetProp_UnboxedPrototype::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                                  ICGetProp_UnboxedPrototype& other)
{
    return New<ICGetProp_UnboxedPrototype>(space, other.jitCode(), firstMonitorStub, other.group(),
                                           other.offset(), other.holder_, other.holderShape_);
}

ICGetProp_NativeDoesNotExist::ICGetProp_NativeDoesNotExist(
        JitCode* stubCode, ICStub* firstMonitorStub, size_t protoChainDepth)
  : ICMonitoredStub(GetProp_NativeDoesNotExist, stubCode, firstMonitorStub)
{
    MOZ_ASSERT(protoChainDepth <= MAX_PROTO_CHAIN_DEPTH);
    extra_ = protoChainDepth;
}

/* static */ size_t
ICGetProp_NativeDoesNotExist::offsetOfShape(size_t idx)
{
    MOZ_ASSERT(ICGetProp_NativeDoesNotExistImpl<0>::offsetOfShape(idx) ==
               ICGetProp_NativeDoesNotExistImpl<
                    ICGetProp_NativeDoesNotExist::MAX_PROTO_CHAIN_DEPTH>::offsetOfShape(idx));
    return ICGetProp_NativeDoesNotExistImpl<0>::offsetOfShape(idx);
}

template <size_t ProtoChainDepth>
ICGetProp_NativeDoesNotExistImpl<ProtoChainDepth>::ICGetProp_NativeDoesNotExistImpl(
        JitCode* stubCode, ICStub* firstMonitorStub, const AutoShapeVector* shapes)
  : ICGetProp_NativeDoesNotExist(stubCode, firstMonitorStub, ProtoChainDepth)
{
    MOZ_ASSERT(shapes->length() == NumShapes);
    for (size_t i = 0; i < NumShapes; i++)
        shapes_[i].init((*shapes)[i]);
}

ICGetPropNativeDoesNotExistCompiler::ICGetPropNativeDoesNotExistCompiler(
        JSContext* cx, ICStub* firstMonitorStub, HandleObject obj, size_t protoChainDepth)
  : ICStubCompiler(cx, ICStub::GetProp_NativeDoesNotExist),
    firstMonitorStub_(firstMonitorStub),
    obj_(cx, obj),
    protoChainDepth_(protoChainDepth)
{
    MOZ_ASSERT(protoChainDepth_ <= ICGetProp_NativeDoesNotExist::MAX_PROTO_CHAIN_DEPTH);
}

ICGetPropCallGetter::ICGetPropCallGetter(Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                                         JSObject* holder, Shape* holderShape, JSFunction* getter,
                                         uint32_t pcOffset)
  : ICMonitoredStub(kind, stubCode, firstMonitorStub),
    holder_(holder),
    holderShape_(holderShape),
    getter_(getter),
    pcOffset_(pcOffset)
{
    MOZ_ASSERT(kind == ICStub::GetProp_CallScripted  ||
               kind == ICStub::GetProp_CallNative    ||
               kind == ICStub::GetProp_CallNativePrototype ||
               kind == ICStub::GetProp_CallDOMProxyNative ||
               kind == ICStub::GetProp_CallDOMProxyWithGenerationNative);
}

ICGetPropCallPrototypeGetter::ICGetPropCallPrototypeGetter(Kind kind, JitCode* stubCode,
                                                           ICStub* firstMonitorStub,
                                                           Shape* receiverShape, JSObject* holder,
                                                           Shape* holderShape,
                                                           JSFunction* getter, uint32_t pcOffset)
  : ICGetPropCallGetter(kind, stubCode, firstMonitorStub, holder, holderShape, getter, pcOffset),
    receiverShape_(receiverShape)
{
    MOZ_ASSERT(kind == ICStub::GetProp_CallScripted || kind == ICStub::GetProp_CallNativePrototype);
}

ICInstanceOf_Function::ICInstanceOf_Function(JitCode* stubCode, Shape* shape,
                                             JSObject* prototypeObj, uint32_t slot)
  : ICStub(InstanceOf_Function, stubCode),
    shape_(shape),
    prototypeObj_(prototypeObj),
    slot_(slot)
{ }

/* static */ ICGetProp_CallScripted*
ICGetProp_CallScripted::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                              ICGetProp_CallScripted& other)
{
    return New<ICGetProp_CallScripted>(space, other.jitCode(), firstMonitorStub,
                                       other.receiverShape_, other.holder_, other.holderShape_,
                                       other.getter_, other.pcOffset_);
}

/* static */ ICGetProp_CallNative*
ICGetProp_CallNative::Clone( ICStubSpace* space, ICStub* firstMonitorStub,
                            ICGetProp_CallNative& other)
{
    return New<ICGetProp_CallNative>(space, other.jitCode(), firstMonitorStub, other.holder_,
                                     other.holderShape_, other.getter_, other.pcOffset_);
}

/* static */ ICGetProp_CallNativePrototype*
ICGetProp_CallNativePrototype::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                                     ICGetProp_CallNativePrototype& other)
{
    return New<ICGetProp_CallNativePrototype>(space, other.jitCode(), firstMonitorStub,
                                              other.receiverShape_, other.holder_,
                                              other.holderShape_, other.getter_, other.pcOffset_);
}

ICSetProp_Native::ICSetProp_Native(JitCode* stubCode, ObjectGroup* group, Shape* shape,
                                   uint32_t offset)
  : ICUpdatedStub(SetProp_Native, stubCode),
    group_(group),
    shape_(shape),
    offset_(offset)
{ }

ICSetProp_Native*
ICSetProp_Native::Compiler::getStub(ICStubSpace* space)
{
    RootedObjectGroup group(cx, obj_->getGroup(cx));
    if (!group)
        return nullptr;

    RootedShape shape(cx, obj_->lastProperty());
    ICSetProp_Native* stub = ICStub::New<ICSetProp_Native>(space, getStubCode(), group, shape, offset_);
    if (!stub || !stub->initUpdatingChain(cx, space))
        return nullptr;
    return stub;
}

ICSetProp_NativeAdd::ICSetProp_NativeAdd(JitCode* stubCode, ObjectGroup* group,
                                         size_t protoChainDepth,
                                         Shape* newShape,
                                         ObjectGroup* newGroup,
                                         uint32_t offset)
  : ICUpdatedStub(SetProp_NativeAdd, stubCode),
    group_(group),
    newShape_(newShape),
    newGroup_(newGroup),
    offset_(offset)
{
    MOZ_ASSERT(protoChainDepth <= MAX_PROTO_CHAIN_DEPTH);
    extra_ = protoChainDepth;
}

template <size_t ProtoChainDepth>
ICSetProp_NativeAddImpl<ProtoChainDepth>::ICSetProp_NativeAddImpl(JitCode* stubCode,
                                                                  ObjectGroup* group,
                                                                  const AutoShapeVector* shapes,
                                                                  Shape* newShape,
                                                                  ObjectGroup* newGroup,
                                                                  uint32_t offset)
  : ICSetProp_NativeAdd(stubCode, group, ProtoChainDepth, newShape, newGroup, offset)
{
    MOZ_ASSERT(shapes->length() == NumShapes);
    for (size_t i = 0; i < NumShapes; i++)
        shapes_[i].init((*shapes)[i]);
}

ICSetPropNativeAddCompiler::ICSetPropNativeAddCompiler(JSContext* cx, HandleObject obj,
                                                       HandleShape oldShape,
                                                       HandleObjectGroup oldGroup,
                                                       size_t protoChainDepth,
                                                       bool isFixedSlot,
                                                       uint32_t offset)
  : ICStubCompiler(cx, ICStub::SetProp_NativeAdd),
    obj_(cx, obj),
    oldShape_(cx, oldShape),
    oldGroup_(cx, oldGroup),
    protoChainDepth_(protoChainDepth),
    isFixedSlot_(isFixedSlot),
    offset_(offset)
{
    MOZ_ASSERT(protoChainDepth_ <= ICSetProp_NativeAdd::MAX_PROTO_CHAIN_DEPTH);
}

ICSetPropCallSetter::ICSetPropCallSetter(Kind kind, JitCode* stubCode, Shape* shape,
                                         JSObject* holder, Shape* holderShape,
                                         JSFunction* setter, uint32_t pcOffset)
  : ICStub(kind, stubCode),
    shape_(shape),
    holder_(holder),
    holderShape_(holderShape),
    setter_(setter),
    pcOffset_(pcOffset)
{
    MOZ_ASSERT(kind == ICStub::SetProp_CallScripted || kind == ICStub::SetProp_CallNative);
}

/* static */ ICSetProp_CallScripted*
ICSetProp_CallScripted::Clone(ICStubSpace* space, ICStub*, ICSetProp_CallScripted& other)
{
    return New<ICSetProp_CallScripted>(space, other.jitCode(), other.shape_, other.holder_,
                                       other.holderShape_, other.setter_, other.pcOffset_);
}

/* static */ ICSetProp_CallNative*
ICSetProp_CallNative::Clone(ICStubSpace* space, ICStub*, ICSetProp_CallNative& other)
{
    return New<ICSetProp_CallNative>(space, other.jitCode(), other.shape_, other.holder_,
                                     other.holderShape_, other.setter_, other.pcOffset_);
}

ICCall_Scripted::ICCall_Scripted(JitCode* stubCode, ICStub* firstMonitorStub,
                                 JSFunction* callee, JSObject* templateObject,
                                 uint32_t pcOffset)
  : ICMonitoredStub(ICStub::Call_Scripted, stubCode, firstMonitorStub),
    callee_(callee),
    templateObject_(templateObject),
    pcOffset_(pcOffset)
{ }

/* static */ ICCall_Scripted*
ICCall_Scripted::Clone(ICStubSpace* space, ICStub* firstMonitorStub, ICCall_Scripted& other)
{
    return New<ICCall_Scripted>(space, other.jitCode(), firstMonitorStub, other.callee_,
                                other.templateObject_, other.pcOffset_);
}

/* static */ ICCall_AnyScripted*
ICCall_AnyScripted::Clone(ICStubSpace* space, ICStub* firstMonitorStub, ICCall_AnyScripted& other)
{
    return New<ICCall_AnyScripted>(space, other.jitCode(), firstMonitorStub, other.pcOffset_);
}

ICCall_Native::ICCall_Native(JitCode* stubCode, ICStub* firstMonitorStub,
                             JSFunction* callee, JSObject* templateObject,
                             uint32_t pcOffset)
  : ICMonitoredStub(ICStub::Call_Native, stubCode, firstMonitorStub),
    callee_(callee),
    templateObject_(templateObject),
    pcOffset_(pcOffset)
{
#if defined(JS_ARM_SIMULATOR) || defined(JS_MIPS_SIMULATOR)
    // The simulator requires VM calls to be redirected to a special swi
    // instruction to handle them. To make this work, we store the redirected
    // pointer in the stub.
    native_ = Simulator::RedirectNativeFunction(JS_FUNC_TO_DATA_PTR(void*, callee->native()),
                                                Args_General3);
#endif
}

/* static */ ICCall_Native*
ICCall_Native::Clone(ICStubSpace* space, ICStub* firstMonitorStub, ICCall_Native& other)
{
    return New<ICCall_Native>(space, other.jitCode(), firstMonitorStub, other.callee_,
                              other.templateObject_, other.pcOffset_);
}

ICCall_ClassHook::ICCall_ClassHook(JitCode* stubCode, ICStub* firstMonitorStub,
                                   const Class* clasp, Native native,
                                   JSObject* templateObject, uint32_t pcOffset)
  : ICMonitoredStub(ICStub::Call_ClassHook, stubCode, firstMonitorStub),
    clasp_(clasp),
    native_(JS_FUNC_TO_DATA_PTR(void*, native)),
    templateObject_(templateObject),
    pcOffset_(pcOffset)
{
#if defined(JS_ARM_SIMULATOR) || defined(JS_MIPS_SIMULATOR)
    // The simulator requires VM calls to be redirected to a special swi
    // instruction to handle them. To make this work, we store the redirected
    // pointer in the stub.
    native_ = Simulator::RedirectNativeFunction(native_, Args_General3);
#endif
}

/* static */ ICCall_ClassHook*
ICCall_ClassHook::Clone(ICStubSpace* space, ICStub* firstMonitorStub, ICCall_ClassHook& other)
{
    ICCall_ClassHook* res = New<ICCall_ClassHook>(space, other.jitCode(), firstMonitorStub,
                                                  other.clasp(), nullptr, other.templateObject_,
                                                  other.pcOffset_);
    if (res)
        res->native_ = other.native();
    return res;
}

/* static */ ICCall_ScriptedApplyArray*
ICCall_ScriptedApplyArray::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                                 ICCall_ScriptedApplyArray& other)
{
    return New<ICCall_ScriptedApplyArray>(space, other.jitCode(), firstMonitorStub,
                                          other.pcOffset_);
}

/* static */ ICCall_ScriptedApplyArguments*
ICCall_ScriptedApplyArguments::Clone(ICStubSpace* space,
                                     ICStub* firstMonitorStub,
                                     ICCall_ScriptedApplyArguments& other)
{
    return New<ICCall_ScriptedApplyArguments>(space, other.jitCode(), firstMonitorStub,
                                              other.pcOffset_);
}

/* static */ ICCall_ScriptedFunCall*
ICCall_ScriptedFunCall::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                              ICCall_ScriptedFunCall& other)
{
    return New<ICCall_ScriptedFunCall>(space, other.jitCode(), firstMonitorStub, other.pcOffset_);
}

ICGetPropCallDOMProxyNativeStub::ICGetPropCallDOMProxyNativeStub(Kind kind, JitCode* stubCode,
                                                                 ICStub* firstMonitorStub,
                                                                 Shape* shape,
                                                                 Shape* expandoShape,
                                                                 JSObject* holder,
                                                                 Shape* holderShape,
                                                                 JSFunction* getter,
                                                                 uint32_t pcOffset)
: ICGetPropCallGetter(kind, stubCode, firstMonitorStub, holder, holderShape,
                      getter, pcOffset),
    shape_(shape),
    expandoShape_(expandoShape)
{ }

ICGetPropCallDOMProxyNativeCompiler::ICGetPropCallDOMProxyNativeCompiler(JSContext* cx,
                                                                         ICStub::Kind kind,
                                                                         ICStub* firstMonitorStub,
                                                                         Handle<ProxyObject*> proxy,
                                                                         HandleObject holder,
                                                                         HandleFunction getter,
                                                                         uint32_t pcOffset)
  : ICStubCompiler(cx, kind),
    firstMonitorStub_(firstMonitorStub),
    proxy_(cx, proxy),
    holder_(cx, holder),
    getter_(cx, getter),
    pcOffset_(pcOffset)
{
    MOZ_ASSERT(kind == ICStub::GetProp_CallDOMProxyNative ||
               kind == ICStub::GetProp_CallDOMProxyWithGenerationNative);
    MOZ_ASSERT(proxy_->handler()->family() == GetDOMProxyHandlerFamily());
}

/* static */ ICGetProp_CallDOMProxyNative*
ICGetProp_CallDOMProxyNative::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                                    ICGetProp_CallDOMProxyNative& other)
{
    return New<ICGetProp_CallDOMProxyNative>(space, other.jitCode(), firstMonitorStub,
                                             other.shape_, other.expandoShape_,
                                             other.holder_, other.holderShape_, other.getter_,
                                             other.pcOffset_);
}

/* static */ ICGetProp_CallDOMProxyWithGenerationNative*
ICGetProp_CallDOMProxyWithGenerationNative::Clone(ICStubSpace* space,
                                                  ICStub* firstMonitorStub,
                                                  ICGetProp_CallDOMProxyWithGenerationNative& other)
{
    return New<ICGetProp_CallDOMProxyWithGenerationNative>(space, other.jitCode(), firstMonitorStub,
                                                           other.shape_,
                                                           other.expandoAndGeneration_,
                                                           other.generation_,
                                                           other.expandoShape_, other.holder_,
                                                           other.holderShape_, other.getter_,
                                                           other.pcOffset_);
}

ICGetProp_DOMProxyShadowed::ICGetProp_DOMProxyShadowed(JitCode* stubCode,
                                                       ICStub* firstMonitorStub,
                                                       Shape* shape,
                                                       const BaseProxyHandler* proxyHandler,
                                                       PropertyName* name,
                                                       uint32_t pcOffset)
  : ICMonitoredStub(ICStub::GetProp_DOMProxyShadowed, stubCode, firstMonitorStub),
    shape_(shape),
    proxyHandler_(proxyHandler),
    name_(name),
    pcOffset_(pcOffset)
{ }

/* static */ ICGetProp_DOMProxyShadowed*
ICGetProp_DOMProxyShadowed::Clone(ICStubSpace* space, ICStub* firstMonitorStub,
                                  ICGetProp_DOMProxyShadowed& other)
{
    return New<ICGetProp_DOMProxyShadowed>(space, other.jitCode(), firstMonitorStub, other.shape_,
                                           other.proxyHandler_, other.name_, other.pcOffset_);
}

//
// Rest_Fallback
//

static bool DoRestFallback(JSContext* cx, ICRest_Fallback* stub,
                           BaselineFrame* frame, MutableHandleValue res)
{
    unsigned numFormals = frame->numFormalArgs() - 1;
    unsigned numActuals = frame->numActualArgs();
    unsigned numRest = numActuals > numFormals ? numActuals - numFormals : 0;
    Value* rest = frame->argv() + numFormals;

    ArrayObject* obj = NewDenseCopiedArray(cx, numRest, rest, NullPtr());
    if (!obj)
        return false;
    ObjectGroup::fixRestArgumentsGroup(cx, obj);
    res.setObject(*obj);
    return true;
}

typedef bool (*DoRestFallbackFn)(JSContext*, ICRest_Fallback*, BaselineFrame*,
                                 MutableHandleValue);
static const VMFunction DoRestFallbackInfo =
    FunctionInfo<DoRestFallbackFn>(DoRestFallback, TailCall);

bool
ICRest_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
    masm.push(BaselineStubReg);

    return tailCallVM(DoRestFallbackInfo, masm);
}

} // namespace jit
} // namespace js
