/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "debugger/ExecutionTracer.h"

#include "debugger/Frame.h"       // DebuggerFrameType
#include "vm/ObjectOperations.h"  // DefineDataElement
#include "vm/Time.h"

#include "debugger/Debugger-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

enum class OutOfLineEntryType : uint8_t {
  ScriptURL,
  Atom,
};

enum class InlineEntryType : uint8_t {
  StackFunctionEnter,
  StackFunctionLeave,
  LabelEnter,
  LabelLeave,
  Error,
};

MOZ_RUNINIT mozilla::Vector<ExecutionTracer*> ExecutionTracer::globalInstances;
MOZ_RUNINIT Mutex
    ExecutionTracer::globalInstanceLock(mutexid::ExecutionTracerGlobalLock);

static JS::ExecutionTrace::ImplementationType GetImplementation(
    AbstractFramePtr frame) {
  if (frame.isBaselineFrame()) {
    return JS::ExecutionTrace::ImplementationType::Baseline;
  }

  if (frame.isRematerializedFrame()) {
    return JS::ExecutionTrace::ImplementationType::Ion;
  }

  if (frame.isWasmDebugFrame()) {
    return JS::ExecutionTrace::ImplementationType::Wasm;
  }

  return JS::ExecutionTrace::ImplementationType::Interpreter;
}

static DebuggerFrameType GetFrameType(AbstractFramePtr frame) {
  // Indirect eval frames are both isGlobalFrame() and isEvalFrame(), so the
  // order of checks here is significant.
  if (frame.isEvalFrame()) {
    return DebuggerFrameType::Eval;
  }

  if (frame.isGlobalFrame()) {
    return DebuggerFrameType::Global;
  }

  if (frame.isFunctionFrame()) {
    return DebuggerFrameType::Call;
  }

  if (frame.isModuleFrame()) {
    return DebuggerFrameType::Module;
  }

  if (frame.isWasmDebugFrame()) {
    return DebuggerFrameType::WasmCall;
  }

  MOZ_CRASH("Unknown frame type");
}

[[nodiscard]] static bool GetFunctionName(JSContext* cx,
                                          JS::Handle<JSFunction*> fun,
                                          JS::MutableHandle<JSAtom*> result) {
  if (!fun->getDisplayAtom(cx, result)) {
    return false;
  }

  if (result) {
    cx->markAtom(result);
  }
  return true;
}

static double GetNowMilliseconds() {
  return (mozilla::TimeStamp::Now() - mozilla::TimeStamp::ProcessCreation())
      .ToMilliseconds();
}

void ExecutionTracer::handleError(JSContext* cx) {
  inlineData_.beginWritingEntry();
  inlineData_.write(uint8_t(InlineEntryType::Error));
  inlineData_.finishWritingEntry();
  cx->clearPendingException();
  cx->suspendExecutionTracing();
}

void ExecutionTracer::writeScriptUrl(ScriptSource* scriptSource) {
  outOfLineData_.beginWritingEntry();
  outOfLineData_.write(uint8_t(OutOfLineEntryType::ScriptURL));
  outOfLineData_.write(scriptSource->id());

  if (scriptSource->hasDisplayURL()) {
    outOfLineData_.writeCString<char16_t, TracerStringEncoding::TwoByte>(
        scriptSource->displayURL());
  } else {
    const char* filename =
        scriptSource->filename() ? scriptSource->filename() : "";
    outOfLineData_.writeCString<char, TracerStringEncoding::UTF8>(filename);
  }
  outOfLineData_.finishWritingEntry();
}

bool ExecutionTracer::writeAtom(JSContext* cx, JS::Handle<JSAtom*> atom,
                                uint32_t id) {
  outOfLineData_.beginWritingEntry();
  outOfLineData_.write(uint8_t(OutOfLineEntryType::Atom));
  outOfLineData_.write(id);

  if (!atom) {
    outOfLineData_.writeEmptyString();
  } else {
    if (!outOfLineData_.writeString(cx, atom)) {
      return false;
    }
  }
  outOfLineData_.finishWritingEntry();
  return true;
}

