/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/transport/service_entry_point_utils.h"

#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"

#ifdef __linux__  // TODO: consider making this ifndef _WIN32
#include <sys/resource.h>
#endif

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace mongo {

namespace {
void* runFunc(void* ctx) {
    std::unique_ptr<stdx::function<void()>> taskPtr(static_cast<stdx::function<void()>*>(ctx));
    (*taskPtr)();

    return nullptr;
}
}  // namespace

void launchServiceWorkerThread(stdx::function<void()> task) {
    auto ctx = stdx::make_unique<stdx::function<void()>>(std::move(task));

    try {
#ifndef __linux__  // TODO: consider making this ifdef _WIN32
        stdx::thread(stdx::bind(runFunc, ctx.get())).detach();
        ctx.release();
#else
        pthread_attr_t attrs;
        pthread_attr_init(&attrs);
        pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);

        static const size_t STACK_SIZE =
            1024 * 1024;  // if we change this we need to update the warning

        struct rlimit limits;
        invariant(getrlimit(RLIMIT_STACK, &limits) == 0);
        if (limits.rlim_cur > STACK_SIZE) {
            size_t stackSizeToSet = STACK_SIZE;
#if !__has_feature(address_sanitizer)
            if (kDebugBuild)
                stackSizeToSet /= 2;
#endif
            int failed = pthread_attr_setstacksize(&attrs, stackSizeToSet);
            if (failed) {
                const auto ewd = errnoWithDescription(failed);
                warning() << "pthread_attr_setstacksize failed: " << ewd;
            }
        } else if (limits.rlim_cur < 1024 * 1024) {
            warning() << "Stack size set to " << (limits.rlim_cur / 1024) << "KB. We suggest 1MB";
        }


        pthread_t thread;
        int failed = pthread_create(&thread, &attrs, runFunc, ctx.get());

        pthread_attr_destroy(&attrs);

        if (failed) {
            log() << "pthread_create failed: " << errnoWithDescription(failed);
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        ctx.release();
#endif  // __linux__

    } catch (...) {
        log() << "failed to create service entry worker thread";
    }
}

}  // namespace mongo
