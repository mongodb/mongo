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

#include "mongo/util/thread_context.h"

#include "mongo/base/init.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
const auto kMainThreadId = stdx::this_thread::get_id();
AtomicWord<bool> gHasInitializedMain{false};
}  // namespace

thread_local ThreadContext::Handle ThreadContext::_handle;

MONGO_INITIALIZER(ThreadContextsInitialized)(InitializerContext*) {
    ThreadContext::initializeMain();
}

void ThreadContext::initializeMain() {
    invariant(stdx::this_thread::get_id() == kMainThreadId,
              "initializeMain() must be called on the main thread");

    if (gHasInitializedMain.swap(true)) {
        return;
    }

    _handle.init();
}

ThreadContext::Handle::Handle() {
    if (!gHasInitializedMain.loadRelaxed()) {
        // If we have not initialized main, then we delay until the MONGO_INITIALIZER runs.
        return;
    }

    init();
}

void ThreadContext::Handle::init() {
    // Note that construction happens before assignment to the thread_local.
    instance = make_intrusive<ThreadContext>();
}

ThreadContext::Handle::~Handle() {
    if (!instance) {
        // If we don't have an instance, just skip. This is mostly going to be pre-main failures.
        return;
    }

    // Remove from the thread local access, then destroy our pointer to it.
    auto localInstance = std::exchange(instance, {});
    localInstance->_isAlive.store(false);
    localInstance.reset();
}

}  // namespace mongo