bool ExecutionTracer::writeFunctionFrame(JSContext* cx,
                                         AbstractFramePtr frame) {
  JS::Rooted<JSFunction*> fn(cx, frame.callee());
  TracingCaches& caches = cx->caches().tracingCaches;
  if (fn->baseScript()) {
    uint32_t scriptSourceId = fn->baseScript()->scriptSource()->id();
    TracingCaches::GetOrPutResult scriptSourceRes =
        caches.putScriptSourceIfMissing(scriptSourceId);
    if (scriptSourceRes == TracingCaches::GetOrPutResult::OOM) {
      ReportOutOfMemory(cx);
      return false;
    }
    if (scriptSourceRes == TracingCaches::GetOrPutResult::NewlyAdded) {
      writeScriptUrl(fn->baseScript()->scriptSource());
    }
    inlineData_.write(fn->baseScript()->lineno());
    inlineData_.write(fn->baseScript()->column().oneOriginValue());
    inlineData_.write(scriptSourceId);
    inlineData_.write(
        fn->baseScript()->realm()->creationOptions().profilerRealmID());
  } else {
    // In the case of no baseScript, we just fill it out with 0s. 0 is an
    // invalid script source ID, so it is distinguishable from a real one
    inlineData_.write(uint32_t(0));  // line number
    inlineData_.write(uint32_t(0));  // column
    inlineData_.write(uint32_t(0));  // script source id
  }

  JS::Rooted<JSAtom*> functionName(cx);
  if (!GetFunctionName(cx, fn, &functionName)) {
    return false;
  }
  uint32_t functionNameId = 0;
  TracingCaches::GetOrPutResult fnNameRes =
      caches.getOrPutAtom(functionName, &functionNameId);
  if (fnNameRes == TracingCaches::GetOrPutResult::OOM) {
    ReportOutOfMemory(cx);
    return false;
  }
  if (fnNameRes == TracingCaches::GetOrPutResult::NewlyAdded) {
    if (!writeAtom(cx, functionName, functionNameId)) {
      // It's worth noting here that this will leave the caches out of sync
      // with what has actually been written into the out of line data.
      // This is a normal and allowed situation for the tracer, so we have
      // no special handling here for it. However, if we ever want to make
      // a stronger guarantee in the future, we need to revisit this.
      return false;
    }
  }

  inlineData_.write(functionNameId);
  inlineData_.write(uint8_t(GetImplementation(frame)));
  inlineData_.write(GetNowMilliseconds());
  return true;
}

void ExecutionTracer::onEnterFrame(JSContext* cx, AbstractFramePtr frame) {
  LockGuard<Mutex> guard(bufferLock_);

  DebuggerFrameType type = GetFrameType(frame);
  if (type == DebuggerFrameType::Call) {
    if (frame.isFunctionFrame() && !frame.callee()->isSelfHostedBuiltin()) {
      inlineData_.beginWritingEntry();
      inlineData_.write(uint8_t(InlineEntryType::StackFunctionEnter));
      if (!writeFunctionFrame(cx, frame)) {
        handleError(cx);
        return;
      }

      inlineData_.finishWritingEntry();
    }
  }
}

void ExecutionTracer::onLeaveFrame(JSContext* cx, AbstractFramePtr frame) {
  LockGuard<Mutex> guard(bufferLock_);

  DebuggerFrameType type = GetFrameType(frame);
  if (type == DebuggerFrameType::Call) {
    if (frame.isFunctionFrame() && !frame.callee()->isSelfHostedBuiltin()) {
      inlineData_.beginWritingEntry();
      inlineData_.write(uint8_t(InlineEntryType::StackFunctionLeave));
      if (!writeFunctionFrame(cx, frame)) {
        handleError(cx);
        return;
      }
      inlineData_.finishWritingEntry();
    }
  }
}

template <typename CharType, TracerStringEncoding Encoding>
void ExecutionTracer::onEnterLabel(const CharType* eventType) {
  LockGuard<Mutex> guard(bufferLock_);

  inlineData_.beginWritingEntry();
  inlineData_.write(uint8_t(InlineEntryType::LabelEnter));
  inlineData_.writeCString<CharType, Encoding>(eventType);
  inlineData_.write(GetNowMilliseconds());
  inlineData_.finishWritingEntry();
}

template <typename CharType, TracerStringEncoding Encoding>
void ExecutionTracer::onLeaveLabel(const CharType* eventType) {
  LockGuard<Mutex> guard(bufferLock_);

  inlineData_.beginWritingEntry();
  inlineData_.write(uint8_t(InlineEntryType::LabelLeave));
  inlineData_.writeCString<CharType, Encoding>(eventType);
  inlineData_.write(GetNowMilliseconds());
  inlineData_.finishWritingEntry();
}

