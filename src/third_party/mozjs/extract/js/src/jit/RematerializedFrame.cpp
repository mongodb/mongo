/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/RematerializedFrame.h"

#include "jit/JitFrames.h"
#include "vm/ArgumentsObject.h"
#include "vm/Debugger.h"

#include "jit/JitFrames-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace jit;

struct CopyValueToRematerializedFrame
{
    Value* slots;

    explicit CopyValueToRematerializedFrame(Value* slots)
      : slots(slots)
    { }

    void operator()(const Value& v) {
        *slots++ = v;
    }
};

RematerializedFrame::RematerializedFrame(JSContext* cx, uint8_t* top, unsigned numActualArgs,
                                         InlineFrameIterator& iter, MaybeReadFallback& fallback)
  : prevUpToDate_(false),
    isDebuggee_(iter.script()->isDebuggee()),
    isConstructing_(iter.isConstructing()),
    hasCachedSavedFrame_(false),
    top_(top),
    pc_(iter.pc()),
    frameNo_(iter.frameNo()),
    numActualArgs_(numActualArgs),
    script_(iter.script())
{
    if (iter.isFunctionFrame())
        callee_ = iter.callee(fallback);
    else
        callee_ = nullptr;

    CopyValueToRematerializedFrame op(slots_);
    iter.readFrameArgsAndLocals(cx, op, op, &envChain_, &hasInitialEnv_, &returnValue_,
                                &argsObj_, &thisArgument_, &newTarget_, ReadFrame_Actuals,
                                fallback);
}

/* static */ RematerializedFrame*
RematerializedFrame::New(JSContext* cx, uint8_t* top, InlineFrameIterator& iter,
                         MaybeReadFallback& fallback)
{
    unsigned numFormals = iter.isFunctionFrame() ? iter.calleeTemplate()->nargs() : 0;
    unsigned argSlots = Max(numFormals, iter.numActualArgs());
    unsigned extraSlots = argSlots + iter.script()->nfixed();

    // One Value slot is included in sizeof(RematerializedFrame), so we can
    // reduce the extra slot count by one.  However, if there are zero slot
    // allocations total, then reducing the slots by one will lead to
    // the memory allocation being smaller  than sizeof(RematerializedFrame).
    if (extraSlots > 0)
        extraSlots -= 1;

    size_t numBytes = sizeof(RematerializedFrame) + (extraSlots * sizeof(Value));
    MOZ_ASSERT(numBytes >= sizeof(RematerializedFrame));

    void* buf = cx->pod_calloc<uint8_t>(numBytes);
    if (!buf)
        return nullptr;

    return new (buf) RematerializedFrame(cx, top, iter.numActualArgs(), iter, fallback);
}

/* static */ bool
RematerializedFrame::RematerializeInlineFrames(JSContext* cx, uint8_t* top,
                                               InlineFrameIterator& iter,
                                               MaybeReadFallback& fallback,
                                               GCVector<RematerializedFrame*>& frames)
{
    Rooted<GCVector<RematerializedFrame*>> tempFrames(cx, GCVector<RematerializedFrame*>(cx));
    if (!tempFrames.resize(iter.frameCount()))
        return false;

    while (true) {
        size_t frameNo = iter.frameNo();
        tempFrames[frameNo].set(RematerializedFrame::New(cx, top, iter, fallback));
        if (!tempFrames[frameNo])
            return false;
        if (tempFrames[frameNo]->environmentChain()) {
            if (!EnsureHasEnvironmentObjects(cx, tempFrames[frameNo].get()))
                return false;
        }

        if (!iter.more())
            break;
        ++iter;
    }

    frames = Move(tempFrames.get());
    return true;
}

/* static */ void
RematerializedFrame::FreeInVector(GCVector<RematerializedFrame*>& frames)
{
    for (size_t i = 0; i < frames.length(); i++) {
        RematerializedFrame* f = frames[i];
        MOZ_ASSERT(!Debugger::inFrameMaps(f));
        f->RematerializedFrame::~RematerializedFrame();
        js_free(f);
    }
    frames.clear();
}

CallObject&
RematerializedFrame::callObj() const
{
    MOZ_ASSERT(hasInitialEnvironment());

    JSObject* env = environmentChain();
    while (!env->is<CallObject>())
        env = env->enclosingEnvironment();
    return env->as<CallObject>();
}

bool
RematerializedFrame::initFunctionEnvironmentObjects(JSContext* cx)
{
    return js::InitFunctionEnvironmentObjects(cx, this);
}

bool
RematerializedFrame::pushVarEnvironment(JSContext* cx, HandleScope scope)
{
    return js::PushVarEnvironmentObject(cx, scope, this);
}

void
RematerializedFrame::trace(JSTracer* trc)
{
    TraceRoot(trc, &script_, "remat ion frame script");
    TraceRoot(trc, &envChain_, "remat ion frame env chain");
    if (callee_)
        TraceRoot(trc, &callee_, "remat ion frame callee");
    if (argsObj_)
        TraceRoot(trc, &argsObj_, "remat ion frame argsobj");
    TraceRoot(trc, &returnValue_, "remat ion frame return value");
    TraceRoot(trc, &thisArgument_, "remat ion frame this");
    TraceRoot(trc, &newTarget_, "remat ion frame newTarget");
    TraceRootRange(trc, numArgSlots() + script_->nfixed(), slots_, "remat ion frame stack");
}

void
RematerializedFrame::dump()
{
    fprintf(stderr, " Rematerialized Ion Frame%s\n", inlined() ? " (inlined)" : "");
    if (isFunctionFrame()) {
        fprintf(stderr, "  callee fun: ");
#ifdef DEBUG
        DumpValue(ObjectValue(*callee()));
#else
        fprintf(stderr, "?\n");
#endif
    } else {
        fprintf(stderr, "  global frame, no callee\n");
    }

    fprintf(stderr, "  file %s line %zu offset %zu\n",
            script()->filename(), script()->lineno(),
            script()->pcToOffset(pc()));

    fprintf(stderr, "  script = %p\n", (void*) script());

    if (isFunctionFrame()) {
        fprintf(stderr, "  env chain: ");
#ifdef DEBUG
        DumpValue(ObjectValue(*environmentChain()));
#else
        fprintf(stderr, "?\n");
#endif

        if (hasArgsObj()) {
            fprintf(stderr, "  args obj: ");
#ifdef DEBUG
            DumpValue(ObjectValue(argsObj()));
#else
            fprintf(stderr, "?\n");
#endif
        }

        fprintf(stderr, "  this: ");
#ifdef DEBUG
        DumpValue(thisArgument());
#else
        fprintf(stderr, "?\n");
#endif

        for (unsigned i = 0; i < numActualArgs(); i++) {
            if (i < numFormalArgs())
                fprintf(stderr, "  formal (arg %d): ", i);
            else
                fprintf(stderr, "  overflown (arg %d): ", i);
#ifdef DEBUG
            DumpValue(argv()[i]);
#else
            fprintf(stderr, "?\n");
#endif
        }

        for (unsigned i = 0; i < script()->nfixed(); i++) {
            fprintf(stderr, "  local %d: ", i);
#ifdef DEBUG
            DumpValue(locals()[i]);
#else
            fprintf(stderr, "?\n");
#endif
        }
    }

    fputc('\n', stderr);
}
