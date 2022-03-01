/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/TraceLogging.h"

#include "mozilla/EndianUtils.h"
#include "mozilla/MemoryReporting.h"

#include <algorithm>
#include <string.h>
#include <utility>

#include "jit/BaselineJIT.h"
#include "jit/CompileWrappers.h"
#include "jit/JitSpewer.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Printf.h"
#include "js/TraceLoggerAPI.h"
#include "threading/LockGuard.h"
#include "util/GetPidProvider.h"  // getpid()
#include "util/Text.h"
#include "vm/Activation.h"  // js::ActivationIterator
#include "vm/FrameIter.h"   // js::JitFrameIter
#include "vm/JSContext.h"
#include "vm/JSScript.h"
#include "vm/Runtime.h"
#include "vm/Time.h"
#include "vm/TraceLoggingGraph.h"

using namespace js;

static TraceLoggerThreadState* traceLoggerState = nullptr;

static bool getTraceLoggerSupported() {
  char* str = getenv("JS_TRACE_LOGGING");

  if (!str) {
    // Default to unsupported.
    return false;
  }

  if (strcmp(str, "false") == 0 || strcmp(str, "no") == 0 ||
      strcmp(str, "0") == 0) {
    return false;
  }

  if (strcmp(str, "true") == 0 || strcmp(str, "yes") == 0 ||
      strcmp(str, "1") == 0) {
    fprintf(stderr, "Warning: TraceLogger disabled temporarily, bug 1565788\n");
    return false;
  }

  fprintf(stderr, "Warning: I didn't understand JS_TRACE_LOGGING=\"%s\"\n",
          str);
  return false;
}

size_t js::SizeOfTraceLogState(mozilla::MallocSizeOf mallocSizeOf) {
  return traceLoggerState ? traceLoggerState->sizeOfIncludingThis(mallocSizeOf)
                          : 0;
}

void js::ResetTraceLogger() {
  if (!traceLoggerState) {
    return;
  }

  traceLoggerState->clear();
}

void js::DestroyTraceLoggerThreadState() {
  if (traceLoggerState) {
    js_delete(traceLoggerState);
    traceLoggerState = nullptr;
  }
}

#ifdef DEBUG
void js::AssertCurrentThreadOwnsTraceLoggerThreadStateLock() {
  if (traceLoggerState) {
    traceLoggerState->lock.assertOwnedByCurrentThread();
  }
}
#endif

void js::DestroyTraceLogger(TraceLoggerThread* logger) {
  MOZ_ASSERT(traceLoggerState);
  traceLoggerState->destroyLogger(logger);
}

bool TraceLoggerThread::init() {
  if (!events_.init()) {
    return false;
  }

  // Minimum amount of capacity needed for operation to allow flushing.
  // Flushing requires space for the actual event and two spaces to log the
  // start and stop of flushing.
  if (!events_.ensureSpaceBeforeAdd(3)) {
    return false;
  }

  char* buffer = js_pod_malloc<char>(32);
  js::ThisThread::GetName(buffer, 32);
  threadName_.reset(buffer);

  return true;
}

void TraceLoggerThread::initGraph() {
  // Create a graph. I don't like this is called reset, but it locks the
  // graph into the UniquePtr. So it gets deleted when TraceLoggerThread
  // is destructed.
  graph_.reset(js_new<TraceLoggerGraph>());
  if (!graph_.get()) {
    return;
  }

  MOZ_ASSERT(traceLoggerState);
  bool graphFile = traceLoggerState->isGraphFileEnabled();
  double delta =
      traceLoggerState->getTimeStampOffset(mozilla::TimeStamp::NowUnfuzzed());
  uint64_t start = static_cast<uint64_t>(delta);
  if (!graph_->init(start, graphFile)) {
    graph_ = nullptr;
    return;
  }

  if (graphFile) {
    // Report the textIds to the graph.
    for (uint32_t i = 0; i < TraceLogger_TreeItemEnd; i++) {
      TraceLoggerTextId id = TraceLoggerTextId(i);
      graph_->addTextId(i, TLTextIdString(id));
    }
    graph_->addTextId(TraceLogger_TreeItemEnd, "TraceLogger internal");
    for (uint32_t i = TraceLogger_TreeItemEnd + 1; i < TraceLogger_Last; i++) {
      TraceLoggerTextId id = TraceLoggerTextId(i);
      graph_->addTextId(i, TLTextIdString(id));
    }
  }
}

void TraceLoggerThreadState::disableAllTextIds() {
  for (uint32_t i = 1; i < TraceLogger_Last; i++) {
    enabledTextIds[i] = false;
  }
}

void TraceLoggerThreadState::enableTextIdsForProfiler() {
  enableDefaultLogging();
}

void TraceLoggerThreadState::disableTextIdsForProfiler() {
  disableAllTextIds();
  // We have to keep the Baseline and IonMonkey id's alive because they control
  // whether the jitted codegen has tracelogger start & stop events builtin.
  // Otherwise, we end up in situations when some jitted code that was created
  // before the profiler was even started ends up not starting and stoping any
  // events.  The TraceLogger_Engine stop events can accidentally stop the wrong
  // event in this case, and then it's no longer possible to build a graph.
  enabledTextIds[TraceLogger_Engine] = true;
  enabledTextIds[TraceLogger_Interpreter] = true;
  enabledTextIds[TraceLogger_Baseline] = true;
  enabledTextIds[TraceLogger_IonMonkey] = true;
}

void TraceLoggerThreadState::enableDefaultLogging() {
  enabledTextIds[TraceLogger_AnnotateScripts] = true;
  enabledTextIds[TraceLogger_Bailout] = true;
  enabledTextIds[TraceLogger_Baseline] = true;
  enabledTextIds[TraceLogger_BaselineCompilation] = true;
  enabledTextIds[TraceLogger_GC] = true;
  enabledTextIds[TraceLogger_GCAllocation] = true;
  enabledTextIds[TraceLogger_GCSweeping] = true;
  enabledTextIds[TraceLogger_Interpreter] = true;
  enabledTextIds[TraceLogger_IonAnalysis] = true;
  enabledTextIds[TraceLogger_IonCompilation] = true;
  enabledTextIds[TraceLogger_IonLinking] = true;
  enabledTextIds[TraceLogger_IonMonkey] = true;
  enabledTextIds[TraceLogger_MinorGC] = true;
  enabledTextIds[TraceLogger_Frontend] = true;
  enabledTextIds[TraceLogger_ParsingFull] = true;
  enabledTextIds[TraceLogger_ParsingSyntax] = true;
  enabledTextIds[TraceLogger_BytecodeEmission] = true;
  enabledTextIds[TraceLogger_IrregexpCompile] = true;
  enabledTextIds[TraceLogger_IrregexpExecute] = true;
  enabledTextIds[TraceLogger_Scripts] = true;
  enabledTextIds[TraceLogger_Engine] = true;
  enabledTextIds[TraceLogger_WasmCompilation] = true;
  enabledTextIds[TraceLogger_Invalidation] = true;
}

