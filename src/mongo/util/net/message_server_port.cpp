// message_server_port.cpp

/*    Copyright 2009 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include <memory>
#include <system_error>

#include "mongo/base/disallow_copying.h"
#include "mongo/config.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/synchronization.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/message_port.h"
#include "mongo/util/net/message_server.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"

#ifdef __linux__  // TODO: consider making this ifndef _WIN32
#include <sys/resource.h>
#endif

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace mongo {

using std::unique_ptr;
using std::endl;

namespace {

class MessagingPortWithHandler : public MessagingPort {
    MONGO_DISALLOW_COPYING(MessagingPortWithHandler);

public:
    MessagingPortWithHandler(const std::shared_ptr<Socket>& socket,
                             const std::shared_ptr<MessageHandler> handler,
                             long long connectionId)
        : MessagingPort(socket), _handler(handler) {
        setConnectionId(connectionId);
    }

    const std::shared_ptr<MessageHandler> getHandler() const {
        return _handler;
    }

private:
    const std::shared_ptr<MessageHandler> _handler;
};

}  // namespace

class PortMessageServer : public MessageServer, public Listener {
public:
    /**
     * Creates a new message server.
     *
     * @param opts
     * @param handler the handler to use.
     */
    PortMessageServer(const MessageServer::Options& opts, std::shared_ptr<MessageHandler> handler)
        : Listener("", opts.ipList, opts.port), _handler(std::move(handler)) {}

    virtual void accepted(std::shared_ptr<Socket> psocket, long long connectionId) {
        ScopeGuard sleepAfterClosingPort = MakeGuard(sleepmillis, 2);
        std::unique_ptr<MessagingPortWithHandler> portWithHandler(
            new MessagingPortWithHandler(psocket, _handler, connectionId));

        if (!Listener::globalTicketHolder.tryAcquire()) {
            log() << "connection refused because too many open connections: "
                  << Listener::globalTicketHolder.used() << endl;
            return;
        }

        try {
#ifndef __linux__  // TODO: consider making this ifdef _WIN32
            {
                stdx::thread thr(stdx::bind(&handleIncomingMsg, portWithHandler.get()));
                thr.detach();
            }
#else
            pthread_attr_t attrs;
            pthread_attr_init(&attrs);
            pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);

            static const size_t STACK_SIZE =
                1024 * 1024;  // if we change this we need to update the warning

            struct rlimit limits;
            verify(getrlimit(RLIMIT_STACK, &limits) == 0);
            if (limits.rlim_cur > STACK_SIZE) {
                size_t stackSizeToSet = STACK_SIZE;
#if !__has_feature(address_sanitizer)
                if (kDebugBuild)
                    stackSizeToSet /= 2;
#endif
                pthread_attr_setstacksize(&attrs, stackSizeToSet);
            } else if (limits.rlim_cur < 1024 * 1024) {
                warning() << "Stack size set to " << (limits.rlim_cur / 1024)
                          << "KB. We suggest 1MB" << endl;
            }


            pthread_t thread;
            int failed = pthread_create(&thread, &attrs, &handleIncomingMsg, portWithHandler.get());

            pthread_attr_destroy(&attrs);

            if (failed) {
                log() << "pthread_create failed: " << errnoWithDescription(failed) << endl;
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again));
            }
#endif  // __linux__

            portWithHandler.release();
            sleepAfterClosingPort.Dismiss();
        } catch (...) {
            Listener::globalTicketHolder.release();
            log() << "failed to create thread after accepting new connection, closing connection";
        }
    }

    virtual void setAsTimeTracker() {
        Listener::setAsTimeTracker();
    }

    virtual bool setupSockets() {
        return Listener::setupSockets();
    }

    void run() {
        initAndListen();
    }

    virtual bool useUnixSockets() const {
        return true;
    }

private:
    const std::shared_ptr<MessageHandler> _handler;

    /**
     * Handles incoming messages from a given socket.
     *
     * Terminating conditions:
     * 1. Assertions while handling the request.
     * 2. Socket is closed.
     * 3. Server is shutting down (based on inShutdown)
     *
     * @param arg this method is in charge of cleaning up the arg object.
     *
     * @return NULL
     */
    static void* handleIncomingMsg(void* arg) {
        TicketHolderReleaser connTicketReleaser(&Listener::globalTicketHolder);

        invariant(arg);
        unique_ptr<MessagingPortWithHandler> portWithHandler(
            static_cast<MessagingPortWithHandler*>(arg));
        const std::shared_ptr<MessageHandler> handler = portWithHandler->getHandler();

        setThreadName(std::string(str::stream() << "conn" << portWithHandler->connectionId()));
        portWithHandler->psock->setLogLevel(logger::LogSeverity::Debug(1));

        Message m;
        int64_t counter = 0;
        try {
            handler->connected(portWithHandler.get());
            ON_BLOCK_EXIT([handler]() { handler->close(); });

            while (!inShutdown()) {
                m.reset();
                portWithHandler->psock->clearCounters();

                if (!portWithHandler->recv(m)) {
                    if (!serverGlobalParams.quiet) {
                        int conns = Listener::globalTicketHolder.used() - 1;
                        const char* word = (conns == 1 ? " connection" : " connections");
                        log() << "end connection " << portWithHandler->psock->remoteString() << " ("
                              << conns << word << " now open)" << endl;
                    }
                    break;
                }

                handler->process(m, portWithHandler.get());
                networkCounter.hit(portWithHandler->psock->getBytesIn(),
                                   portWithHandler->psock->getBytesOut());

                // Occasionally we want to see if we're using too much memory.
                if ((counter++ & 0xf) == 0) {
                    markThreadIdle();
                }
            }
        } catch (AssertionException& e) {
            log() << "AssertionException handling request, closing client connection: " << e
                  << endl;
        } catch (SocketException& e) {
            log() << "SocketException handling request, closing client connection: " << e << endl;
        } catch (const DBException&
                     e) {  // must be right above std::exception to avoid catching subclasses
            log() << "DBException handling request, closing client connection: " << e << endl;
        } catch (std::exception& e) {
            error() << "Uncaught std::exception: " << e.what() << ", terminating" << endl;
            quickExit(EXIT_UNCAUGHT);
        }
        portWithHandler->shutdown();

        return NULL;
    }
};


MessageServer* createServer(const MessageServer::Options& opts,
                            std::shared_ptr<MessageHandler> handler) {
    return new PortMessageServer(opts, std::move(handler));
}

}  // namespace mongo
