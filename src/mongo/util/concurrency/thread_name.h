/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/thread_context.h"

namespace mongo {

/**
 * ThreadName is a uniquely identifyable, immutable, ref-counted string.
 *
 * This class is used for three purposes:
 * - Setting the official thread name with the OS.
 * - Populating the "ctx" field for log lines.
 * - Providing a thread name to gdb.
 */
class ThreadName : public RefCountable {
public:
    using Id = size_t;

    /**
     * Create a new instance.
     *
     * Note that this does not set it to be the official one for the thread.
     */
    explicit ThreadName(StringData name);
    ThreadName(const ThreadName&) = delete;
    ThreadName(ThreadName&&) = delete;

    /**
     * Get the official ThreadName for the current thread via the ThreadContext.
     */
    static boost::intrusive_ptr<ThreadName> get(boost::intrusive_ptr<ThreadContext> context);

    /**
     * Set the official ThreadName for the current thread via the ThreadContext.
     *
     * Note that this also will set the OS thread name if the name is different from the current
     * one.
     *
     * If a different non-anonymous thread name was previously set, this returns that name. If the
     * given name was already set, a previous name was released, or the initial name was set, this
     * returns an empty pointer.
     */
    static boost::intrusive_ptr<ThreadName> set(boost::intrusive_ptr<ThreadContext> context,
                                                boost::intrusive_ptr<ThreadName> name);

    /**
     * Release the current thread name.
     *
     * This does not unset the OS thread name or change the current storage. Instead, this marks the
     * current name as available for reuse or replacement.
     */
    static void release(boost::intrusive_ptr<ThreadContext> context);

    /**
     * Get a string for the current thread without new allocations.
     *
     * In pre-init, this returns "-". That value will mostly be associated with the main thread.
     * If a thread is somehow started in pre-init and dodges our ThreadSafetyContext checks, it will
     * also return "-" for this function.
     */
    static StringData getStaticString();

    StringData toString() const {
        return _storage;
    }

    friend bool operator==(const ThreadName& lhs, const ThreadName& rhs) noexcept {
        return lhs._id == rhs._id;
    }

    friend bool operator!=(const ThreadName& lhs, const ThreadName& rhs) noexcept {
        return lhs._id != rhs._id;
    }

private:
    static Id _nextId();

    const Id _id;
    const std::string _storage;
};

/**
 * Sets the name of the current thread.
 */
inline void setThreadName(StringData name) {
    ThreadName::set(ThreadContext::get(), make_intrusive<ThreadName>(name));
}

/**
 * Retrieves the name of the current thread, as previously set, or "thread#" if no name was
 * previously set. The returned StringData is always null terminated so it is safe to pass to APIs
 * that expect c-strings.
 */
inline StringData getThreadName() {
    return ThreadName::get(ThreadContext::get())->toString();
}

}  // namespace mongo