void TraceLoggerThreadState::enableIonLogging() {
  enabledTextIds[TraceLogger_IonCompilation] = true;
  enabledTextIds[TraceLogger_IonLinking] = true;
  enabledTextIds[TraceLogger_PruneUnusedBranches] = true;
  enabledTextIds[TraceLogger_FoldTests] = true;
  enabledTextIds[TraceLogger_SplitCriticalEdges] = true;
  enabledTextIds[TraceLogger_RenumberBlocks] = true;
  enabledTextIds[TraceLogger_ScalarReplacement] = true;
  enabledTextIds[TraceLogger_DominatorTree] = true;
  enabledTextIds[TraceLogger_PhiAnalysis] = true;
  enabledTextIds[TraceLogger_MakeLoopsContiguous] = true;
  enabledTextIds[TraceLogger_ApplyTypes] = true;
  enabledTextIds[TraceLogger_EagerSimdUnbox] = true;
  enabledTextIds[TraceLogger_AliasAnalysis] = true;
  enabledTextIds[TraceLogger_GVN] = true;
  enabledTextIds[TraceLogger_LICM] = true;
  enabledTextIds[TraceLogger_RangeAnalysis] = true;
  enabledTextIds[TraceLogger_LoopUnrolling] = true;
  enabledTextIds[TraceLogger_FoldLinearArithConstants] = true;
  enabledTextIds[TraceLogger_EffectiveAddressAnalysis] = true;
  enabledTextIds[TraceLogger_AlignmentMaskAnalysis] = true;
  enabledTextIds[TraceLogger_EliminateDeadCode] = true;
  enabledTextIds[TraceLogger_ReorderInstructions] = true;
  enabledTextIds[TraceLogger_EdgeCaseAnalysis] = true;
  enabledTextIds[TraceLogger_EliminateRedundantChecks] = true;
  enabledTextIds[TraceLogger_AddKeepAliveInstructions] = true;
  enabledTextIds[TraceLogger_GenerateLIR] = true;
  enabledTextIds[TraceLogger_RegisterAllocation] = true;
  enabledTextIds[TraceLogger_GenerateCode] = true;
  enabledTextIds[TraceLogger_Scripts] = true;
}

void TraceLoggerThreadState::enableFrontendLogging() {
  enabledTextIds[TraceLogger_Frontend] = true;
  enabledTextIds[TraceLogger_ParsingFull] = true;
  enabledTextIds[TraceLogger_ParsingSyntax] = true;
  enabledTextIds[TraceLogger_BytecodeEmission] = true;
  enabledTextIds[TraceLogger_BytecodeFoldConstants] = true;
  enabledTextIds[TraceLogger_BytecodeNameFunctions] = true;
}

TraceLoggerThread::~TraceLoggerThread() {
  if (graph_.get()) {
    if (!failed_) {
      graph_->log(events_, traceLoggerState->startTime);
    }
    graph_ = nullptr;
  }
}

bool TraceLoggerThread::enable() {
  if (enabled_ > 0) {
    enabled_++;
    return true;
  }

  if (failed_) {
    return false;
  }

  enabled_ = 1;
  logTimestamp(TraceLogger_Enable);

  return true;
}

bool TraceLoggerThread::fail(JSContext* cx, const char* error) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_TRACELOGGER_ENABLE_FAIL, error);
  failed_ = true;
  enabled_ = 0;

  return false;
}

void TraceLoggerThread::silentFail(const char* error) {
  traceLoggerState->maybeSpewError(error);
  failed_ = true;
  enabled_ = 0;
}

size_t TraceLoggerThread::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  size_t size = 0;
#ifdef DEBUG
  size += graphStack_.sizeOfExcludingThis(mallocSizeOf);
#endif
  size += events_.sizeOfExcludingThis(mallocSizeOf);
  if (graph_.get()) {
    size += graph_->sizeOfIncludingThis(mallocSizeOf);
  }
  return size;
}

size_t TraceLoggerThread::sizeOfIncludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
}

bool TraceLoggerThread::enable(JSContext* cx) {
  using namespace js::jit;

  if (!enable()) {
    return fail(cx, "internal error");
  }

  if (enabled_ == 1) {
    // Get the top Activation to log the top script/pc (No inlined frames).
    ActivationIterator iter(cx);
    Activation* act = iter.activation();

    if (!act) {
      return fail(cx, "internal error");
    }

    JSScript* script = nullptr;
    int32_t engine = 0;

    if (act->isJit()) {
      JitFrameIter frame(iter->asJit());

      while (!frame.done()) {
        if (frame.isWasm()) {
          JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                    JSMSG_TRACELOGGER_ENABLE_FAIL,
                                    "not yet supported in wasm code");
          return false;
        }
        if (frame.asJSJit().isScripted()) {
          break;
        }
        ++frame;
      }

      MOZ_ASSERT(!frame.done());

      const JSJitFrameIter& jitFrame = frame.asJSJit();
      MOZ_ASSERT(jitFrame.isIonJS() || jitFrame.isBaselineJS());

      script = jitFrame.script();
      engine =
          jitFrame.isIonJS() ? TraceLogger_IonMonkey : TraceLogger_Baseline;
    } else {
      MOZ_ASSERT(act->isInterpreter());
      InterpreterFrame* fp = act->asInterpreter()->current();
      MOZ_ASSERT(!fp->runningInJit());

      script = fp->script();
      engine = TraceLogger_Interpreter;
    }
    if (script->compartment() != cx->compartment()) {
      return fail(cx, "compartment mismatch");
    }

    TraceLoggerEvent event(TraceLogger_Scripts, script);
    startEvent(event);
    startEvent(engine);
  }

  return true;
}

bool TraceLoggerThread::disable(bool force, const char* error) {
  if (failed_) {
    MOZ_ASSERT(enabled_ == 0);
    return false;
  }

  if (enabled_ == 0) {
    return true;
  }

  if (enabled_ > 1 && !force) {
    enabled_--;
    return true;
  }

  if (force) {
    traceLoggerState->maybeSpewError(error);
  }

  logTimestamp(TraceLogger_Disable);
  enabled_ = 0;

  return true;
}

const char* TraceLoggerThread::maybeEventText(uint32_t id) {
  if (TLTextIdIsEnumEvent(id)) {
    return TLTextIdString(static_cast<TraceLoggerTextId>(id));
  }
  return traceLoggerState->maybeEventText(id);
}

TraceLoggerEventPayload* TraceLoggerThreadState::getPayload(uint32_t id) {
  if (TLTextIdIsEnumEvent(id)) {
    return nullptr;
  }

  TextIdToPayloadMap::Ptr p = textIdPayloads.lookup(id);
  if (!p) {
    return nullptr;
  }

  p->value()->use();
  return p->value();
}

const char* TraceLoggerThreadState::maybeEventText(uint32_t id) {
  LockGuard<Mutex> guard(lock);

  TextIdToPayloadMap::Ptr p = textIdPayloads.lookup(id);
  if (!p) {
    return nullptr;
  }

  uint32_t dictId = p->value()->dictionaryId();
  MOZ_ASSERT(dictId < nextDictionaryId);
  return dictionaryData[dictId].get();
}

const char* TraceLoggerThreadState::maybeEventText(TraceLoggerEventPayload* p) {
  LockGuard<Mutex> guard(lock);
  if (!p) {
    return nullptr;
  }

  uint32_t dictId = p->dictionaryId();
  MOZ_ASSERT(dictId < nextDictionaryId);
  return dictionaryData[dictId].get();
}

size_t TraceLoggerThreadState::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) {
  LockGuard<Mutex> guard(lock);

  // Do not count threadLoggers since they are counted by
  // JSContext::traceLogger.

  size_t size = 0;
  size += dictionaryData.sizeOfExcludingThis(mallocSizeOf);
  size += payloadDictionary.shallowSizeOfExcludingThis(mallocSizeOf);
  size += textIdPayloads.shallowSizeOfExcludingThis(mallocSizeOf);
  for (TextIdToPayloadMap::Range r = textIdPayloads.all(); !r.empty();
       r.popFront()) {
    r.front().value()->sizeOfIncludingThis(mallocSizeOf);
  }

  return size;
}

