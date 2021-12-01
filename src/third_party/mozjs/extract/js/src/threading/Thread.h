/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_Thread_h
#define threading_Thread_h

#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/IndexSequence.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Tuple.h"

#include <stdint.h>

#include "jsutil.h"

#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "vm/MutexIDs.h"

#ifdef XP_WIN
# define THREAD_RETURN_TYPE unsigned int
# define THREAD_CALL_API __stdcall
#else
# define THREAD_RETURN_TYPE void*
# define THREAD_CALL_API
#endif

namespace js {
namespace detail {
template <typename F, typename... Args>
class ThreadTrampoline;
} // namespace detail

// Execute the given functor concurrent with the currently executing instruction
// stream and within the current address space. Use with care.
class Thread
{
public:
  struct Hasher;

  class Id
  {
    friend struct Hasher;
    class PlatformData;
    void* platformData_[2];

  public:
    Id();

    Id(const Id&) = default;
    Id(Id&&) = default;
    Id& operator=(const Id&) = default;
    Id& operator=(Id&&) = default;

    bool operator==(const Id& aOther) const;
    bool operator!=(const Id& aOther) const { return !operator==(aOther); }

    inline PlatformData* platformData();
    inline const PlatformData* platformData() const;
  };

  // Provides optional parameters to a Thread.
  class Options
  {
    size_t stackSize_;

  public:
    Options() : stackSize_(0) {}

    Options& setStackSize(size_t sz) { stackSize_ = sz; return *this; }
    size_t stackSize() const { return stackSize_; }
  };

  // A js::HashTable hash policy for keying hash tables by js::Thread::Id.
  struct Hasher
  {
    typedef Id Lookup;

    static HashNumber hash(const Lookup& l);

    static bool match(const Id& key, const Lookup& lookup) {
      return key == lookup;
    }
  };

  // Create a Thread in an initially unjoinable state. A thread of execution can
  // be created for this Thread by calling |init|. Some of the thread's
  // properties may be controlled by passing options to this constructor.
  template <typename O = Options,
            // SFINAE to make sure we don't try and treat functors for the other
            // constructor as an Options and vice versa.
            typename NonConstO = typename mozilla::RemoveConst<O>::Type,
            typename DerefO = typename mozilla::RemoveReference<NonConstO>::Type,
            typename = typename mozilla::EnableIf<mozilla::IsSame<DerefO, Options>::value,
                                                  void*>::Type>
  explicit Thread(O&& options = Options())
    : idMutex_(mutexid::ThreadId)
    , id_(Id())
    , options_(mozilla::Forward<O>(options))
  {
    MOZ_ASSERT(js::IsInitialized());
  }

  // Start a thread of execution at functor |f| with parameters |args|. This
  // method will return false if thread creation fails. This Thread must not
  // already have been created. Note that the arguments must be either POD or
  // rvalue references (mozilla::Move). Attempting to pass a reference will
  // result in the value being copied, which may not be the intended behavior.
  // See the comment below on ThreadTrampoline::args for an explanation.
  template <typename F, typename... Args>
  MOZ_MUST_USE bool init(F&& f, Args&&... args) {
    MOZ_RELEASE_ASSERT(id_ == Id());
    using Trampoline = detail::ThreadTrampoline<F, Args...>;
    AutoEnterOOMUnsafeRegion oom;
    auto trampoline = js_new<Trampoline>(mozilla::Forward<F>(f),
                                         mozilla::Forward<Args>(args)...);
    if (!trampoline)
      oom.crash("js::Thread::init");
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
  Id get_id();

  // Allow threads to be moved so that they can be stored in containers.
  Thread(Thread&& aOther);
  Thread& operator=(Thread&& aOther);

private:
  // Disallow copy as that's not sensible for unique resources.
  Thread(const Thread&) = delete;
  void operator=(const Thread&) = delete;

  bool joinable(LockGuard<Mutex>& lock);

  // Synchronize id_ initialization during thread startup.
  Mutex idMutex_;

  // Provide a process global ID to each thread.
  Id id_;

  // Overridable thread creation options.
  Options options_;

  // Dispatch to per-platform implementation of thread creation.
  MOZ_MUST_USE bool create(THREAD_RETURN_TYPE (THREAD_CALL_API *aMain)(void*), void* aArg);
};

namespace ThisThread {

// Return the thread id of the calling thread.
Thread::Id GetId();

// Set the current thread name. Note that setting the thread name may not be
// available on all platforms; on these platforms setName() will simply do
// nothing.
void SetName(const char* name);

// Get the current thread name. As with SetName, not available on all
// platforms. On these platforms getName() will give back an empty string (by
// storing NUL in nameBuffer[0]). 'len' is the bytes available to be written in
// 'nameBuffer', including the terminating NUL.
void GetName(char* nameBuffer, size_t len);

} // namespace ThisThread

namespace detail {

// Platform thread APIs allow passing a single void* argument to the target
// thread. This class is responsible for safely ferrying the arg pack and
// functor across that void* membrane and running it in the other thread.
template <typename F, typename... Args>
class ThreadTrampoline
{
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
  mozilla::Tuple<typename mozilla::Decay<Args>::Type...> args;

public:
  // Note that this template instatiation duplicates and is identical to the
  // class template instantiation. It is required for perfect forwarding of
  // rvalue references, which is only enabled for calls to a function template,
  // even if the class template arguments are correct.
  template <typename G, typename... ArgsT>
  explicit ThreadTrampoline(G&& aG, ArgsT&&... aArgsT)
    : f(mozilla::Forward<F>(aG)),
      args(mozilla::Forward<Args>(aArgsT)...)
  {
  }

  static THREAD_RETURN_TYPE THREAD_CALL_API Start(void* aPack) {
    auto* pack = static_cast<ThreadTrampoline<F, Args...>*>(aPack);
    pack->callMain(typename mozilla::IndexSequenceFor<Args...>::Type());
    js_delete(pack);
    return 0;
  }

  template<size_t ...Indices>
  void callMain(mozilla::IndexSequence<Indices...>) {
    f(mozilla::Get<Indices>(args)...);
  }
};

} // namespace detail
} // namespace js

#undef THREAD_RETURN_TYPE

#endif // threading_Thread_h
