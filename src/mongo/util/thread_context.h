/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

/**
 * A ThreadContext is a simple decorable that has an explicit one-to-one relationship with threads.
 *
 * There are three lifetime tricks that this class does:
 * 1. It only exists on the main thread after we run MONGO_INITIALIZERS (a.k.a. post-init).
 * 2. It constructs and destructs on the local stack and thus cannot reference itself in its
 *    constructor or destructor.
 * 3. It can be persisted past the death of its original thread. This means that decorations may
 *    be destructed on a thread with a different ThreadContext attached. It also means that
 *    ThreadContexts can be safely tracked in data structures without complicated lifetime logic.
 *
 * There may be situations where you want to do one thing to a decoration when the thread dies and
 * another when the decoration is destructed. If this comes up, we will need a graph of ordered
 * actions like we have for ServiceContext.
 */
class ThreadContext final : public Decorable<ThreadContext>, public RefCountable {
public:
    ThreadContext() = default;
    virtual ~ThreadContext() = default;

    /**
     * This initializes the main thread in a MONGO_INITIALIZER.
     *
     * This is invalid to invoke on other threads.
     */
    static void initializeMain();

    /**
     * Get the ThreadContext for the current thread.
     *
     * If you are in pre-init, this will return an empty pointer. If you want to access a given
     * ThreadContext during thread death and beyond, store a copy of this intrusive_ptr somewhere.
     */
    static const boost::intrusive_ptr<ThreadContext>& get() {
        return _handle.instance;
    }

    /**
     * Get the thread id for the current thread.
     */
    const auto& threadId() const {
        return _threadId;
    }

    /**
     * Get if the current thread is still running.
     *
     * This value only transitions from true to false.
     */
    bool isAlive() const {
        return _isAlive.load();
    }

private:
    /**
     * This handle class is the actual thread_local variable.
     *
     * Its functions manage lifetime and set _isAlive.
     */
    struct Handle {
        /**
         * Create a new ThreadContext post-init and do nothing pre-init.
         */
        Handle();

        /**
         * Move the ThreadContext to the local stack and destroy it.
         */
        ~Handle();

        /**
         * Create and bind a new ThreadContext.
         */
        void init();

        boost::intrusive_ptr<ThreadContext> instance;
    };

    inline static thread_local auto _handle = Handle{};

    const ProcessId _threadId = ProcessId::getCurrentThreadId();
    AtomicWord<bool> _isAlive{true};
};

}  // namespace mongo
