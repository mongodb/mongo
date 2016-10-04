// listen.h

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

#pragma once

#include <set>
#include <string>
#include <vector>

#include "mongo/config.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/net/abstract_message_port.h"
#include "mongo/util/net/sock.h"

namespace mongo {

const int DEFAULT_MAX_CONN = 1000000;

class ServiceContext;

class Listener {
    MONGO_DISALLOW_COPYING(Listener);

public:
    /** Obtain the Listener for a provided ServiceContext. */
    static Listener* get(ServiceContext* context);

    Listener(const std::string& name,
             const std::string& ip,
             int port,
             ServiceContext* ctx,
             bool setAsServiceCtxDecoration,
             bool logConnect = true);

    virtual ~Listener();

    void initAndListen();  // never returns unless error (start a thread)

    /* spawn a thread, etc., then return */
    virtual void accepted(std::unique_ptr<AbstractMessagingPort> mp) = 0;

    const int _port;

    /**
     * Allocate sockets for the listener and set _setupSocketsSuccessful to true
     * iff the process was successful.
     * Returns _setupSocketsSuccessful.
     */
    bool setupSockets();

    /**
     * Blocks until initAndListen has been called on this instance and gotten far enough that
     * it is ready to receive incoming network requests.
     */
    void waitUntilListening() const;

    void shutdown();

private:
    std::vector<SockAddr> _mine;
    std::vector<SOCKET> _socks;
    std::string _name;
    std::string _ip;
    bool _setupSocketsSuccessful;
    bool _logConnect;
    mutable stdx::mutex _readyMutex;                   // Protects _ready
    mutable stdx::condition_variable _readyCondition;  // Used to wait for changes to _ready
    // Boolean that indicates whether this Listener is ready to accept incoming network requests
    bool _ready;
    AtomicBool _finished{false};

    ServiceContext* _ctx;
    bool _setAsServiceCtxDecoration;

    virtual void _accepted(const std::shared_ptr<Socket>& psocket, long long connectionId);

#ifdef MONGO_CONFIG_SSL
    SSLManagerInterface* _ssl;
#endif

    void _logListen(int port, bool ssl);

    virtual bool useUnixSockets() const {
        return false;
    }

public:
    /** the "next" connection number.  every connection to this process has a unique number */
    static AtomicInt64 globalConnectionNumber;

    /** keeps track of how many allowed connections there are and how many are being used*/
    static TicketHolder globalTicketHolder;

    /** makes sure user input is sane */
    static void checkTicketNumbers();
};

class ListeningSockets {
public:
    ListeningSockets() : _sockets(new std::set<int>()), _socketPaths(new std::set<std::string>()) {}
    void add(int sock) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _sockets->insert(sock);
    }
    void addPath(const std::string& path) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _socketPaths->insert(path);
    }
    void remove(int sock) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _sockets->erase(sock);
    }
    void closeAll();
    static ListeningSockets* get();

private:
    stdx::mutex _mutex;
    std::set<int>* _sockets;
    std::set<std::string>* _socketPaths;  // for unix domain sockets
    static ListeningSockets* _instance;
};
}
