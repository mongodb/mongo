/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TraceLogging_h
#define TraceLogging_h

#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Vector.h"

#include <utility>

#include "jsapi.h"
#include "jsfriendapi.h"

#include "js/AllocPolicy.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/HashTable.h"
#include "js/TraceLoggerAPI.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"
#include "js/Vector.h"
#include "threading/LockGuard.h"
#include "vm/MutexIDs.h"
#include "vm/TraceLoggingGraph.h"
#include "vm/TraceLoggingTypes.h"

namespace js {

namespace jit {
class CompileRuntime;
}  // namespace jit

/*
 * [SMDOC] Tracelogging overview.
 *
 * Tracelogging makes it possible to trace the occurrence of a single event
 * and/or the start and stop of an event. This is implemented with as low
 * overhead as possible to not interfere with running.
 *
 * Logging something is done in 3 stages.
 * 1) Get the tracelogger of the current thread. cx may be omitted, in which
 *    case it will be fetched from TLS.
 *     - TraceLoggerForCurrentThread(cx);
 *
 * 2) Optionally create a TraceLoggerEvent for the text that needs to get
 *    logged. This step takes some time, so try to do this beforehand, outside
 *    the hot path and don't do unnecessary repetitions, since it will cripple
 *    performance.
 *     - TraceLoggerEvent event(logger, "foo");
 *
 *    There are also some predefined events. They are located in
 *    TraceLoggerTextId. They don't require to create an TraceLoggerEvent and
 *    can also be used as an argument to these functions.
 *
 * 3) Log the occurrence of a single event:
 *    - TraceLogTimestamp(logger, TraceLoggerTextId);
 *      Note: it is temporarily not supported to provide an TraceLoggerEvent as
 *            argument to log the occurrence of a single event.
 *
 *    or log the start and stop of an event:
 *    - TraceLogStartEvent(logger, TraceLoggerTextId);
 *    - TraceLogStartEvent(logger, TraceLoggerEvent);
 *    - TraceLogStopEvent(logger, TraceLoggerTextId);
 *    - TraceLogStopEvent(logger, TraceLoggerEvent);
 *
 *    or the start/stop of an event with a RAII class:
 *    - AutoTraceLog atl(logger, TraceLoggerTextId);
 *    - AutoTraceLog atl(logger, TraceLoggerEvent);
 */

class AutoTraceLog;
class TraceLoggerEventPayload;
class TraceLoggerThread;

/**
 * An event that can be used to report start/stop events to TraceLogger. It
 * prepares the given info by requesting a TraceLoggerEventPayload containing
 * the string to report and an unique id. It also increases the useCount of
 * this payload, so it cannot get removed.
 */
class TraceLoggerEvent {
#ifdef JS_TRACE_LOGGING
 private:
  class EventPayloadOrTextId {
    /**
     * Payload can be a pointer to a TraceLoggerEventPayload* or a
     * TraceLoggerTextId. The last bit decides how to read the payload.
     *
     * payload_ = [                   | 0 ]
     *             -----------------------
     *             TraceLoggerEventPayload* (incl. last bit)
     *
     * payload_ = [                   | 1 ]
     *             -------------------
     *             TraceLoggerTextId (excl. last bit)
     */
    uintptr_t payload_;

   public:
    EventPayloadOrTextId() : payload_(0) {}

    bool isEventPayload() const { return (payload_ & 1) == 0; }
    TraceLoggerEventPayload* eventPayload() const {
      MOZ_ASSERT(isEventPayload());
      return (TraceLoggerEventPayload*)payload_;
    }
    void setEventPayload(TraceLoggerEventPayload* payload) {
      MOZ_ASSERT(payload);
      payload_ = (uintptr_t)payload;
      MOZ_ASSERT((payload_ & 1) == 0);
    }
    bool isTextId() const { return (payload_ & 1) == 1; }
    uint32_t textId() const {
      MOZ_ASSERT(isTextId());
      return payload_ >> 1;
    }
    void setTextId(TraceLoggerTextId textId) {
      static_assert(TraceLogger_Last < (UINT32_MAX >> 1),
                    "Too many predefined text ids.");
      payload_ = (((uint32_t)textId) << 1) | 1;
    }
  };

  EventPayloadOrTextId payload_;

