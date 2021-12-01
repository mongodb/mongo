/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Promise_h
#define builtin_Promise_h

#include "builtin/SelfHostingDefines.h"
#include "threading/ConditionVariable.h"
#include "threading/Mutex.h"
#include "vm/NativeObject.h"

namespace js {

enum PromiseSlots {
    PromiseSlot_Flags = 0,
    PromiseSlot_ReactionsOrResult,
    PromiseSlot_RejectFunction,
    PromiseSlot_AwaitGenerator = PromiseSlot_RejectFunction,
    PromiseSlot_DebugInfo,
    PromiseSlots,
};

#define PROMISE_FLAG_RESOLVED  0x1
#define PROMISE_FLAG_FULFILLED 0x2
#define PROMISE_FLAG_HANDLED   0x4
#define PROMISE_FLAG_REPORTED  0x8
#define PROMISE_FLAG_DEFAULT_RESOLVE_FUNCTION 0x10
#define PROMISE_FLAG_DEFAULT_REJECT_FUNCTION  0x20
#define PROMISE_FLAG_ASYNC    0x40

class AutoSetNewObjectMetadata;

class PromiseObject : public NativeObject
{
  public:
    static const unsigned RESERVED_SLOTS = PromiseSlots;
    static const Class class_;
    static const Class protoClass_;
    static PromiseObject* create(JSContext* cx, HandleObject executor,
                                 HandleObject proto = nullptr, bool needsWrapping = false);

    static PromiseObject* createSkippingExecutor(JSContext* cx);

    static JSObject* unforgeableResolve(JSContext* cx, HandleValue value);
    static JSObject* unforgeableReject(JSContext* cx, HandleValue value);

    JS::PromiseState state() {
        int32_t flags = getFixedSlot(PromiseSlot_Flags).toInt32();
        if (!(flags & PROMISE_FLAG_RESOLVED)) {
            MOZ_ASSERT(!(flags & PROMISE_FLAG_FULFILLED));
            return JS::PromiseState::Pending;
        }
        if (flags & PROMISE_FLAG_FULFILLED)
            return JS::PromiseState::Fulfilled;
        return JS::PromiseState::Rejected;
    }
    Value value()  {
        MOZ_ASSERT(state() == JS::PromiseState::Fulfilled);
        return getFixedSlot(PromiseSlot_ReactionsOrResult);
    }
    Value reason() {
        MOZ_ASSERT(state() == JS::PromiseState::Rejected);
        return getFixedSlot(PromiseSlot_ReactionsOrResult);
    }

    static MOZ_MUST_USE bool resolve(JSContext* cx, Handle<PromiseObject*> promise,
                                     HandleValue resolutionValue);
    static MOZ_MUST_USE bool reject(JSContext* cx, Handle<PromiseObject*> promise,
                                    HandleValue rejectionValue);

    static void onSettled(JSContext* cx, Handle<PromiseObject*> promise);

    double allocationTime();
    double resolutionTime();
    JSObject* allocationSite();
    JSObject* resolutionSite();
    double lifetime();
    double timeToResolution() {
        MOZ_ASSERT(state() != JS::PromiseState::Pending);
        return resolutionTime() - allocationTime();
    }
    MOZ_MUST_USE bool dependentPromises(JSContext* cx, MutableHandle<GCVector<Value>> values);
    uint64_t getID();
    bool isUnhandled() {
        MOZ_ASSERT(state() == JS::PromiseState::Rejected);
        return !(getFixedSlot(PromiseSlot_Flags).toInt32() & PROMISE_FLAG_HANDLED);
    }
    void markAsReported() {
        MOZ_ASSERT(isUnhandled());
        int32_t flags = getFixedSlot(PromiseSlot_Flags).toInt32();
        setFixedSlot(PromiseSlot_Flags, Int32Value(flags | PROMISE_FLAG_REPORTED));
    }
};

/**
 * Unforgeable version of the JS builtin Promise.all.
 *
 * Takes an AutoObjectVector of Promise objects and returns a promise that's
 * resolved with an array of resolution values when all those promises have
 * been resolved, or rejected with the rejection value of the first rejected
 * promise.
 *
 * Asserts that all objects in the `promises` vector are, maybe wrapped,
 * instances of `Promise` or a subclass of `Promise`.
 */
MOZ_MUST_USE JSObject*
GetWaitForAllPromise(JSContext* cx, const JS::AutoObjectVector& promises);

/**
 * Enqueues resolve/reject reactions in the given Promise's reactions lists
 * as though calling the original value of Promise.prototype.then.
 *
 * If the `createDependent` flag is not set, no dependent Promise will be
 * created. This is used internally to implement DOM functionality.
 * Note: In this case, the reactions pushed using this function contain a
 * `promise` field that can contain null. That field is only ever used by
 * devtools, which have to treat these reactions specially.
 */
MOZ_MUST_USE bool
OriginalPromiseThen(JSContext* cx, Handle<PromiseObject*> promise,
                    HandleValue onFulfilled, HandleValue onRejected,
                    MutableHandleObject dependent, bool createDependent);

/**
 * PromiseResolve ( C, x )
 *
 * The abstract operation PromiseResolve, given a constructor and a value,
 * returns a new promise resolved with that value.
 */
MOZ_MUST_USE JSObject*
PromiseResolve(JSContext* cx, HandleObject constructor, HandleValue value);


MOZ_MUST_USE PromiseObject*
CreatePromiseObjectForAsync(JSContext* cx, HandleValue generatorVal);

MOZ_MUST_USE bool
IsPromiseForAsync(JSObject* promise);

MOZ_MUST_USE bool
AsyncFunctionReturned(JSContext* cx, Handle<PromiseObject*> resultPromise, HandleValue value);

MOZ_MUST_USE bool
AsyncFunctionThrown(JSContext* cx, Handle<PromiseObject*> resultPromise);

MOZ_MUST_USE bool
AsyncFunctionAwait(JSContext* cx, Handle<PromiseObject*> resultPromise, HandleValue value);

class AsyncGeneratorObject;

MOZ_MUST_USE bool
AsyncGeneratorAwait(JSContext* cx, Handle<AsyncGeneratorObject*> asyncGenObj, HandleValue value);

MOZ_MUST_USE bool
AsyncGeneratorResolve(JSContext* cx, Handle<AsyncGeneratorObject*> asyncGenObj,
                      HandleValue value, bool done);

MOZ_MUST_USE bool
AsyncGeneratorReject(JSContext* cx, Handle<AsyncGeneratorObject*> asyncGenObj,
                     HandleValue exception);

MOZ_MUST_USE bool
AsyncGeneratorEnqueue(JSContext* cx, HandleValue asyncGenVal, CompletionKind completionKind,
                      HandleValue completionValue, MutableHandleValue result);

bool
AsyncFromSyncIteratorMethod(JSContext* cx, CallArgs& args, CompletionKind completionKind);

// An OffThreadPromiseTask holds a rooted Promise JSObject while executing an
// off-thread task (defined by the subclass) that needs to resolve the Promise
// on completion. Because OffThreadPromiseTask contains a PersistentRooted, it
// must be destroyed on an active JSContext thread of the Promise's JSRuntime.
// OffThreadPromiseTasks may be run off-thread in various ways (e.g., see
// PromiseHelperTask). At any time, the task can be dispatched to an active
// JSContext of the Promise's JSRuntime by calling dispatchResolve().

class OffThreadPromiseTask : public JS::Dispatchable
{
    friend class OffThreadPromiseRuntimeState;