TraceLoggerEventPayload* TraceLoggerThreadState::getOrCreateEventPayload(
    const char* text) {
  LockGuard<Mutex> guard(lock);

  uint32_t dictId = nextDictionaryId;

  StringHashToDictionaryMap::AddPtr dictp =
      payloadDictionary.lookupForAdd(text);
  if (dictp) {
    dictId = dictp->value();
    MOZ_ASSERT(dictId < nextDictionaryId);  // Sanity check.
  } else {
    UniqueChars str = DuplicateString(text);
    if (!str) {
      return nullptr;
    }
    if (!payloadDictionary.add(dictp, str.get(), nextDictionaryId)) {
      return nullptr;
    }
    if (!dictionaryData.append(std::move(str))) {
      return nullptr;
    }

    nextDictionaryId++;
  }

  // Look for a free entry, as some textId's may
  // already be taken from previous profiling sessions.
  while (textIdPayloads.has(nextTextId)) {
    nextTextId++;
  }

  auto* payload = js_new<TraceLoggerEventPayload>(nextTextId, dictId);
  if (!payload) {
    return nullptr;
  }

  if (!textIdPayloads.putNew(nextTextId, payload)) {
    js_delete(payload);
    return nullptr;
  }

  payload->use();

  nextTextId++;

  return payload;
}

TraceLoggerEventPayload* TraceLoggerThreadState::getOrCreateEventPayload(
    const char* filename, uint32_t lineno, uint32_t colno) {
  if (!filename) {
    filename = "<unknown>";
  }

  TraceLoggerEventPayload* payload = getOrCreateEventPayload(filename);
  if (!payload) {
    return nullptr;
  }

  payload->setLine(lineno);
  payload->setColumn(colno);

  return payload;
}

TraceLoggerEventPayload* TraceLoggerThreadState::getOrCreateEventPayload(
    JSScript* script) {
  return getOrCreateEventPayload(script->filename(), script->lineno(),
                                 script->column());
}

void TraceLoggerThreadState::purgeUnusedPayloads() {
  // Care needs to be taken to maintain a coherent state in this function,
  // as payloads can have their use count change at any time from non-zero to
  // zero (but not the other way around; see TraceLoggerEventPayload::use()).
  LockGuard<Mutex> guard(lock);

  // Free all other payloads that have no uses anymore.
  for (TextIdToPayloadMap::Enum e(textIdPayloads); !e.empty(); e.popFront()) {
    if (e.front().value()->uses() == 0) {
      uint32_t dictId = e.front().value()->dictionaryId();
      dictionaryData.erase(dictionaryData.begin() + dictId);
      js_delete(e.front().value());
      e.removeFront();
    }
  }
}

void TraceLoggerThread::startEvent(TraceLoggerTextId id) {
  startEvent(uint32_t(id));
}

void TraceLoggerThread::startEvent(const TraceLoggerEvent& event) {
  if (!event.hasTextId()) {
    if (!enabled()) {
      return;
    }
    startEvent(TraceLogger_Error);
    disable(/* force = */ true,
            "TraceLogger encountered an empty event. "
            "Potentially due to OOM during creation of "
            "this event. Disabling TraceLogger.");
    return;
  }
  startEvent(event.textId());
}

void TraceLoggerThread::startEvent(uint32_t id) {
  if (!jit::JitOptions.enableTraceLogger) {
    return;
  }

  MOZ_ASSERT(TLTextIdIsTreeEvent(id) || id == TraceLogger_Error);
  MOZ_ASSERT(traceLoggerState);
  if (!traceLoggerState->isTextIdEnabled(id)) {
    return;
  }

#ifdef DEBUG
  if (enabled_ > 0) {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!graphStack_.append(id)) {
      oomUnsafe.crash("Could not add item to debug stack.");
    }
  }
#endif

  if (graph_.get() && traceLoggerState->isGraphFileEnabled()) {
    // Flush each textId to disk.  textId values up to TraceLogger_Last are
    // statically defined and each one has an associated constant event string
    // defined by TLTextIdString().  For any events with textId >=
    // TraceLogger_Last the payload associated with that textId must first be
    // found and then maybeEventText() will find the event string form the
    // dictionary.
    for (uint32_t otherId = graph_->nextTextId(); otherId <= id; otherId++) {
      if (TLTextIdIsEnumEvent(id)) {
        const char* text = TLTextIdString(static_cast<TraceLoggerTextId>(id));
        graph_->addTextId(otherId, text);
      } else {
        TraceLoggerEventPayload* p = traceLoggerState->getPayload(id);
        if (p) {
          const char* filename = traceLoggerState->maybeEventText(p);
          mozilla::Maybe<uint32_t> line = p->line();
          mozilla::Maybe<uint32_t> column = p->column();
          graph_->addTextId(otherId, filename, line, column);
          p->release();
        }
      }
    }
  }

  log(id);
}

void TraceLoggerThread::stopEvent(TraceLoggerTextId id) {
  stopEvent(uint32_t(id));
}

void TraceLoggerThread::stopEvent(const TraceLoggerEvent& event) {
  if (!event.hasTextId()) {
    stopEvent(TraceLogger_Error);
    return;
  }
  stopEvent(event.textId());
}

void TraceLoggerThread::stopEvent(uint32_t id) {
  if (!jit::JitOptions.enableTraceLogger) {
    return;
  }

  MOZ_ASSERT(TLTextIdIsTreeEvent(id) || id == TraceLogger_Error);
  MOZ_ASSERT(traceLoggerState);
  if (!traceLoggerState->isTextIdEnabled(id)) {
    return;
  }

#ifdef DEBUG
  if (!graphStack_.empty()) {
    uint32_t prev = graphStack_.popCopy();
    if (id == TraceLogger_Error || prev == TraceLogger_Error) {
      // When encountering an Error id the stack will most likely not be correct
      // anymore. Ignore this.
    } else if (id == TraceLogger_Engine) {
      MOZ_ASSERT(prev == TraceLogger_IonMonkey ||
                 prev == TraceLogger_Baseline ||
                 prev == TraceLogger_Interpreter);
    } else if (id == TraceLogger_Scripts) {
      MOZ_ASSERT(TLTextIdIsScriptEvent(prev));
    } else if (TLTextIdIsScriptEvent(id)) {
      MOZ_ASSERT(TLTextIdIsScriptEvent(prev));
      if (prev != id) {
        // Ignore if the text has been flushed already.
        MOZ_ASSERT_IF(maybeEventText(prev),
                      strcmp(maybeEventText(id), maybeEventText(prev)) == 0);
      }
    } else {
      MOZ_ASSERT(id == prev);
    }
  }
#endif

  log(TraceLogger_Stop);
}

JS::AutoTraceLoggerLockGuard::AutoTraceLoggerLockGuard() {
  traceLoggerState->lock.lock();
}

JS::AutoTraceLoggerLockGuard::~AutoTraceLoggerLockGuard() {
  traceLoggerState->lock.unlock();
}

