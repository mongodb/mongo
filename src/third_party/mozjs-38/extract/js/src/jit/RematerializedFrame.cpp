/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/RematerializedFrame.h"

#include "jit/JitFrames.h"
#include "vm/ArgumentsObject.h"
#include "vm/Debugger.h"

#include "jsscriptinlines.h"
#include "jit/JitFrames-inl.h"

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
    top_(top),
    pc_(iter.pc()),
    frameNo_(iter.frameNo()),
    numActualArgs_(numActualArgs),
    script_(iter.script())
{
    CopyValueToRematerializedFrame op(slots_);
    iter.readFrameArgsAndLocals(cx, op, op, &scopeChain_, &hasCallObj_, &returnValue_,
                                &argsObj_, &thisValue_, ReadFrame_Actuals,
                                fallback);
}

/* static */ RematerializedFrame*
RematerializedFrame::New(JSContext* cx, uint8_t* top, InlineFrameIterator& iter,
                         MaybeReadFallback& fallback)
{
    unsigned numFormals = iter.isFunctionFrame() ? iter.calleeTemplate()->nargs() : 0;
    unsigned argSlots = Max(numFormals, iter.numActualArgs());
    size_t numBytes = sizeof(RematerializedFrame) +
        (argSlots + iter.script()->nfixed()) * sizeof(Value) -
        sizeof(Value); // 1 Value included in sizeof(RematerializedFrame)

    void* buf = cx->pod_calloc<uint8_t>(numBytes);
    if (!buf)
        return nullptr;

    return new (buf) RematerializedFrame(cx, top, iter.numActualArgs(), iter, fallback);
}

/* static */ bool
RematerializedFrame::RematerializeInlineFrames(JSContext* cx, uint8_t* top,
                                               InlineFrameIterator& iter,
                                               MaybeReadFallback& fallback,
                                               Vector<RematerializedFrame*>& frames)
{
    if (!frames.resize(iter.frameCount()))
        return false;

    while (true) {
        size_t frameNo = iter.frameNo();
        RematerializedFrame* frame = RematerializedFrame::New(cx, top, iter, fallback);
        if (!frame)
            return false;
        if (frame->scopeChain()) {
            if (!EnsureHasScopeObjects(cx, frame))
                return false;
        }

        frames[frameNo] = frame;

        if (!iter.more())
            break;
        ++iter;
    }

    return true;
}

/* static */ void
RematerializedFrame::FreeInVector(Vector<RematerializedFrame*>& frames)
{
    for (size_t i = 0; i < frames.length(); i++) {
        RematerializedFrame* f = frames[i];
        Debugger::assertNotInFrameMaps(f);
        f->RematerializedFrame::~RematerializedFrame();
        js_free(f);
    }
    frames.clear();
}

/* static */ void
RematerializedFrame::MarkInVector(JSTracer* trc, Vector<RematerializedFrame*>& frames)
{
    for (size_t i = 0; i < frames.length(); i++)
        frames[i]->mark(trc);
}

CallObject&
RematerializedFrame::callObj() const
{
    MOZ_ASSERT(hasCallObj());

    JSObject* scope = scopeChain();
    while (!scope->is<CallObject>())
        scope = scope->enclosingScope();
    return scope->as<CallObject>();
}

void
RematerializedFrame::pushOnScopeChain(ScopeObject& scope)
{
    MOZ_ASSERT(*scopeChain() == scope.enclosingScope() ||
               *scopeChain() == scope.as<CallObject>().enclosingScope().as<DeclEnvObject>().enclosingScope());
    scopeChain_ = &scope;
}

bool
RematerializedFrame::initFunctionScopeObjects(JSContext* cx)
{
    MOZ_ASSERT(isNonEvalFunctionFrame());
    MOZ_ASSERT(fun()->isHeavyweight());
    CallObject* callobj = CallObject::createForFunction(cx, this);
    if (!callobj)
        return false;
    pushOnScopeChain(*callobj);
    hasCallObj_ = true;
    return true;
}

void
RematerializedFrame::mark(JSTracer* trc)
{
    gc::MarkScriptRoot(trc, &script_, "remat ion frame script");
    gc::MarkObjectRoot(trc, &scopeChain_, "remat ion frame scope chain");
    gc::MarkValueRoot(trc, &returnValue_, "remat ion frame return value");
    gc::MarkValueRoot(trc, &thisValue_, "remat ion frame this");
    gc::MarkValueRootRange(trc, slots_, slots_ + numActualArgs_ + script_->nfixed(),
                           "remat ion frame stack");
}

void
RematerializedFrame::dump()
{
    fprintf(stderr, " Rematerialized Ion Frame%s\n", inlined() ? " (inlined)" : "");
    if (isFunctionFrame()) {
        fprintf(stderr, "  callee fun: ");
#ifdef DEBUG
        js_DumpValue(ObjectValue(*callee()));
#else
        fprintf(stderr, "?\n");
#endif
    } else {
        fprintf(stderr, "  global frame, no callee\n");
    }

    fprintf(stderr, "  file %s line %u offset %zu\n",
            script()->filename(), (unsigned) script()->lineno(),
            script()->pcToOffset(pc()));

    fprintf(stderr, "  script = %p\n", (void*) script());

    if (isFunctionFrame()) {
        fprintf(stderr, "  scope chain: ");
#ifdef DEBUG
        js_DumpValue(ObjectValue(*scopeChain()));
#else
        fprintf(stderr, "?\n");
#endif

        if (hasArgsObj()) {
            fprintf(stderr, "  args obj: ");
#ifdef DEBUG
            js_DumpValue(ObjectValue(argsObj()));
#else
            fprintf(stderr, "?\n");
#endif
        }

        fprintf(stderr, "  this: ");
#ifdef DEBUG
        js_DumpValue(thisValue());
#else
        fprintf(stderr, "?\n");
#endif

        for (unsigned i = 0; i < numActualArgs(); i++) {
            if (i < numFormalArgs())
                fprintf(stderr, "  formal (arg %d): ", i);
            else
                fprintf(stderr, "  overflown (arg %d): ", i);
#ifdef DEBUG
            js_DumpValue(argv()[i]);
#else
            fprintf(stderr, "?\n");
#endif
        }

        for (unsigned i = 0; i < script()->nfixed(); i++) {
            fprintf(stderr, "  local %d: ", i);
#ifdef DEBUG
            js_DumpValue(locals()[i]);
#else
            fprintf(stderr, "?\n");
#endif
        }
    }

    fputc('\n', stderr);
}