bool ExecutionTracer::readFunctionFrame(
    JS::ExecutionTrace::EventKind kind,
    JS::ExecutionTrace::TracedEvent& event) {
  MOZ_ASSERT(kind == JS::ExecutionTrace::EventKind::FunctionEnter ||
             kind == JS::ExecutionTrace::EventKind::FunctionLeave);

  event.kind = kind;

  uint8_t implementation;
  inlineData_.read(&event.functionEvent.lineNumber);
  inlineData_.read(&event.functionEvent.column);
  inlineData_.read(&event.functionEvent.scriptId);
  inlineData_.read(&event.functionEvent.realmID);
  inlineData_.read(&event.functionEvent.functionNameId);
  inlineData_.read(&implementation);
  inlineData_.read(&event.time);

  event.functionEvent.implementation =
      JS::ExecutionTrace::ImplementationType(implementation);

  return true;
}

bool ExecutionTracer::readLabel(JS::ExecutionTrace::EventKind kind,
                                JS::ExecutionTrace::TracedEvent& event,
                                TracingScratchBuffer& scratchBuffer,
                                mozilla::Vector<char>& stringBuffer) {
  MOZ_ASSERT(kind == JS::ExecutionTrace::EventKind::LabelEnter ||
             kind == JS::ExecutionTrace::EventKind::LabelLeave);

  event.kind = kind;
  size_t index;
  if (!inlineData_.readString(scratchBuffer, stringBuffer, &index)) {
    return false;
  }
  event.labelEvent.label = index;

  double time;
  inlineData_.read(&time);
  event.time = time;

  return true;
}

bool ExecutionTracer::readInlineEntry(
    mozilla::Vector<JS::ExecutionTrace::TracedEvent>& events,
    TracingScratchBuffer& scratchBuffer, mozilla::Vector<char>& stringBuffer) {
  uint8_t entryType;
  inlineData_.read(&entryType);

  switch (InlineEntryType(entryType)) {
    case InlineEntryType::StackFunctionEnter:
    case InlineEntryType::StackFunctionLeave: {
      JS::ExecutionTrace::EventKind kind;
      if (InlineEntryType(entryType) == InlineEntryType::StackFunctionEnter) {
        kind = JS::ExecutionTrace::EventKind::FunctionEnter;
      } else {
        kind = JS::ExecutionTrace::EventKind::FunctionLeave;
      }
      JS::ExecutionTrace::TracedEvent event;
      if (!readFunctionFrame(kind, event)) {
        return false;
      }

      if (!events.append(std::move(event))) {
        return false;
      }
      return true;
    }
    case InlineEntryType::LabelEnter:
    case InlineEntryType::LabelLeave: {
      JS::ExecutionTrace::EventKind kind;
      if (InlineEntryType(entryType) == InlineEntryType::LabelEnter) {
        kind = JS::ExecutionTrace::EventKind::LabelEnter;
      } else {
        kind = JS::ExecutionTrace::EventKind::LabelLeave;
      }

      JS::ExecutionTrace::TracedEvent event;
      if (!readLabel(kind, event, scratchBuffer, stringBuffer)) {
        return false;
      }

      if (!events.append(std::move(event))) {
        return false;
      }

      return true;
    }
    case InlineEntryType::Error: {
      JS::ExecutionTrace::TracedEvent event;
      event.kind = JS::ExecutionTrace::EventKind::Error;

      if (!events.append(std::move(event))) {
        return false;
      }

      return true;
    }
    default:
      return false;
  }
}

bool ExecutionTracer::readOutOfLineEntry(
    mozilla::HashMap<uint32_t, size_t>& scriptUrls,
    mozilla::HashMap<uint32_t, size_t>& atoms,
    TracingScratchBuffer& scratchBuffer, mozilla::Vector<char>& stringBuffer) {
  uint8_t entryType;
  outOfLineData_.read(&entryType);

  switch (OutOfLineEntryType(entryType)) {
    case OutOfLineEntryType::ScriptURL: {
      uint32_t id;
      outOfLineData_.read(&id);

      size_t index;
      if (!outOfLineData_.readString(scratchBuffer, stringBuffer, &index)) {
        return false;
      }

      if (!scriptUrls.put(id, index)) {
        return false;
      }

      return true;
    }
    case OutOfLineEntryType::Atom: {
      uint32_t id;
      outOfLineData_.read(&id);

      size_t index;
      if (!outOfLineData_.readString(scratchBuffer, stringBuffer, &index)) {
        return false;
      }

      if (!atoms.put(id, index)) {
        return false;
      }

      return true;
    }
    default:
      return false;
  }
}