size_t JS::TraceLoggerDictionaryImpl::NextChunk(JSContext* cx,
                                                size_t* dataIndex,
                                                char buffer[],
                                                size_t bufferSize) {
  MOZ_ASSERT(dataIndex != nullptr);
  if (!traceLoggerState || bufferSize == 0 || !buffer ||
      !jit::JitOptions.enableTraceLogger) {
    return 0;
  }

  size_t bufferIndex = 0;

  const char* eventString = nullptr;
  if (TLTextIdIsEnumEvent(*dataIndex)) {
    eventString = TLTextIdString(static_cast<TraceLoggerTextId>(*dataIndex));
  } else {
    uint32_t dictId = *dataIndex - TraceLogger_Last;
    if (dictId < traceLoggerState->dictionaryData.length()) {
      eventString = traceLoggerState->dictionaryData[dictId].get();
      MOZ_ASSERT(eventString);
    }
  }

  if (eventString) {
    size_t length = strlen(eventString);
    if (length < bufferSize - 1) {
      memcpy(buffer, eventString, length);
      buffer[length] = '\0';
      bufferIndex = length;
    } else {
      memcpy(buffer, eventString, bufferSize);
      buffer[bufferSize - 1] = '\0';
      bufferIndex = bufferSize - 1;
    }
  }

  (*dataIndex)++;
  return bufferIndex;
}

size_t JS::TraceLoggerIdImpl::NextChunk(JSContext* cx, size_t* dataIndex,
                                        uint32_t buffer[], size_t bufferSize) {
  MOZ_ASSERT(dataIndex != nullptr);
  if (!cx || !cx->traceLogger) {
    return 0;
  }

  if (bufferSize == 0 || !buffer || !jit::JitOptions.enableTraceLogger) {
    return 0;
  }

  size_t bufferIndex = 0;
  ContinuousSpace<EventEntry>& events = cx->traceLogger->events_;

  for (; *dataIndex < events.size(); (*dataIndex)++) {
    if (TLTextIdIsInternalEvent(events[*dataIndex].textId)) {
      continue;
    }

    if (TLTextIdIsScriptEvent(events[*dataIndex].textId)) {
      TraceLoggerEventPayload* payload =
          traceLoggerState->getPayload(events[*dataIndex].textId);
      MOZ_ASSERT(payload);
      if (!payload) {
        return 0;
      }
      // Write the index of this event into the jsTracerDictionary array
      // property
      uint32_t dictId = TraceLogger_Last + payload->dictionaryId();
      buffer[bufferIndex++] = dictId;
      payload->release();
    } else {
      buffer[bufferIndex++] = events[*dataIndex].textId;
    }

    if (bufferIndex == bufferSize) {
      break;
    }
  }

  return bufferIndex;
}

size_t JS::TraceLoggerLineNoImpl::NextChunk(JSContext* cx, size_t* dataIndex,
                                            int32_t buffer[],
                                            size_t bufferSize) {
  MOZ_ASSERT(dataIndex != nullptr);
  if (!cx || !cx->traceLogger) {
    return 0;
  }

  if (bufferSize == 0 || !buffer || !jit::JitOptions.enableTraceLogger) {
    return 0;
  }

  size_t bufferIndex = 0;
  ContinuousSpace<EventEntry>& events = cx->traceLogger->events_;

  for (; *dataIndex < events.size(); (*dataIndex)++) {
    if (TLTextIdIsInternalEvent(events[*dataIndex].textId)) {
      continue;
    }

    if (TLTextIdIsScriptEvent(events[*dataIndex].textId)) {
      TraceLoggerEventPayload* payload =
          traceLoggerState->getPayload(events[*dataIndex].textId);
      MOZ_ASSERT(payload);
      if (!payload) {
        return 0;
      }
      mozilla::Maybe<uint32_t> line = payload->line();
      payload->release();
      if (line) {
        buffer[bufferIndex++] = *line;
      } else {
        buffer[bufferIndex++] = -1;
      }
    } else {
      buffer[bufferIndex++] = -1;
    }
    if (bufferIndex == bufferSize) {
      break;
    }
  }

  return bufferIndex;
}

size_t JS::TraceLoggerColNoImpl::NextChunk(JSContext* cx, size_t* dataIndex,
                                           int32_t buffer[],
                                           size_t bufferSize) {
  MOZ_ASSERT(dataIndex != nullptr);
  if (!cx || !cx->traceLogger) {
    return 0;
  }

  if (bufferSize == 0 || !buffer || !jit::JitOptions.enableTraceLogger) {
    return 0;
  }

  size_t bufferIndex = 0;
  ContinuousSpace<EventEntry>& events = cx->traceLogger->events_;

  for (; *dataIndex < events.size(); (*dataIndex)++) {
    if (TLTextIdIsInternalEvent(events[*dataIndex].textId)) {
      continue;
    }

    if (TLTextIdIsScriptEvent(events[*dataIndex].textId)) {
      TraceLoggerEventPayload* payload =
          traceLoggerState->getPayload(events[*dataIndex].textId);
      MOZ_ASSERT(payload);
      if (!payload) {
        return 0;
      }
      mozilla::Maybe<uint32_t> column = payload->column();
      payload->release();
      if (column) {
        buffer[bufferIndex++] = *column;
      } else {
        buffer[bufferIndex++] = -1;
      }
    } else {
      buffer[bufferIndex++] = -1;
    }
    if (bufferIndex == bufferSize) {
      break;
    }
  }

  return bufferIndex;
}

size_t JS::TraceLoggerTimeStampImpl::NextChunk(JSContext* cx, size_t* dataIndex,
                                               mozilla::TimeStamp buffer[],
                                               size_t bufferSize) {
  MOZ_ASSERT(dataIndex != nullptr);
  if (!cx || !cx->traceLogger) {
    return 0;
  }

  if (bufferSize == 0 || !buffer || !jit::JitOptions.enableTraceLogger) {
    return 0;
  }

  size_t bufferIndex = 0;
  ContinuousSpace<EventEntry>& events = cx->traceLogger->events_;

  for (; *dataIndex < events.size(); (*dataIndex)++) {
    if (TLTextIdIsInternalEvent(events[*dataIndex].textId)) {
      continue;
    }
    buffer[bufferIndex++] = events[*dataIndex].time;
    if (bufferIndex == bufferSize) {
      break;
    }
  }

  return bufferIndex;
}

size_t JS::TraceLoggerDurationImpl::NextChunk(JSContext* cx, size_t* dataIndex,
                                              double buffer[],
                                              size_t bufferSize) {
  MOZ_ASSERT(dataIndex != nullptr);
  if (!cx || !cx->traceLogger) {
    return 0;
  }

  if (bufferSize == 0 || !buffer || !jit::JitOptions.enableTraceLogger) {
    return 0;
  }

  ContinuousSpace<EventEntry>& events = cx->traceLogger->events_;
  Vector<size_t, 0, js::SystemAllocPolicy> eventStack;
  using EventDurationMap =
      HashMap<size_t, double, DefaultHasher<size_t>, SystemAllocPolicy>;
  EventDurationMap eventMap;

  size_t bufferIndex = 0;
  for (; *dataIndex < events.size(); (*dataIndex)++) {
    if (TLTextIdIsInternalEvent(events[*dataIndex].textId)) {
      continue;
    }
    double duration = 0;
    if (TLTextIdIsLogEvent(events[*dataIndex].textId)) {
      // log events are snapshot events with no start & stop
      duration = -1;
    } else if (EventDurationMap::Ptr p = eventMap.lookup(*dataIndex)) {
      // value has already been cached
      duration = p->value();
    } else {
      MOZ_ASSERT(eventStack.empty());
      if (!eventStack.append(*dataIndex)) {
        return 0;
      }

      // Search through the events array to find the matching stop event in
      // order to calculate the duration time.  Cache all other durations we
      // calculate in the meantime.
      for (size_t j = *dataIndex + 1; j < events.size(); j++) {
        uint32_t id = events[j].textId;
        if (id == TraceLogger_Stop) {
          uint32_t prev = eventStack.popCopy();
          double delta = (events[j].time - events[prev].time).ToMicroseconds();
          if (prev == *dataIndex) {
            MOZ_ASSERT(eventStack.empty());
            duration = delta;
            break;
          }

          if (!eventMap.putNew(prev, delta)) {
            return 0;
          }

        } else if (TLTextIdIsTreeEvent(id)) {
          if (!eventStack.append(j)) {
            return 0;
          }
        }

        // If we reach the end of the list, use the last event as the end
        // event for all events remaining on the stack.
        if (j == events.size() - 1) {
          while (!eventStack.empty()) {
            uint32_t prev = eventStack.popCopy();
            double delta =
                (events[j].time - events[prev].time).ToMicroseconds();
            if (prev == *dataIndex) {
              MOZ_ASSERT(eventStack.empty());
              duration = delta;
            } else {
              if (!eventMap.putNew(prev, delta)) {
                return 0;
              }
            }
          }
        }
      }
    }

    buffer[bufferIndex++] = duration;
    if (bufferIndex == bufferSize) {
      break;
    }
  }

  return bufferIndex;
}