 public:
  TraceLoggerEvent() : payload_() {}
  explicit TraceLoggerEvent(TraceLoggerTextId textId);
  TraceLoggerEvent(TraceLoggerTextId type, JSScript* script);
  TraceLoggerEvent(TraceLoggerTextId type, const char* filename, uint32_t line,
                   uint32_t column);
  explicit TraceLoggerEvent(const char* text);
  TraceLoggerEvent(const TraceLoggerEvent& event);
  TraceLoggerEvent& operator=(const TraceLoggerEvent& other);
  ~TraceLoggerEvent();
  uint32_t textId() const;
  bool hasTextId() const { return hasExtPayload() || payload_.isTextId(); }

 private:
  TraceLoggerEventPayload* extPayload() const {
    MOZ_ASSERT(hasExtPayload());
    return payload_.eventPayload();
  }
  bool hasExtPayload() const {
    return payload_.isEventPayload() && !!payload_.eventPayload();
  }
#else
 public:
  TraceLoggerEvent() {}
  explicit TraceLoggerEvent(TraceLoggerTextId textId) {}
  TraceLoggerEvent(TraceLoggerTextId type, JSScript* script) {}
  TraceLoggerEvent(TraceLoggerTextId type, const char* filename, uint32_t line,
                   uint32_t column) {}
  explicit TraceLoggerEvent(const char* text) {}
  TraceLoggerEvent(const TraceLoggerEvent& event) {}
  TraceLoggerEvent& operator=(const TraceLoggerEvent& other) { return *this; };
  ~TraceLoggerEvent() {}
  uint32_t textId() const { return 0; }
  bool hasTextId() const { return false; }
#endif
};

#ifdef DEBUG
void AssertCurrentThreadOwnsTraceLoggerThreadStateLock();
#endif

/**
 * An internal class holding the string information to report, together with an
 * unique id, and an useCount. Whenever this useCount reaches 0, this event
 * cannot get started/stopped anymore. Consumers may still request the
 * string information through maybeEventText below, but this may not succeed:
 * when the use count becomes zero, a payload may be deleted by any thread
 * holding the TraceLoggerThreadState lock, after that the pointers have been
 * cleared out of the dictionaryVector.
 */
class TraceLoggerEventPayload {
  uint32_t textId_;
  uint32_t dictionaryId_;
  mozilla::Maybe<uint32_t> line_;
  mozilla::Maybe<uint32_t> col_;
  mozilla::Atomic<uint32_t> uses_;

 public:
  TraceLoggerEventPayload(uint32_t textId, uint32_t dictionaryId)
      : textId_(textId),
        dictionaryId_(dictionaryId),
        line_(mozilla::Nothing()),
        col_(mozilla::Nothing()),
        uses_(0) {}

  ~TraceLoggerEventPayload() { MOZ_ASSERT(uses_ == 0); }

  void setLine(uint32_t line) { line_ = mozilla::Some(line); }
  void setColumn(uint32_t col) { col_ = mozilla::Some(col); }

  mozilla::Maybe<uint32_t> line() { return line_; }
  mozilla::Maybe<uint32_t> column() { return col_; }
  uint32_t textId() { return textId_; }
  uint32_t dictionaryId() { return dictionaryId_; }
  void setDictionaryId(uint32_t dictId) { dictionaryId_ = dictId; }
  uint32_t uses() { return uses_; }

  // Payloads may have their use count change at any time, *except* the count
  // can only go from zero to non-zero while the thread state lock is held.
  // This should only happen under getOrCreateEventPayload below, and avoids
  // races with purgeUnusedPayloads.
  void use() {
#ifdef DEBUG
    if (!uses_) {
      AssertCurrentThreadOwnsTraceLoggerThreadStateLock();
    }
#endif
    uses_++;
  }
  void release() { uses_--; }
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this);
  }
};

// Data structures used to collect per function statistics for spewing
struct ScriptStats {
  struct EventData {
    uint32_t time;
    uint32_t count;
  };

  EventData events_[TraceLogger_Last];

  char* scriptName;
  uint32_t selfTime;

  explicit ScriptStats(char* scriptName_)
      : scriptName(scriptName_), selfTime(0) {
    for (EventData& entry : events_) {
      entry.time = 0;
      entry.count = 0;
    }
  }

  ~ScriptStats() {
    JS::FreePolicy freePolicy;
    freePolicy(scriptName);
  }
};

typedef HashMap<char*, UniquePtr<ScriptStats>, mozilla::CStringHasher,
                SystemAllocPolicy>
    ScriptMap;

