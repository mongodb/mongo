/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/RematerializedFrame.h"

#include <algorithm>
#include <utility>

#include "jit/Bailouts.h"
#include "jit/JSJitFrameIter.h"
#include "js/friend/DumpFunctions.h"  // js::DumpValue
#include "vm/ArgumentsObject.h"

#include "vm/EnvironmentObject-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace jit;

struct CopyValueToRematerializedFrame {
  Value* slots;

  explicit CopyValueToRematerializedFrame(Value* slots) : slots(slots) {}

  void operator()(const Value& v) { *slots++ = v; }
};

RematerializedFrame::RematerializedFrame(JSContext* cx, uint8_t* top,
                                         unsigned numActualArgs,
                                         InlineFrameIterator& iter,
                                         MaybeReadFallback& fallback)
    : prevUpToDate_(false),
      isDebuggee_(iter.script()->isDebuggee()),
      hasInitialEnv_(false),
      isConstructing_(iter.isConstructing()),
      hasCachedSavedFrame_(false),
      top_(top),
      pc_(iter.pc()),
      frameNo_(iter.frameNo()),
      numActualArgs_(numActualArgs),
      script_(iter.script()),
      envChain_(nullptr),
      argsObj_(nullptr) {
  if (iter.isFunctionFrame()) {
    callee_ = iter.callee(fallback);
  } else {
    callee_ = nullptr;
  }

  CopyValueToRematerializedFrame op(slots_);
  iter.readFrameArgsAndLocals(
      cx, op, op, &envChain_, &hasInitialEnv_, &returnValue_, &argsObj_,
      &thisArgument_, ReadFrameArgsBehavior::ActualsAndFormals, fallback);
}

/* static */
RematerializedFrame* RematerializedFrame::New(JSContext* cx, uint8_t* top,
                                              InlineFrameIterator& iter,
                                              MaybeReadFallback& fallback) {
  unsigned numFormals =
      iter.isFunctionFrame() ? iter.calleeTemplate()->nargs() : 0;
  unsigned argSlots = std::max(numFormals, iter.numActualArgs());
  unsigned extraSlots = argSlots + iter.script()->nfixed();

  // One Value slot is included in sizeof(RematerializedFrame), so we can
  // reduce the extra slot count by one.  However, if there are zero slot
  // allocations total, then reducing the slots by one will lead to
  // the memory allocation being smaller  than sizeof(RematerializedFrame).
  if (extraSlots > 0) {
    extraSlots -= 1;
  }

  RematerializedFrame* buf =
      cx->pod_calloc_with_extra<RematerializedFrame, Value>(extraSlots);
  if (!buf) {
    return nullptr;
  }

  return new (buf)
      RematerializedFrame(cx, top, iter.numActualArgs(), iter, fallback);
}

/* static */
bool RematerializedFrame::RematerializeInlineFrames(
    JSContext* cx, uint8_t* top, InlineFrameIterator& iter,
    MaybeReadFallback& fallback, RematerializedFrameVector& frames) {
  Rooted<RematerializedFrameVector> tempFrames(cx,
                                               RematerializedFrameVector(cx));
  if (!tempFrames.resize(iter.frameCount())) {
    return false;
  }

  while (true) {
    size_t frameNo = iter.frameNo();
    tempFrames[frameNo].reset(
        RematerializedFrame::New(cx, top, iter, fallback));
    if (!tempFrames[frameNo]) {
      return false;
    }
    if (tempFrames[frameNo]->environmentChain()) {
      if (!EnsureHasEnvironmentObjects(cx, tempFrames[frameNo].get().get())) {
        return false;
      }
    }

    if (!iter.more()) {
      break;
    }
    ++iter;
  }

  frames = std::move(tempFrames.get());
  return true;
}

CallObject& RematerializedFrame::callObj() const {
  MOZ_ASSERT(hasInitialEnvironment());
  MOZ_ASSERT(callee()->needsCallObject());

  JSObject* env = environmentChain();
  while (!env->is<CallObject>()) {
    env = env->enclosingEnvironment();
  }
  return env->as<CallObject>();
}

bool RematerializedFrame::initFunctionEnvironmentObjects(JSContext* cx) {
  return js::InitFunctionEnvironmentObjects(cx, this);
}

bool RematerializedFrame::pushVarEnvironment(JSContext* cx,
                                             Handle<Scope*> scope) {
  return js::PushVarEnvironmentObject(cx, scope, this);
}

void RematerializedFrame::trace(JSTracer* trc) {
  TraceRoot(trc, &script_, "remat ion frame script");
  TraceRoot(trc, &envChain_, "remat ion frame env chain");
  if (callee_) {
    TraceRoot(trc, &callee_, "remat ion frame callee");
  }
  if (argsObj_) {
    TraceRoot(trc, &argsObj_, "remat ion frame argsobj");
  }
  TraceRoot(trc, &returnValue_, "remat ion frame return value");
  TraceRoot(trc, &thisArgument_, "remat ion frame this");
  TraceRootRange(trc, numArgSlots() + script_->nfixed(), slots_,
                 "remat ion frame stack");
}

void RematerializedFrame::dump() {
  fprintf(stderr, " Rematerialized Ion Frame%s\n",
          inlined() ? " (inlined)" : "");
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

  fprintf(stderr, "  file %s line %u offset %zu\n", script()->filename(),
          script()->lineno(), script()->pcToOffset(pc()));

  fprintf(stderr, "  script = %p\n", (void*)script());

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
      if (i < numFormalArgs()) {
        fprintf(stderr, "  formal (arg %u): ", i);
      } else {
        fprintf(stderr, "  overflown (arg %u): ", i);
      }
#ifdef DEBUG
      DumpValue(argv()[i]);
#else
      fprintf(stderr, "?\n");
#endif
    }

    for (unsigned i = 0; i < script()->nfixed(); i++) {
      fprintf(stderr, "  local %u: ", i);
#ifdef DEBUG
      DumpValue(locals()[i]);
#else
      fprintf(stderr, "?\n");
#endif
    }
  }

  fputc('\n', stderr);
}