void TraceLoggerThread::logTimestamp(TraceLoggerTextId id) {
  logTimestamp(uint32_t(id));
}

void TraceLoggerThread::logTimestamp(uint32_t id) {
  MOZ_ASSERT(id > TraceLogger_TreeItemEnd && id < TraceLogger_Last);
  log(id);
}

void TraceLoggerThread::log(uint32_t id) {
  if (enabled_ == 0) {
    return;
  }

#ifdef DEBUG
  if (id == TraceLogger_Disable) {
    graphStack_.clear();
  }
#endif

  MOZ_ASSERT(traceLoggerState);

  // We request for 3 items to add, since if we don't have enough room
  // we record the time it took to make more space. To log this information
  // we need 2 extra free entries.
  if (!events_.hasSpaceForAdd(3)) {
    mozilla::TimeStamp start = mozilla::TimeStamp::NowUnfuzzed();

    if (!events_.ensureSpaceBeforeAdd(3)) {
      if (graph_.get()) {
        graph_->log(events_, traceLoggerState->startTime);
      }

      // The data structures are full, and the graph file is not enabled
      // so we cannot flush to disk.  Trace logging should stop here.
      if (!traceLoggerState->isGraphFileEnabled()) {
        enabled_ = 0;
        return;
      }

      iteration_++;
      events_.clear();

      // Periodically remove unused payloads from the global logger state.
      traceLoggerState->purgeUnusedPayloads();
    }

    // Log the time it took to flush the events_ as being from the
    // Tracelogger.
    if (graph_.get()) {
      MOZ_ASSERT(events_.hasSpaceForAdd(2));
      EventEntry& entryStart = events_.pushUninitialized();
      entryStart.time = start;
      entryStart.textId = TraceLogger_Internal;

      EventEntry& entryStop = events_.pushUninitialized();
      entryStop.time = mozilla::TimeStamp::NowUnfuzzed();
      entryStop.textId = TraceLogger_Stop;
    }
  }

  mozilla::TimeStamp time = mozilla::TimeStamp::NowUnfuzzed();

  EventEntry& entry = events_.pushUninitialized();
  entry.time = time;
  entry.textId = id;
}

bool TraceLoggerThreadState::remapDictionaryEntries(
    mozilla::Vector<UniqueChars, 0, SystemAllocPolicy>* newDictionary,
    uint32_t* newNextDictionaryId) {
  MOZ_ASSERT(newNextDictionaryId != nullptr && newDictionary != nullptr);

  typedef HashMap<uint32_t, uint32_t, DefaultHasher<uint32_t>,
                  SystemAllocPolicy>
      DictionaryMap;
  DictionaryMap dictionaryMap;

  // Clear all payloads that are not currently used.  There may be some events
  // that still hold a pointer to a payload.  Restarting the profiler may reuse
  // the exact same event as a previous session if it's still alive so we need
  // to maintain it's existence.
  for (TextIdToPayloadMap::Enum e(textIdPayloads); !e.empty(); e.popFront()) {
    if (e.front().value()->uses() == 0) {
      js_delete(e.front().value());
      e.removeFront();
    } else {
      TraceLoggerEventPayload* payload = e.front().value();
      uint32_t dictId = payload->dictionaryId();

      if (dictionaryMap.has(dictId)) {
        DictionaryMap::Ptr mapPointer = dictionaryMap.lookup(dictId);
        MOZ_ASSERT(mapPointer);
        payload->setDictionaryId(mapPointer->value());
      } else {
        if (!newDictionary->append(std::move(dictionaryData[dictId]))) {
          return false;
        }
        payload->setDictionaryId(*newNextDictionaryId);

        if (!dictionaryMap.putNew(dictId, *newNextDictionaryId)) {
          return false;
        }

        (*newNextDictionaryId)++;
      }
    }
  }

  return true;
}

void TraceLoggerThreadState::clear() {
  LockGuard<Mutex> guard(lock);

  uint32_t newNextDictionaryId = 0;
  mozilla::Vector<UniqueChars, 0, SystemAllocPolicy> newDictionary;
  if (remapDictionaryEntries(&newDictionary, &newNextDictionaryId)) {
    // Clear and free any data used for the string dictionary.
    for (auto range = dictionaryData.all(); !range.empty(); range.popFront()) {
      range.front().reset();
    }
    dictionaryData.clearAndFree();
    dictionaryData = std::move(newDictionary);

    payloadDictionary.clearAndCompact();

    nextTextId = TraceLogger_Last;
    nextDictionaryId = newNextDictionaryId;
  }

  for (TraceLoggerThread* logger : threadLoggers) {
    logger->clear();
  }
}

void TraceLoggerThread::clear() {
  if (graph_.get()) {
    graph_.reset();
  }

  graph_ = nullptr;

#ifdef DEBUG
  graphStack_.clear();
#endif

  if (!events_.reset()) {
    silentFail("Cannot reset event buffer.");
  }
}

TraceLoggerThreadState::~TraceLoggerThreadState() {
  while (TraceLoggerThread* logger = threadLoggers.popFirst()) {
    js_delete(logger);
  }

  threadLoggers.clear();

  for (TextIdToPayloadMap::Range r = textIdPayloads.all(); !r.empty();
       r.popFront()) {
    js_delete(r.front().value());
  }

#ifdef DEBUG
  initialized = false;
#endif
}