bool ExecutionTracer::readInlineEntries(
    mozilla::Vector<JS::ExecutionTrace::TracedEvent>& events,
    TracingScratchBuffer& scratchBuffer, mozilla::Vector<char>& stringBuffer) {
  while (inlineData_.readable()) {
    inlineData_.beginReadingEntry();
    if (!readInlineEntry(events, scratchBuffer, stringBuffer)) {
      inlineData_.skipEntry();
      return false;
    }
    inlineData_.finishReadingEntry();
  }
  return true;
}

bool ExecutionTracer::readOutOfLineEntries(
    mozilla::HashMap<uint32_t, size_t>& scriptUrls,
    mozilla::HashMap<uint32_t, size_t>& atoms,
    TracingScratchBuffer& scratchBuffer, mozilla::Vector<char>& stringBuffer) {
  while (outOfLineData_.readable()) {
    outOfLineData_.beginReadingEntry();
    if (!readOutOfLineEntry(scriptUrls, atoms, scratchBuffer, stringBuffer)) {
      outOfLineData_.skipEntry();
      return false;
    }
    outOfLineData_.finishReadingEntry();
  }
  return true;
}

bool ExecutionTracer::getNativeTrace(
    JS::ExecutionTrace::TracedJSContext& context,
    TracingScratchBuffer& scratchBuffer, mozilla::Vector<char>& stringBuffer) {
  LockGuard<Mutex> guard(bufferLock_);

  if (!readOutOfLineEntries(context.scriptUrls, context.atoms, scratchBuffer,
                            stringBuffer)) {
    return false;
  }

  if (!readInlineEntries(context.events, scratchBuffer, stringBuffer)) {
    return false;
  }

  return true;
}

bool ExecutionTracer::getNativeTraceForAllContexts(JS::ExecutionTrace& trace) {
  LockGuard<Mutex> guard(globalInstanceLock);
  TracingScratchBuffer scratchBuffer;
  for (ExecutionTracer* tracer : globalInstances) {
    JS::ExecutionTrace::TracedJSContext* context = nullptr;
    for (JS::ExecutionTrace::TracedJSContext& t : trace.contexts) {
      if (t.id == tracer->threadId_) {
        context = &t;
        break;
      }
    }
    if (!context) {
      if (!trace.contexts.append(JS::ExecutionTrace::TracedJSContext())) {
        return false;
      }
      context = &trace.contexts[trace.contexts.length() - 1];
      context->id = tracer->threadId_;
    }
    if (!tracer->getNativeTrace(*context, scratchBuffer, trace.stringBuffer)) {
      return false;
    }
  }

  return true;
}

void JS_TracerEnterLabelTwoByte(JSContext* cx, const char16_t* label) {
  CHECK_THREAD(cx);
  if (cx->hasExecutionTracer()) {
    cx->getExecutionTracer()
        .onEnterLabel<char16_t, TracerStringEncoding::TwoByte>(label);
  }
}

void JS_TracerEnterLabelLatin1(JSContext* cx, const char* label) {
  CHECK_THREAD(cx);
  if (cx->hasExecutionTracer()) {
    cx->getExecutionTracer().onEnterLabel<char, TracerStringEncoding::Latin1>(
        label);
  }
}

void JS_TracerLeaveLabelTwoByte(JSContext* cx, const char16_t* label) {
  CHECK_THREAD(cx);
  if (cx->hasExecutionTracer()) {
    cx->getExecutionTracer()
        .onLeaveLabel<char16_t, TracerStringEncoding::TwoByte>(label);
  }
}

void JS_TracerLeaveLabelLatin1(JSContext* cx, const char* label) {
  CHECK_THREAD(cx);
  if (cx->hasExecutionTracer()) {
    cx->getExecutionTracer().onLeaveLabel<char, TracerStringEncoding::Latin1>(
        label);
  }
}

bool JS_TracerIsTracing(JSContext* cx) { return cx->hasExecutionTracer(); }

bool JS_TracerBeginTracing(JSContext* cx) {
  CHECK_THREAD(cx);
  return cx->enableExecutionTracing();
}

bool JS_TracerEndTracing(JSContext* cx) {
  CHECK_THREAD(cx);
  cx->disableExecutionTracing();
  return true;
}

bool JS_TracerSnapshotTrace(JS::ExecutionTrace& trace) {
  return ExecutionTracer::getNativeTraceForAllContexts(trace);
}
