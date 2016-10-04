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

#include "mongo/db/client.h"
#include "mongo/db/server_options.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/quick_exit.h"

#ifdef __linux__  // TODO: consider making this ifndef _WIN32
#include <sys/resource.h>
#endif

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace mongo {

namespace {

struct Context {
    Context(transport::Session session, stdx::function<void(transport::Session*)> task)
        : session(std::move(session)), task(std::move(task)) {}

    transport::Session session;
    stdx::function<void(transport::Session*)> task;
};

void* runFunc(void* ptr) {
    std::unique_ptr<Context> ctx(static_cast<Context*>(ptr));

    auto tl = ctx->session.getTransportLayer();
    Client::initThread("conn", &ctx->session);
    setThreadName(std::string(str::stream() << "conn" << ctx->session.id()));

    try {
        ctx->task(&ctx->session);
    } catch (const AssertionException& e) {
        log() << "AssertionException handling request, closing client connection: " << e;
    } catch (const SocketException& e) {
        log() << "SocketException handling request, closing client connection: " << e;
    } catch (const DBException& e) {
        // must be right above std::exception to avoid catching subclasses
        log() << "DBException handling request, closing client connection: " << e;
    } catch (const std::exception& e) {
        error() << "Uncaught std::exception: " << e.what() << ", terminating";
        quickExit(EXIT_UNCAUGHT);
    }

    tl->end(ctx->session);

    if (!serverGlobalParams.quiet) {
        auto conns = tl->sessionStats().numOpenSessions;
        const char* word = (conns == 1 ? " connection" : " connections");
        log() << "end connection " << ctx->session.remote() << " (" << conns << word
              << " now open)";
    }

    Client::destroy();

    return nullptr;
}
}  // namespace

void launchWrappedServiceEntryWorkerThread(transport::Session&& session,
                                           stdx::function<void(transport::Session*)> task) {
    auto ctx = stdx::make_unique<Context>(std::move(session), std::move(task));

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
        log() << "failed to create service entry worker thread for " << ctx->session.remote();
    }
}

}  // namespace mongo