bool TraceLoggerThreadState::init() {
  const char* env = getenv("TLLOG");
  if (env) {
    if (strstr(env, "help")) {
      fflush(nullptr);
      printf(
          "\n"
          "usage: TLLOG=option,option,option,... where options can be:\n"
          "\n"
          "Collections:\n"
          "  Default        Output all default. It includes:\n"
          "                 AnnotateScripts, Bailout, Baseline, "
          "BaselineCompilation, GC,\n"
          "                 GCAllocation, GCSweeping, Interpreter, "
          "IonAnalysis, IonCompilation,\n"
          "                 IonLinking, IonMonkey, MinorGC, Frontend, "
          "ParsingFull,\n"
          "                 ParsingSyntax, BytecodeEmission, IrregexpCompile, "
          "IrregexpExecute,\n"
          "                 Scripts, Engine, WasmCompilation\n"
          "\n"
          "  IonCompiler    Output all information about compilation. It "
          "includes:\n"
          "                 IonCompilation, IonLinking, PruneUnusedBranches, "
          "FoldTests,\n"
          "                 SplitCriticalEdges, RenumberBlocks, "
          "ScalarReplacement,\n"
          "                 DominatorTree, PhiAnalysis, MakeLoopsContiguous, "
          "ApplyTypes,\n"
          "                 EagerSimdUnbox, AliasAnalysis, GVN, LICM, "
          "RangeAnalysis,\n"
          "                 LoopUnrolling, FoldLinearArithConstants, "
          "EffectiveAddressAnalysis,\n"
          "                 AlignmentMaskAnalysis, EliminateDeadCode, "
          "ReorderInstructions,\n"
          "                 EdgeCaseAnalysis, EliminateRedundantChecks,\n"
          "                 AddKeepAliveInstructions, GenerateLIR, "
          "RegisterAllocation,\n"
          "                 GenerateCode, Scripts\n"
          "\n"
          "  VMSpecific     Output the specific name of the VM call\n"
          "\n"
          "  Frontend       Output all information about frontend compilation. "
          "It includes:\n"
          "                 Frontend, ParsingFull, ParsingSyntax, Tokenizing,\n"
          "                 BytecodeEmission, BytecodeFoldConstants, "
          "BytecodeNameFunctions\n"
          "Specific log items:\n");
      for (uint32_t i = 1; i < TraceLogger_Last; i++) {
        TraceLoggerTextId id = TraceLoggerTextId(i);
        if (!TLTextIdIsTogglable(id)) {
          continue;
        }
        printf("  %s\n", TLTextIdString(id));
      }
      printf("\n");
      exit(0);
      /*NOTREACHED*/
    }

    for (uint32_t i = 1; i < TraceLogger_Last; i++) {
      TraceLoggerTextId id = TraceLoggerTextId(i);
      if (TLTextIdIsTogglable(id)) {
        enabledTextIds[i] = ContainsFlag(env, TLTextIdString(id));
      } else {
        enabledTextIds[i] = true;
      }
    }

    if (ContainsFlag(env, "Default")) {
      enableDefaultLogging();
    }

    if (ContainsFlag(env, "IonCompiler")) {
      enableIonLogging();
    }

    if (ContainsFlag(env, "Frontend")) {
      enableFrontendLogging();
    }

#ifdef DEBUG
    enabledTextIds[TraceLogger_Error] = true;
#endif

  } else {
    // Most of the textId's will be enabled through JS::StartTraceLogger when
    // the gecko profiler is started.
    disableTextIdsForProfiler();
  }

  enabledTextIds[TraceLogger_Interpreter] = enabledTextIds[TraceLogger_Engine];
  enabledTextIds[TraceLogger_Baseline] = enabledTextIds[TraceLogger_Engine];
  enabledTextIds[TraceLogger_IonMonkey] = enabledTextIds[TraceLogger_Engine];

  enabledTextIds[TraceLogger_Error] = true;

  const char* options = getenv("TLOPTIONS");
  if (options) {
    if (strstr(options, "help")) {
      fflush(nullptr);
      printf(
          "\n"
          "usage: TLOPTIONS=option,option,option,... where options can be:\n"
          "\n"
          "  EnableMainThread        Start logging main threads immediately.\n"
          "  EnableOffThread         Start logging helper threads "
          "immediately.\n"
          "  EnableGraph             Enable the tracelogging graph.\n"
          "  EnableGraphFile         Enable flushing tracelogger data to a "
          "file.\n"
          "  Errors                  Report errors during tracing to "
          "stderr.\n");
      printf("\n");
      exit(0);
      /*NOTREACHED*/
    }

    if (strstr(options, "EnableMainThread")) {
      mainThreadEnabled = true;
    }
    if (strstr(options, "EnableOffThread")) {
      helperThreadEnabled = true;
    }
    if (strstr(options, "EnableGraph")) {
      graphEnabled = true;
    }
    if (strstr(options, "EnableGraphFile")) {
      graphFileEnabled = true;
      jit::JitOptions.enableTraceLogger = true;
    }
    if (strstr(options, "Errors")) {
      spewErrors = true;
    }
  } else {
    mainThreadEnabled = true;
    helperThreadEnabled = true;
    graphEnabled = false;
    graphFileEnabled = false;
    spewErrors = false;
  }

  startTime = mozilla::TimeStamp::NowUnfuzzed();

#ifdef DEBUG
  initialized = true;
#endif

  return true;
}

void TraceLoggerThreadState::enableTextId(JSContext* cx, uint32_t textId) {
  MOZ_ASSERT(TLTextIdIsTogglable(textId));

  if (enabledTextIds[textId]) {
    return;
  }

  ReleaseAllJITCode(cx->runtime()->defaultFreeOp());

  enabledTextIds[textId] = true;
  if (textId == TraceLogger_Engine) {
    enabledTextIds[TraceLogger_IonMonkey] = true;
    enabledTextIds[TraceLogger_Baseline] = true;
    enabledTextIds[TraceLogger_Interpreter] = true;
  }

  if (textId == TraceLogger_Scripts) {
    jit::ToggleBaselineTraceLoggerScripts(cx->runtime(), true);
  }
  if (textId == TraceLogger_Engine) {
    jit::ToggleBaselineTraceLoggerEngine(cx->runtime(), true);
  }
}
void TraceLoggerThreadState::disableTextId(JSContext* cx, uint32_t textId) {
  MOZ_ASSERT(TLTextIdIsTogglable(textId));

  if (!enabledTextIds[textId]) {
    return;
  }

  ReleaseAllJITCode(cx->runtime()->defaultFreeOp());

  enabledTextIds[textId] = false;
  if (textId == TraceLogger_Engine) {
    enabledTextIds[TraceLogger_IonMonkey] = false;
    enabledTextIds[TraceLogger_Baseline] = false;
    enabledTextIds[TraceLogger_Interpreter] = false;
  }

  if (textId == TraceLogger_Scripts) {
    jit::ToggleBaselineTraceLoggerScripts(cx->runtime(), false);
  }
  if (textId == TraceLogger_Engine) {
    jit::ToggleBaselineTraceLoggerEngine(cx->runtime(), false);
  }
}

TraceLoggerThread* js::TraceLoggerForCurrentThread(JSContext* maybecx) {
  if (!traceLoggerState) {
    return nullptr;
  }
  return traceLoggerState->forCurrentThread(maybecx);
}

TraceLoggerThread* TraceLoggerThreadState::forCurrentThread(
    JSContext* maybecx) {
  if (!jit::JitOptions.enableTraceLogger) {
    return nullptr;
  }

  MOZ_ASSERT(initialized);
  MOZ_ASSERT_IF(maybecx, maybecx == TlsContext.get());

  JSContext* cx = maybecx ? maybecx : TlsContext.get();
  if (!cx) {
    return nullptr;
  }

  if (!cx->traceLogger) {
    LockGuard<Mutex> guard(lock);

    TraceLoggerThread* logger = js_new<TraceLoggerThread>(cx);
    if (!logger) {
      return nullptr;
    }

    if (!logger->init()) {
      return nullptr;
    }

    threadLoggers.insertFront(logger);
    cx->traceLogger = logger;

    if (graphEnabled) {
      logger->initGraph();
    }

    if (cx->isHelperThreadContext() ? helperThreadEnabled : mainThreadEnabled) {
      logger->enable();
    }
  }

  return cx->traceLogger;
}

void TraceLoggerThreadState::destroyLogger(TraceLoggerThread* logger) {
  MOZ_ASSERT(initialized);
  MOZ_ASSERT(logger);
  LockGuard<Mutex> guard(lock);

  logger->remove();
  js_delete(logger);
}

bool js::TraceLogTextIdEnabled(uint32_t textId) {
  if (!traceLoggerState) {
    return false;
  }
  return traceLoggerState->isTextIdEnabled(textId);
}