    JSRuntime*                       runtime_;
    PersistentRooted<PromiseObject*> promise_;
    bool                             registered_;

    void operator=(const OffThreadPromiseTask&) = delete;
    OffThreadPromiseTask(const OffThreadPromiseTask&) = delete;

  protected:
    OffThreadPromiseTask(JSContext* cx, Handle<PromiseObject*> promise);

    // To be called by OffThreadPromiseTask and implemented by the derived class.
    virtual bool resolve(JSContext* cx, Handle<PromiseObject*> promise) = 0;

    // JS::Dispatchable implementation. Ends with 'delete this'.
    void run(JSContext* cx, MaybeShuttingDown maybeShuttingDown) final;

  public:
    ~OffThreadPromiseTask() override;

    // Initializing an OffThreadPromiseTask informs the runtime that it must
    // wait on shutdown for this task to rejoin the active JSContext by calling
    // dispatchResolveAndDestroy().
    bool init(JSContext* cx);

    // An initialized OffThreadPromiseTask can be dispatched to an active
    // JSContext of its Promise's JSRuntime from any thread. Normally, this will
    // lead to resolve() being called on JSContext thread, given the Promise.
    // However, if shutdown interrupts, resolve() may not be called, though the
    // OffThreadPromiseTask will be destroyed on a JSContext thread.
    void dispatchResolveAndDestroy();
};

using OffThreadPromiseTaskSet = HashSet<OffThreadPromiseTask*,
                                        DefaultHasher<OffThreadPromiseTask*>,
                                        SystemAllocPolicy>;

using DispatchableVector = Vector<JS::Dispatchable*, 0, SystemAllocPolicy>;

class OffThreadPromiseRuntimeState
{
    friend class OffThreadPromiseTask;

    // These fields are initialized once before any off-thread usage and thus do
    // not require a lock.
    JS::DispatchToEventLoopCallback dispatchToEventLoopCallback_;
    void*                           dispatchToEventLoopClosure_;

    // These fields are mutated by any thread and are guarded by mutex_.
    Mutex                           mutex_;
    ConditionVariable               allCanceled_;
    OffThreadPromiseTaskSet         live_;
    size_t                          numCanceled_;
    DispatchableVector              internalDispatchQueue_;
    ConditionVariable               internalDispatchQueueAppended_;
    bool                            internalDispatchQueueClosed_;

    static bool internalDispatchToEventLoop(void*, JS::Dispatchable*);
    bool usingInternalDispatchQueue() const;

    void operator=(const OffThreadPromiseRuntimeState&) = delete;
    OffThreadPromiseRuntimeState(const OffThreadPromiseRuntimeState&) = delete;

  public:
    OffThreadPromiseRuntimeState();
    ~OffThreadPromiseRuntimeState();
    void init(JS::DispatchToEventLoopCallback callback, void* closure);
    void initInternalDispatchQueue();
    bool initialized() const;

    // If initInternalDispatchQueue() was called, internalDrain() can be
    // called to periodically drain the dispatch queue before shutdown.
    void internalDrain(JSContext* cx);
    bool internalHasPending();

    // shutdown() must be called by the JSRuntime while the JSRuntime is valid.
    void shutdown(JSContext* cx);
};

bool
Promise_static_resolve(JSContext* cx, unsigned argc, Value* vp);
bool
Promise_reject(JSContext* cx, unsigned argc, Value* vp);
bool
Promise_then(JSContext* cx, unsigned argc, Value* vp);

} // namespace js

#endif /* builtin_Promise_h */