typedef Vector<UniquePtr<ScriptStats>, 0, SystemAllocPolicy> SortedStatsVector;
typedef Vector<uint32_t, 0, js::SystemAllocPolicy> EventVector;

// Per thread trace logger state.
class TraceLoggerThread : public mozilla::LinkedListElement<TraceLoggerThread> {
#ifdef JS_TRACE_LOGGING
  friend JS::TraceLoggerIdImpl;
  friend JS::TraceLoggerTimeStampImpl;
  friend JS::TraceLoggerDurationImpl;
  friend JS::TraceLoggerLineNoImpl;
  friend JS::TraceLoggerColNoImpl;

 private:
  JSContext* cx_;
  uint32_t enabled_ = 0;
  bool failed_ = false;

  UniquePtr<TraceLoggerGraph> graph_;
  ContinuousSpace<EventEntry> events_;

  // Every time the events get flushed, this count is increased by one.
  // Together with events.lastEntryId(), this gives an unique id for every
  // event.
  uint32_t iteration_ = 0;

#  ifdef DEBUG
  EventVector graphStack_;
#  endif

  JS::UniqueChars threadName_ = nullptr;

 public:
  AutoTraceLog* top_ = nullptr;

  explicit TraceLoggerThread(JSContext* cx) : cx_(cx) {}

  bool init();
  ~TraceLoggerThread();

  bool init(uint32_t loggerId);
  void initGraph();

  void clear();

  bool enable();
  bool enable(JSContext* cx);
  bool disable(bool force = false, const char* = "");
  bool enabled() { return enabled_ > 0; }

  void silentFail(const char* error);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

 private:
  bool fail(JSContext* cx, const char* error);

 public:
  // Given the previous iteration and size, return an array of events
  // (there could be lost events). At the same time update the iteration and
  // size and gives back how many events there are.
  EventEntry* getEventsStartingAt(uint32_t* lastIteration, uint32_t* lastSize,
                                  size_t* num) {
    EventEntry* start;
    if (iteration_ == *lastIteration) {
      MOZ_ASSERT(*lastSize <= events_.size());
      *num = events_.size() - *lastSize;
      start = events_.data() + *lastSize;
    } else {
      *num = events_.size();
      start = events_.data();
    }

    getIterationAndSize(lastIteration, lastSize);
    return start;
  }

  void getIterationAndSize(uint32_t* iteration, uint32_t* size) const {
    *iteration = iteration_;
    *size = events_.size();
  }

  bool lostEvents(uint32_t lastIteration, uint32_t lastSize) {
    // If still logging in the same iteration, there are no lost events.
    if (lastIteration == iteration_) {
      MOZ_ASSERT(lastSize <= events_.size());
      return false;
    }

    // If we are in the next consecutive iteration we are only sure we
    // didn't lose any events when the lastSize equals the maximum size
    // 'events' can get.
    if (lastIteration == iteration_ - 1 && lastSize == events_.maxSize()) {
      return false;
    }

    return true;
  }

  bool collectTraceLoggerStats(ScriptMap& map);
  bool sortTraceLoggerStats(ScriptMap& map, SortedStatsVector& sorted_map);
  void spewTraceLoggerStats();

 private:
  const char* maybeEventText(uint32_t id);

 public:
  const char* eventText(uint32_t id) {
    const char* text = maybeEventText(id);
    MOZ_ASSERT(text);
    return text;
  };

 public:
  // Log an event (no start/stop, only the timestamp is recorded).
  void logTimestamp(TraceLoggerTextId id);

  // Record timestamps for start and stop of an event.
  void startEvent(TraceLoggerTextId id);
  void startEvent(const TraceLoggerEvent& event);
  void stopEvent(TraceLoggerTextId id);
  void stopEvent(const TraceLoggerEvent& event);

  // These functions are actually private and shouldn't be used in normal
  // code. They are made public so they can be used in assembly.
  void logTimestamp(uint32_t id);
  void startEvent(uint32_t id);
  void stopEvent(uint32_t id);

 private:
  void stopEvent();
  void log(uint32_t id);

 public:
  static unsigned offsetOfEnabled() {
    return offsetof(TraceLoggerThread, enabled_);
  }
#endif
};

// Process wide trace logger state.
class TraceLoggerThreadState {
#ifdef JS_TRACE_LOGGING
  friend JS::TraceLoggerDictionaryImpl;
#  ifdef DEBUG
  bool initialized = false;
#  endif