void js::TraceLogEnableTextId(JSContext* cx, uint32_t textId) {
  if (!traceLoggerState) {
    return;
  }
  traceLoggerState->enableTextId(cx, textId);
}
void js::TraceLogDisableTextId(JSContext* cx, uint32_t textId) {
  if (!traceLoggerState) {
    return;
  }
  traceLoggerState->disableTextId(cx, textId);
}

TraceLoggerEvent::TraceLoggerEvent(TraceLoggerTextId type, JSScript* script)
    : TraceLoggerEvent(type, script->filename(), script->lineno(),
                       script->column()) {}

TraceLoggerEvent::TraceLoggerEvent(TraceLoggerTextId type, const char* filename,
                                   uint32_t line, uint32_t column)
    : payload_() {
  MOZ_ASSERT(
      type == TraceLogger_Scripts || type == TraceLogger_AnnotateScripts ||
      type == TraceLogger_InlinedScripts || type == TraceLogger_Frontend);

  if (!traceLoggerState || !jit::JitOptions.enableTraceLogger) {
    return;
  }

  // Only log scripts when enabled, otherwise use the more generic type
  // (which will get filtered out).
  if (!traceLoggerState->isTextIdEnabled(type)) {
    payload_.setTextId(type);
    return;
  }

  payload_.setEventPayload(
      traceLoggerState->getOrCreateEventPayload(filename, line, column));
}

TraceLoggerEvent::TraceLoggerEvent(const char* text) : payload_() {
  if (jit::JitOptions.enableTraceLogger && traceLoggerState) {
    payload_.setEventPayload(traceLoggerState->getOrCreateEventPayload(text));
  }
}

TraceLoggerEvent::~TraceLoggerEvent() {
  if (hasExtPayload()) {
    extPayload()->release();
  }
}

uint32_t TraceLoggerEvent::textId() const {
  MOZ_ASSERT(hasTextId());
  if (hasExtPayload()) {
    return extPayload()->textId();
  }
  return payload_.textId();
}

TraceLoggerEvent& TraceLoggerEvent::operator=(const TraceLoggerEvent& other) {
  if (other.hasExtPayload()) {
    other.extPayload()->use();
  }
  if (hasExtPayload()) {
    extPayload()->release();
  }

  payload_ = other.payload_;

  return *this;
}

TraceLoggerEvent::TraceLoggerEvent(const TraceLoggerEvent& other)
    : payload_(other.payload_) {
  if (hasExtPayload()) {
    extPayload()->use();
  }
}

JS_PUBLIC_API bool JS::InitTraceLogger() {
  MOZ_RELEASE_ASSERT(!traceLoggerState);

  if (!getTraceLoggerSupported()) {
    return true;
  }

  traceLoggerState = js_new<TraceLoggerThreadState>();
  if (!traceLoggerState) {
    return false;
  }

  if (!traceLoggerState->init()) {
    DestroyTraceLoggerThreadState();
    return false;
  }

  return true;
}

JS_PUBLIC_API bool JS::TraceLoggerSupported() { return traceLoggerState; }

// Perform a process wide synchronous spew of every thread that tracelogger has
// captured.
void TraceLoggerThreadState::spewTraceLoggerStats() {
  for (TraceLoggerThread* logger : threadLoggers) {
    logger->spewTraceLoggerStats();
  }
}

// Usage here is JS_TRACELOGGER_SPEW=<event1>,<event2>,etc for custom spewing.
// If the environment variable is not found, we use a default set of events.
static bool getSpewIds(EventVector& spewIds) {
  const char* env = getenv("JS_TRACELOGGER_SPEW");
  if (env) {
    for (uint32_t i = 1; i < TraceLogger_Last; i++) {
      TraceLoggerTextId id = TraceLoggerTextId(i);
      if (ContainsFlag(env, TLTextIdString(id))) {
        if (!spewIds.append(id)) {
          return false;
        }
      }
    }
  } else {
    const uint32_t defaultSpewEvents[] = {
        TraceLogger_ParsingFull, TraceLogger_Interpreter,
        TraceLogger_Baseline,    TraceLogger_BaselineCompilation,
        TraceLogger_IonMonkey,   TraceLogger_IonCompilation,
        TraceLogger_Bailout};

    for (uint32_t id : defaultSpewEvents) {
      if (!spewIds.append(id)) {
        return false;
      }
    }
  }

  return true;
}

static void spewHeaderRow(UniqueChars& threadName, EventVector& spewIds) {
  if (threadName) {
    JitSpew(jit::JitSpew_ScriptStats, "Thread: %s (pid=%d)", threadName.get(),
            getpid());
  } else {
    JitSpew(jit::JitSpew_ScriptStats, "Unknown Thread (pid=%d)", getpid());
  }

  UniqueChars header = JS_smprintf("%10s ", "totalTime");
  for (uint32_t i : spewIds) {
    TraceLoggerTextId id = TraceLoggerTextId(i);
    if (TLTextIdIsLogEvent(id)) {
      header =
          JS_sprintf_append(std::move(header), "%12s ", TLTextIdString(id));
    } else {
      header =
          JS_sprintf_append(std::move(header), "%25s ", TLTextIdString(id));
    }
  }

  JitSpew(jit::JitSpew_ScriptStats, "%s Script", header.get());
}

void TraceLoggerThread::spewTraceLoggerStats() {
  if (!jit::JitOptions.enableTraceLogger) {
    return;
  }

  ScriptMap map;
  if (!collectTraceLoggerStats(map)) {
    return;
  }
  if (map.empty()) {
    return;
  }

  SortedStatsVector sorted_map;
  if (!sortTraceLoggerStats(map, sorted_map)) {
    return;
  }
  map.clearAndCompact();

  EventVector spewIds;
  if (!getSpewIds(spewIds)) {
    return;
  }

  // Dynamically generate the header row in JitSpew.
  spewHeaderRow(threadName_, spewIds);

  for (UniquePtr<ScriptStats>& datap : sorted_map) {
    auto& tlevents = datap->events_;
    uint32_t selfTime = datap->selfTime;

    if (selfTime == 0) {
      continue;
    }

    UniqueChars row = JS_smprintf("%10u ", selfTime);
    for (uint32_t i : spewIds) {
      TraceLoggerTextId id = TraceLoggerTextId(i);
      uint32_t time = tlevents[id].time;
      uint32_t freq = tlevents[id].count;
      uint32_t percent = (time * 100) / selfTime;
      if (TLTextIdIsLogEvent(id)) {
        row = JS_sprintf_append(std::move(row), "%12u ", freq);
      } else {
        row = JS_sprintf_append(std::move(row), "%8u (%3u%%,f=%-7u) ", time,
                                percent, freq);
      }
    }
    JitSpew(jit::JitSpew_ScriptStats, "%s %s", row.get(), datap->scriptName);

    // If structured spewer is enabled, we might as well spew everything.
    AutoStructuredSpewer spew(cx_, SpewChannel::ScriptStats, nullptr);
    if (spew) {
      spew->property("script", datap->scriptName);
      spew->property("totalTime", selfTime);
      spew->beginListProperty("events");
      for (uint32_t i = 1; i < TraceLogger_Last; i++) {
        TraceLoggerTextId id = TraceLoggerTextId(i);
        if (TLTextIdIsInternalEvent(id) || tlevents[id].count == 0) {
          continue;
        }

        spew->beginObject();
        spew->property("id", TLTextIdString(id));
        if (TLTextIdIsTreeEvent(id)) {
          spew->property("time", tlevents[id].time);
          spew->property("frequency", tlevents[id].count);
        } else if (TLTextIdIsLogEvent(id)) {
          spew->property("frequency", tlevents[id].count);
        }
        spew->endObject();
      }
      spew->endList();
    }
  }
}

