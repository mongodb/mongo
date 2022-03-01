/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_Thread_h
#define threading_Thread_h

#include "mozilla/Atomics.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Tuple.h"

#include <stdint.h>
#include <type_traits>
#include <utility>

#include "js/Initialization.h"
#include "js/Utility.h"
#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "threading/ThreadId.h"
#include "vm/MutexIDs.h"

#ifdef XP_WIN
#  define THREAD_RETURN_TYPE unsigned int
#  define THREAD_CALL_API __stdcall
#else
#  define THREAD_RETURN_TYPE void*
#  define THREAD_CALL_API
#endif

namespace js {
namespace detail {
template <typename F, typename... Args>
class ThreadTrampoline;
}  // namespace detail

// Execute the given functor concurrent with the currently executing instruction
// stream and within the current address space. Use with care.
class Thread {
 public:
  // Provides optional parameters to a Thread.
  class Options {
    size_t stackSize_;

   public:
    Options() : stackSize_(0) {}

    Options& setStackSize(size_t sz) {
      stackSize_ = sz;
      return *this;
    }
    size_t stackSize() const { return stackSize_; }
  };

  // Create a Thread in an initially unjoinable state. A thread of execution can
  // be created for this Thread by calling |init|. Some of the thread's
  // properties may be controlled by passing options to this constructor.
  template <typename O = Options,
            // SFINAE to make sure we don't try and treat functors for the other
            // constructor as an Options and vice versa.
            typename NonConstO = std::remove_const_t<O>,
            typename DerefO = std::remove_reference_t<NonConstO>,
            typename = std::enable_if_t<std::is_same_v<DerefO, Options>>>
  explicit Thread(O&& options = Options())
      : id_(ThreadId()), options_(std::forward<O>(options)) {
    MOZ_ASSERT(isInitialized());
  }

  // Start a thread of execution at functor |f| with parameters |args|. This
  // method will return false if thread creation fails. This Thread must not
  // already have been created. Note that the arguments must be either POD or
  // rvalue references (std::move). Attempting to pass a reference will
  // result in the value being copied, which may not be the intended behavior.
  // See the comment below on ThreadTrampoline::args for an explanation.
  template <typename F, typename... Args>
  [[nodiscard]] bool init(F&& f, Args&&... args) {
    MOZ_RELEASE_ASSERT(id_ == ThreadId());
    using Trampoline = detail::ThreadTrampoline<F, Args...>;
    auto trampoline =
        js_new<Trampoline>(std::forward<F>(f), std::forward<Args>(args)...);
    if (!trampoline) {
      return false;
    }

    // We hold this lock while create() sets the thread id.
    LockGuard<Mutex> lock(trampoline->createMutex);
    return create(Trampoline::Start, trampoline);
  }

  // The thread must be joined or detached before destruction.
  ~Thread();

  // Move the thread into the detached state without blocking. In the detatched
  // state, the thread continues to run until it exits, but cannot be joined.
  // After this method returns, this Thread no longer represents a thread of
  // execution. When the thread exits, its resources will be cleaned up by the
  // system. At process exit, if the thread is still running, the thread's TLS
  // storage will be destructed, but the thread stack will *not* be unrolled.
  void detach();

  // Block the current thread until this Thread returns from the functor it was
  // created with. The thread's resources will be cleaned up before this
  // function returns. After this method returns, this Thread no longer
  // represents a thread of execution.
  void join();

  // Return true if this thread has not yet been joined or detached. If this
  // method returns false, this Thread does not have an associated thread of
  // execution, for example, if it has been previously moved or joined.
  bool joinable();

  // Returns the id of this thread if this represents a thread of execution or
  // the default constructed Id() if not. The thread ID is guaranteed to
  // uniquely identify a thread and can be compared with the == operator.
  ThreadId get_id();

  // Allow threads to be moved so that they can be stored in containers.
  Thread(Thread&& aOther);
  Thread& operator=(Thread&& aOther);

