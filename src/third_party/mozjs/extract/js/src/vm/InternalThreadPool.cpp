/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/InternalThreadPool.h"

#include "mozilla/TimeStamp.h"

#include "js/ProfilingCategory.h"
#include "js/ProfilingStack.h"
#include "threading/Thread.h"
#include "util/NativeStack.h"
#include "vm/HelperThreadState.h"
#include "vm/JSContext.h"

// We want our default stack size limit to be approximately 2MB, to be safe, but
// expect most threads to use much less. On Linux, however, requesting a stack
// of 2MB or larger risks the kernel allocating an entire 2MB huge page for it
// on first access, which we do not want. To avoid this possibility, we subtract
// 2 standard VM page sizes from our default.
static const uint32_t kDefaultHelperStackSize = 2048 * 1024 - 2 * 4096;

// TSan enforces a minimum stack size that's just slightly larger than our
// default helper stack size.  It does this to store blobs of TSan-specific
// data on each thread's stack.  Unfortunately, that means that even though
// we'll actually receive a larger stack than we requested, the effective
// usable space of that stack is significantly less than what we expect.
// To offset TSan stealing our stack space from underneath us, double the
// default.
//
// Note that we don't need this for ASan/MOZ_ASAN because ASan doesn't
// require all the thread-specific state that TSan does.
#if defined(MOZ_TSAN)
static const uint32_t HELPER_STACK_SIZE = 2 * kDefaultHelperStackSize;
#else
static const uint32_t HELPER_STACK_SIZE = kDefaultHelperStackSize;
#endif

// These macros are identical in function to the same-named ones in
// GeckoProfiler.h, but they are defined separately because SpiderMonkey can't
// use GeckoProfiler.h.
#define PROFILER_RAII_PASTE(id, line) id##line
#define PROFILER_RAII_EXPAND(id, line) PROFILER_RAII_PASTE(id, line)
#define PROFILER_RAII PROFILER_RAII_EXPAND(raiiObject, __LINE__)
#define AUTO_PROFILER_LABEL(label, categoryPair) \
  HelperThread::AutoProfilerLabel PROFILER_RAII( \
      this, label, JS::ProfilingCategoryPair::categoryPair)

using namespace js;

namespace js {

class HelperThread {
  Thread thread;

  ConditionVariable wakeup;

  HelperThreadLockData<HelperThreadTask*> nextTask;

  /*
   * The profiling thread for this helper thread, which can be used to push
   * and pop label frames.
   * This field being non-null indicates that this thread has been registered
   * and needs to be unregistered at shutdown.
   */
  ProfilingStack* profilingStack = nullptr;

 public:
  const uint32_t id;

  explicit HelperThread(uint32_t id);
  [[nodiscard]] bool init(InternalThreadPool* pool);

  ThreadId threadId() { return thread.get_id(); }

  void join();

  static void ThreadMain(InternalThreadPool* pool, HelperThread* helper);
  void threadLoop(InternalThreadPool* pool);

  void ensureRegisteredWithProfiler();
  void unregisterWithProfilerIfNeeded();

  void dispatchTask(HelperThreadTask* task);
  void notify();

 private:
  struct AutoProfilerLabel {
    AutoProfilerLabel(HelperThread* helperThread, const char* label,
                      JS::ProfilingCategoryPair categoryPair);
    ~AutoProfilerLabel();

   private:
    ProfilingStack* profilingStack;
  };
};

}  // namespace js

InternalThreadPool* InternalThreadPool::Instance = nullptr;

/* static */ InternalThreadPool& InternalThreadPool::Get() {
  MOZ_ASSERT(IsInitialized());
  return *Instance;
}

/* static */
bool InternalThreadPool::Initialize(size_t threadCount,
                                    AutoLockHelperThreadState& lock) {
  if (IsInitialized()) {
    return true;
  }

  auto instance = MakeUnique<InternalThreadPool>();
  if (!instance) {
    return false;
  }

  if (!instance->ensureThreadCount(threadCount, lock)) {
    instance->shutDown(lock);
    return false;
  }

  Instance = instance.release();
  HelperThreadState().setDispatchTaskCallback(DispatchTask, threadCount,
                                              HELPER_STACK_SIZE, lock);
  return true;
}

bool InternalThreadPool::ensureThreadCount(size_t threadCount,
                                           AutoLockHelperThreadState& lock) {
  // Ensure space in freeThreadSet.
  threadCount = std::min(threadCount, sizeof(uint32_t) * CHAR_BIT);

  MOZ_ASSERT(threads(lock).length() <= threadCount);

  if (!threads(lock).reserve(threadCount)) {
    return false;
  }

  while (threads(lock).length() < threadCount) {
    uint32_t id = threads(lock).length();

    auto thread = js::MakeUnique<HelperThread>(id);
    if (!thread || !thread->init(this)) {
      return false;
    }

    threads(lock).infallibleEmplaceBack(std::move(thread));

    setThreadFree(id);
  }

  for (size_t i = 0; i < threads(lock).length(); i++) {
    MOZ_ASSERT(threads(lock)[i]->id == i);
  }

  return true;
}

size_t InternalThreadPool::threadCount(const AutoLockHelperThreadState& lock) {
  return threads(lock).length();
}

