/**    Copyright 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo {

/**
 * WithLock is an attestation to pass as an argument to functions that must be called only while
 * holding a lock, as a rigorous alternative to an unchecked naming convention and/or stern
 * comments.  It helps prevent a common usage error.
 *
 * It may be used to modernize code from (something like) this
 *
 *     // Member _mutex MUST be held when calling this:
 *     void _clobber_inlock(OperationContext* opCtx) {
 *         _stuff = makeStuff(opCtx);
 *     }
 *
 * into
 *
 *     void _clobber(WithLock, OperationContext* opCtx) {
 *         _stuff = makeStuff(opCtx);
 *     }
 *
 * A call to such a function looks like this:
 *
 *     stdx::lock_guard<stdx::mutex> lk(_mutex);
 *     _clobber(lk, opCtx);  // instead of _clobber_inlock(opCtx)
 *
 * Note that the formal argument need not (and should not) be named unless it is needed to pass
 * the attestation along to another function:
 *
 *     void _clobber(WithLock lock, OperationContext* opCtx) {
 *         _really_clobber(lock, opCtx);
 *     }
 *
 */
struct WithLock {
    template <typename Mutex>
    WithLock(stdx::lock_guard<Mutex> const&) noexcept {}

    template <typename Mutex>
    WithLock(stdx::unique_lock<Mutex> const& lock) noexcept {
        invariant(lock.owns_lock());
    }

    // Add constructors from any other lock types here.

    // Pass by value is OK.
    WithLock(WithLock const&) noexcept {}
    WithLock(WithLock&&) noexcept {}

    // No assigning WithLocks.
    void operator=(WithLock const&) = delete;
    void operator=(WithLock&&) = delete;

    // No moving a lock_guard<> or unique_lock<> in.
    template <typename Mutex>
    WithLock(stdx::lock_guard<Mutex>&&) = delete;
    template <typename Mutex>
    WithLock(stdx::unique_lock<Mutex>&&) = delete;

    /*
     * Produces a WithLock without benefit of any actual lock, for use in cases where a lock is not
     * really needed, such as in many (but not all!) constructors.
     */
    static WithLock withoutLock() noexcept {
        return {};
    }

private:
    WithLock() noexcept = default;
};

}  // namespace mongo

namespace std {
// No moving a WithLock:
template <>
mongo::WithLock&& move<mongo::WithLock>(mongo::WithLock&&) noexcept = delete;
}  // namespace std