  bool enabledTextIds[TraceLogger_Last];
  bool mainThreadEnabled = false;
  bool helperThreadEnabled = false;
  bool graphEnabled = false;
  bool graphFileEnabled = false;
  bool spewErrors = false;
  mozilla::LinkedList<TraceLoggerThread> threadLoggers;

  // Any events that carry a payload are saved in this hash map.
  // The key is the event textId, and the value is a pointer to
  // the payload object.
  typedef HashMap<uint32_t, TraceLoggerEventPayload*, DefaultHasher<uint32_t>,
                  SystemAllocPolicy>
      TextIdToPayloadMap;

  // The dictionary vector is used to store all of the custom event strings
  // that are referenced by the payload objects.
  typedef mozilla::Vector<JS::UniqueChars, 0, SystemAllocPolicy>
      DictionaryVector;

  // All payload strings are hashed and saved as a key in the payloadDictionary
  // hash table.  The values are indices to the dictionaryData vector where the
  // actual string is stored.  The major benefit of having this hash map is for
  // de-duplication of JS script filenames.
  typedef HashMap<const char*, uint32_t, mozilla::CStringHasher,
                  SystemAllocPolicy>
      StringHashToDictionaryMap;

  TextIdToPayloadMap textIdPayloads;
  StringHashToDictionaryMap payloadDictionary;
  DictionaryVector dictionaryData;

  uint32_t nextTextId = TraceLogger_Last;
  uint32_t nextDictionaryId = 0;

 public:
  mozilla::TimeStamp startTime;

  double getTimeStampOffset(mozilla::TimeStamp time) {
    mozilla::TimeDuration delta = time - startTime;
    return delta.ToMicroseconds();
  }

  JS::UniqueChars getFullScriptName(uint32_t textId);

  // Mutex to guard the data structures used to hold the payload data:
  // textIdPayloads, payloadDictionary & dictionaryData.
  Mutex lock;

  TraceLoggerThreadState() : lock(js::mutexid::TraceLoggerThreadState) {}

  bool init();
  ~TraceLoggerThreadState();

  void enableDefaultLogging();
  void enableIonLogging();
  void enableFrontendLogging();

  void spewTraceLoggerStats();

  void clear();
  bool remapDictionaryEntries(
      mozilla::Vector<JS::UniqueChars, 0, SystemAllocPolicy>* newDictionary,
      uint32_t* newNextDictionaryId);

  TraceLoggerThread* forCurrentThread(JSContext* cx);
  void destroyLogger(TraceLoggerThread* logger);

  bool isTextIdEnabled(uint32_t textId) {
    if (textId < TraceLogger_Last) {
      return enabledTextIds[textId];
    }
    return true;
  }
  void enableTextId(JSContext* cx, uint32_t textId);
  void disableTextId(JSContext* cx, uint32_t textId);
  void maybeSpewError(const char* text) {
    if (spewErrors) {
      fprintf(stderr, "%s\n", text);
    }
  }

  const char* maybeEventText(uint32_t id);
  const char* maybeEventText(TraceLoggerEventPayload* p);

  void purgeUnusedPayloads();

  // These functions map a unique input to a logger ID.
  // This can be used to give start and stop events. Calls to these functions
  // should be limited if possible, because of the overhead. Note: it is not
  // allowed to use them in logTimestamp.
  TraceLoggerEventPayload* getOrCreateEventPayload(const char* text);
  TraceLoggerEventPayload* getOrCreateEventPayload(JSScript* script);
  TraceLoggerEventPayload* getOrCreateEventPayload(const char* filename,
                                                   uint32_t lineno,
                                                   uint32_t colno);
  TraceLoggerEventPayload* getPayload(uint32_t id);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }

  bool isGraphFileEnabled() { return graphFileEnabled; }
  bool isGraphEnabled() { return graphEnabled; }

  void enableTextIdsForProfiler();
  void disableTextIdsForProfiler();
  void disableAllTextIds();
#endif
};

#ifdef JS_TRACE_LOGGING
void ResetTraceLogger();
void DestroyTraceLoggerThreadState();
void DestroyTraceLogger(TraceLoggerThread* logger);

TraceLoggerThread* TraceLoggerForCurrentThread(JSContext* cx = nullptr);
#else
inline TraceLoggerThread* TraceLoggerForCurrentThread(JSContext* cx = nullptr) {
  return nullptr;
};
#endif