/* static */
void InternalThreadPool::ShutDown(AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(HelperThreadState().isTerminating(lock));

  Get().shutDown(lock);
  js_delete(Instance);
  Instance = nullptr;
}

void InternalThreadPool::shutDown(AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(!terminating);
  terminating = true;

  for (auto& thread : threads(lock)) {
    thread->notify();
  }

  for (auto& thread : threads(lock)) {
    AutoUnlockHelperThreadState unlock(lock);
    thread->join();
  }
}

inline HelperThreadVector& InternalThreadPool::threads(
    const AutoLockHelperThreadState& lock) {
  return threads_.ref();
}
inline const HelperThreadVector& InternalThreadPool::threads(
    const AutoLockHelperThreadState& lock) const {
  return threads_.ref();
}

size_t InternalThreadPool::sizeOfIncludingThis(
    mozilla::MallocSizeOf mallocSizeOf,
    const AutoLockHelperThreadState& lock) const {
  return sizeof(InternalThreadPool) +
         threads(lock).sizeOfExcludingThis(mallocSizeOf);
}

/* static */
void InternalThreadPool::DispatchTask(HelperThreadTask* task) {
  Get().dispatchOrQueueTask(task);
}

void InternalThreadPool::dispatchOrQueueTask(HelperThreadTask* task) {
  // This could now use a separate mutex like TaskController, but continues to
  // use the helper thread state lock for convenience.
  AutoLockHelperThreadState lock;
  MOZ_ASSERT(!terminating);
  MOZ_ASSERT(freeThreadSet != 0);

  uint32_t id = mozilla::CountTrailingZeroes32(freeThreadSet);
  clearThreadFree(id);

  HelperThread* thread = threads_.ref()[id].get();
  thread->dispatchTask(task);
}

void InternalThreadPool::setThreadFree(uint32_t threadId) {
  uint32_t idMask = 1 << threadId;
  MOZ_ASSERT((freeThreadSet & idMask) == 0);
  freeThreadSet |= idMask;
}

void InternalThreadPool::clearThreadFree(uint32_t threadId) {
  uint32_t idMask = 1 << threadId;
  MOZ_ASSERT((freeThreadSet & idMask) != 0);
  freeThreadSet &= ~idMask;
}

HelperThread::HelperThread(uint32_t id)
    : thread(Thread::Options().setStackSize(HELPER_STACK_SIZE)), id(id) {}

bool HelperThread::init(InternalThreadPool* pool) {
  return thread.init(HelperThread::ThreadMain, pool, this);
}

void HelperThread::join() { thread.join(); }

/* static */
void HelperThread::ThreadMain(InternalThreadPool* pool, HelperThread* helper) {
  ThisThread::SetName("JS Helper");

  helper->ensureRegisteredWithProfiler();
  helper->threadLoop(pool);
  helper->unregisterWithProfilerIfNeeded();
}

void HelperThread::ensureRegisteredWithProfiler() {
  if (profilingStack) {
    return;
  }

  // Note: To avoid dead locks, we should not hold on the helper thread lock
  // while calling this function. This is safe because the registerThread field
  // is a WriteOnceData<> type stored on the global helper tread state.
  JS::RegisterThreadCallback callback = HelperThreadState().registerThread;
  if (callback) {
    profilingStack =
        callback("JS Helper", reinterpret_cast<void*>(GetNativeStackBase()));
  }
}

void HelperThread::unregisterWithProfilerIfNeeded() {
  if (!profilingStack) {
    return;
  }

  // Note: To avoid dead locks, we should not hold on the helper thread lock
  // while calling this function. This is safe because the unregisterThread
  // field is a WriteOnceData<> type stored on the global helper tread state.
  JS::UnregisterThreadCallback callback = HelperThreadState().unregisterThread;
  if (callback) {
    callback();
    profilingStack = nullptr;
  }
}

HelperThread::AutoProfilerLabel::AutoProfilerLabel(
    HelperThread* helperThread, const char* label,
    JS::ProfilingCategoryPair categoryPair)
    : profilingStack(helperThread->profilingStack) {
  if (profilingStack) {
    profilingStack->pushLabelFrame(label, nullptr, this, categoryPair);
  }
}

HelperThread::AutoProfilerLabel::~AutoProfilerLabel() {
  if (profilingStack) {
    profilingStack->pop();
  }
}

void HelperThread::dispatchTask(HelperThreadTask* task) {
  MOZ_ASSERT(!nextTask);
  nextTask = task;
  notify();
}

void HelperThread::notify() { wakeup.notify_one(); }

void HelperThread::threadLoop(InternalThreadPool* pool) {
  MOZ_ASSERT(CanUseExtraThreads());

  AutoLockHelperThreadState lock;

  while (!pool->terminating) {
    if (!nextTask) {
      AUTO_PROFILER_LABEL("HelperThread::threadLoop::wait", IDLE);
      wakeup.wait(lock);
      continue;
    }

    // JS::RunHelperThreadTask calls runOneTask and then dispatch. Here we split
    // this up so we can mark the current thread as free in between and allow
    // dispatch to pick this thread for the next task.

    HelperThreadState().runOneTask(nextTask, lock);

    nextTask = nullptr;
    pool->setThreadFree(id);

    HelperThreadState().dispatch(lock);
    AutoUnlockHelperThreadState unlock(lock);
  }
}