 private:
  // Disallow copy as that's not sensible for unique resources.
  Thread(const Thread&) = delete;
  void operator=(const Thread&) = delete;

  // Provide a process global ID to each thread.
  ThreadId id_;

  // Overridable thread creation options.
  Options options_;

  // Dispatch to per-platform implementation of thread creation.
  [[nodiscard]] bool create(THREAD_RETURN_TYPE(THREAD_CALL_API* aMain)(void*),
                            void* aArg);

  // An internal version of JS_IsInitialized() that returns whether SpiderMonkey
  // is currently initialized or is in the process of being initialized.
  static inline bool isInitialized() {
    using namespace JS::detail;
    return libraryInitState == InitState::Initializing ||
           libraryInitState == InitState::Running;
  }
};

namespace ThisThread {

// Set the current thread name. Note that setting the thread name may not be
// available on all platforms; on these platforms setName() will simply do
// nothing.
void SetName(const char* name);

// Get the current thread name. As with SetName, not available on all
// platforms. On these platforms getName() will give back an empty string (by
// storing NUL in nameBuffer[0]). 'len' is the bytes available to be written in
// 'nameBuffer', including the terminating NUL.
void GetName(char* nameBuffer, size_t len);

// Causes the current thread to sleep until the
// number of real-time milliseconds specified have elapsed.
void SleepMilliseconds(size_t ms);

}  // namespace ThisThread

namespace detail {

// Platform thread APIs allow passing a single void* argument to the target
// thread. This class is responsible for safely ferrying the arg pack and
// functor across that void* membrane and running it in the other thread.
template <typename F, typename... Args>
class ThreadTrampoline {
  // The functor to call.
  F f;

  // A std::decay copy of the arguments, as specified by std::thread. Using an
  // rvalue reference for the arguments to Thread and ThreadTrampoline gives us
  // move semantics for large structures, allowing us to quickly and easily pass
  // enormous amounts of data to a new thread. Unfortunately, there is a
  // downside: rvalue references becomes lvalue references when used with POD
  // types. This becomes dangerous when attempting to pass POD stored on the
  // stack to the new thread; the rvalue reference will implicitly become an
  // lvalue reference to the stack location. Thus, the value may not exist if
  // the parent thread leaves the frame before the read happens in the new
  // thread. To avoid this dangerous and highly non-obvious footgun, the
  // standard requires a "decay" copy of the arguments at the cost of making it
  // impossible to pass references between threads.
  mozilla::Tuple<std::decay_t<Args>...> args;

  // Protect the thread id during creation.
  Mutex createMutex;

  // Thread can access createMutex.
  friend class js::Thread;

 public:
  // Note that this template instatiation duplicates and is identical to the
  // class template instantiation. It is required for perfect forwarding of
  // rvalue references, which is only enabled for calls to a function template,
  // even if the class template arguments are correct.
  template <typename G, typename... ArgsT>
  explicit ThreadTrampoline(G&& aG, ArgsT&&... aArgsT)
      : f(std::forward<F>(aG)),
        args(std::forward<Args>(aArgsT)...),
        createMutex(mutexid::ThreadId) {}

  static THREAD_RETURN_TYPE THREAD_CALL_API Start(void* aPack) {
    auto* pack = static_cast<ThreadTrampoline<F, Args...>*>(aPack);
    pack->callMain(std::index_sequence_for<Args...>{});
    js_delete(pack);
    return 0;
  }

  template <size_t... Indices>
  void callMain(std::index_sequence<Indices...>) {
    // Pretend createMutex is a semaphore and wait for a notification that the
    // thread that spawned us is ready.
    createMutex.lock();
    createMutex.unlock();
    f(mozilla::Get<Indices>(args)...);
  }
};

}  // namespace detail
}  // namespace js

#undef THREAD_RETURN_TYPE

#endif  // threading_Thread_h