inline bool TraceLoggerEnable(TraceLoggerThread* logger) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    return logger->enable();
  }
#endif
  return false;
}
inline bool TraceLoggerEnable(TraceLoggerThread* logger, JSContext* cx) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    return logger->enable(cx);
  }
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_TRACELOGGER_ENABLE_FAIL, "internal error");
#endif
  return false;
}
inline bool TraceLoggerDisable(TraceLoggerThread* logger) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    return logger->disable();
  }
#endif
  return false;
}
inline void TraceLoggerSilentFail(TraceLoggerThread* logger,
                                  const char* error) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    logger->silentFail(error);
  }
#endif
}

#ifdef JS_TRACE_LOGGING
bool TraceLogTextIdEnabled(uint32_t textId);
void TraceLogEnableTextId(JSContext* cx, uint32_t textId);
void TraceLogDisableTextId(JSContext* cx, uint32_t textId);
#else
inline bool TraceLogTextIdEnabled(uint32_t textId) { return false; }
inline void TraceLogEnableTextId(JSContext* cx, uint32_t textId) {}
inline void TraceLogDisableTextId(JSContext* cx, uint32_t textId) {}
#endif
inline void TraceLogTimestamp(TraceLoggerThread* logger,
                              TraceLoggerTextId textId) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    logger->logTimestamp(textId);
  }
#endif
}
inline void TraceLogStartEvent(TraceLoggerThread* logger,
                               TraceLoggerTextId textId) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    logger->startEvent(textId);
  }
#endif
}
inline void TraceLogStartEvent(TraceLoggerThread* logger,
                               const TraceLoggerEvent& event) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    logger->startEvent(event);
  }
#endif
}
inline void TraceLogStopEvent(TraceLoggerThread* logger,
                              TraceLoggerTextId textId) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    logger->stopEvent(textId);
  }
#endif
}
inline void TraceLogStopEvent(TraceLoggerThread* logger,
                              const TraceLoggerEvent& event) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    logger->stopEvent(event);
  }
#endif
}

// Helper functions for assembly. May not be used otherwise.
inline void TraceLogTimestampPrivate(TraceLoggerThread* logger, uint32_t id) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    logger->logTimestamp(id);
  }
#endif
}
inline void TraceLogStartEventPrivate(TraceLoggerThread* logger, uint32_t id) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    logger->startEvent(id);
  }
#endif
}
inline void TraceLogStopEventPrivate(TraceLoggerThread* logger, uint32_t id) {
#ifdef JS_TRACE_LOGGING
  if (logger) {
    logger->stopEvent(id);
  }
#endif
}

size_t SizeOfTraceLogState(mozilla::MallocSizeOf mallocSizeOf);

// Automatic logging at the start and end of function call.
class MOZ_RAII AutoTraceLog {
#ifdef JS_TRACE_LOGGING
  TraceLoggerThread* logger;
  union {
    const TraceLoggerEvent* event;
    TraceLoggerTextId id;
  } payload;
  bool isEvent;
  bool executed;
  AutoTraceLog* prev;

 public:
  AutoTraceLog(TraceLoggerThread* logger, const TraceLoggerEvent& event)
      : logger(logger), isEvent(true), executed(false), prev(nullptr) {
    payload.event = &event;
    if (logger) {
      logger->startEvent(event);

      prev = logger->top_;
      logger->top_ = this;
    }
  }

  AutoTraceLog(TraceLoggerThread* logger, TraceLoggerTextId id)
      : logger(logger), isEvent(false), executed(false), prev(nullptr) {
    payload.id = id;
    if (logger) {
      logger->startEvent(id);

      prev = logger->top_;
      logger->top_ = this;
    }
  }

  ~AutoTraceLog() {
    if (logger) {
      while (this != logger->top_) {
        logger->top_->stop();
      }
      stop();
    }
  }

 private:
  void stop() {
    if (!executed) {
      executed = true;
      if (isEvent) {
        logger->stopEvent(*payload.event);
      } else {
        logger->stopEvent(payload.id);
      }
    }

    if (logger->top_ == this) {
      logger->top_ = prev;
    }
  }
#else
 public:
  AutoTraceLog(TraceLoggerThread* logger, uint32_t textId) {}
  AutoTraceLog(TraceLoggerThread* logger, const TraceLoggerEvent& event) {}
#endif

 private:
};

}  // namespace js

#endif /* TraceLogging_h */