static bool updateScriptMap(ScriptMap& map, char* key, uint32_t eventId,
                            uint32_t value) {
  if (!key) {
    return false;
  }

  if (!map.has(key)) {
    UniquePtr<ScriptStats> datap;
    datap.reset(js_new<ScriptStats>(key));
    if (!map.putNew(key, std::move(datap))) {
      return false;
    }
  }
  ScriptMap::Ptr p = map.lookup(key);
  p->value()->events_[eventId].time += value;
  p->value()->events_[eventId].count++;

  if (TLTextIdIsTreeEvent(eventId)) {
    p->value()->selfTime += value;
  }
  return true;
}

UniqueChars TraceLoggerThreadState::getFullScriptName(uint32_t textId) {
  TraceLoggerEventPayload* payload = getPayload(textId);
  MOZ_ASSERT(payload);
  if (!payload) {
    return nullptr;
  }
  char* filename = dictionaryData[payload->dictionaryId()].get();
  uint32_t lineno = payload->line() ? *(payload->line()) : 0;
  uint32_t colno = payload->column() ? *(payload->column()) : 0;
  UniqueChars scriptName = JS_smprintf("%s:%u:%u", filename, lineno, colno);
  payload->release();
  return scriptName;
}

static bool sortBySelfTime(const UniquePtr<ScriptStats>& lhs,
                           const UniquePtr<ScriptStats>& rhs) {
  return lhs.get()->selfTime > rhs.get()->selfTime;
}

bool TraceLoggerThread::sortTraceLoggerStats(ScriptMap& map,
                                             SortedStatsVector& sorted_map) {
  for (auto range = map.all(); !range.empty(); range.popFront()) {
    if (!sorted_map.append(std::move(range.front().value()))) {
      return false;
    }
  }

  std::sort(sorted_map.begin(), sorted_map.end(), sortBySelfTime);

  return true;
}

// Traverse each event and calculate the self-time, along with the script that
// each event belongs to. We do this quickly by maintaining two stacks:
//  (i) eventStack - Each new event encountered is pushed onto the stack. Events
//                   are popped off whenever a TraceLogger_Stop is encountered
//                   and sent to updateScriptMap.
// (ii) funcStack - Each new script encountered is pushed onto this stack.
//                  Elements are popped off whenever a TraceLogger_Stop is
//                  encountered that matches a script event on the top of
//                  eventStack.
bool TraceLoggerThread::collectTraceLoggerStats(ScriptMap& map) {
  uint32_t totalJSTime = 0;

  struct eventInfo {
    uint32_t textId;
    uint32_t time;
    mozilla::TimeStamp start;

    explicit eventInfo(uint32_t textId_) : textId(textId_), time(0) {}
  };

  Vector<eventInfo*, 0, js::SystemAllocPolicy> eventStack;
  Vector<uint32_t, 0, js::SystemAllocPolicy> funcStack;

  mozilla::TimeStamp startTime, stopTime;
  uint32_t size = events_.size();
  for (size_t i = 0; i < size; i++) {
    uint32_t textId = events_[i].textId;

    // Record any log events that have no durations such as Bailouts with a
    // value of 1.  Make sure the funcStack actually has something in it or
    // else the Bailout event will not be associated with any script.  This
    // can commonly occur when profiling & tracing starts since we may have
    // already past the point where the script event is created.
    if (TLTextIdIsLogEvent(textId) && !funcStack.empty()) {
      UniqueChars script =
          traceLoggerState->getFullScriptName(funcStack.back());
      if (!updateScriptMap(map, script.release(), textId, 1)) {
        return false;
      }
    }

    // Hit a new tree event or a stop event, so add (new event timestamp - old
    // event timestamp) to the old event's self-time.
    if (TLTextIdIsTreeEvent(textId)) {
      if (!eventStack.empty()) {
        stopTime = events_[i].time;

        uint32_t deltaTime =
            static_cast<uint32_t>((stopTime - startTime).ToMicroseconds());
        eventStack.back()->time += deltaTime;

        if (TLTextIdIsEnumEvent(eventStack.back()->textId)) {
          if (!funcStack.empty() && !eventStack.empty()) {
            UniqueChars script =
                traceLoggerState->getFullScriptName(funcStack.back());
            if (!updateScriptMap(map, script.release(),
                                 eventStack.back()->textId, deltaTime)) {
              return false;
            }
          }
        }
        totalJSTime += deltaTime;
      }

      if (TLTextIdIsScriptEvent(textId)) {
        if (!funcStack.append(textId)) {
          return false;
        }
      }

      eventInfo* entry = js_new<eventInfo>(textId);
      entry->start = events_[i].time;
      if (!eventStack.append(entry)) {
        return false;
      }

      startTime = events_[i].time;

    } else if (textId == TraceLogger_Stop) {
      if (!eventStack.empty()) {
        stopTime = events_[i].time;

        uint32_t deltaTime =
            static_cast<uint32_t>((stopTime - startTime).ToMicroseconds());
        eventInfo* entry = eventStack.popCopy();

        uint32_t topId = entry->textId;
        entry->time += deltaTime;

        if (TLTextIdIsEnumEvent(topId)) {
          // funcStack will usually be empty near the beginning of a profiling
          // session since we may have skipped the point where the script event
          // is created.  If that's the case, then skip this event since we
          // cannot associate it with any script.
          if (!funcStack.empty()) {
            UniqueChars script =
                traceLoggerState->getFullScriptName(funcStack.back());
            if (!updateScriptMap(map, script.release(), topId, deltaTime)) {
              return false;
            }
          }
        }
        js_delete(entry);

        if (TLTextIdIsScriptEvent(topId) && !funcStack.empty()) {
          funcStack.popBack();
        }

        totalJSTime += deltaTime;
        startTime = events_[i].time;
      }
    }
  }

  return true;
}

JS_PUBLIC_API void JS::ResetTraceLogger(void) { js::ResetTraceLogger(); }

JS_PUBLIC_API void JS::SpewTraceLoggerThread(JSContext* cx) {
  if (!traceLoggerState) {
    return;
  }

  if (cx && cx->traceLogger) {
    cx->traceLogger->spewTraceLoggerStats();
  }
}

JS_PUBLIC_API void JS::SpewTraceLoggerForCurrentProcess() {
  if (!traceLoggerState) {
    return;
  }
  traceLoggerState->spewTraceLoggerStats();
}

JS_PUBLIC_API void JS::StartTraceLogger(JSContext* cx) {
  if (!traceLoggerState) {
    return;
  }

  if (!jit::JitOptions.enableTraceLogger) {
    LockGuard<Mutex> guard(traceLoggerState->lock);
    traceLoggerState->enableTextIdsForProfiler();
    jit::JitOptions.enableTraceLogger = true;
    traceLoggerState->startTime = mozilla::TimeStamp::Now();
  }

  TraceLoggerThread* logger = traceLoggerState->forCurrentThread(cx);
  if (logger) {
    logger->enable();
  }
}

JS_PUBLIC_API void JS::StopTraceLogger(JSContext* cx) {
  if (!traceLoggerState) {
    return;
  }

  if (jit::JitOptions.enableTraceLogger) {
    LockGuard<Mutex> guard(traceLoggerState->lock);
    traceLoggerState->disableTextIdsForProfiler();
    jit::JitOptions.enableTraceLogger = false;
  }

  TraceLoggerThread* logger = traceLoggerState->forCurrentThread(cx);
  if (logger) {
    logger->disable();
  }
}
