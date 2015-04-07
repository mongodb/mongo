// client.cpp

/**
*    Copyright (C) 2009 10gen Inc.
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

/* Client represents a connection to the database (the server-side) and corresponds
   to an open socket (or logical connection if pooling on sockets) from a client.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exit.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using logger::LogComponent;

    boost::mutex Client::clientsMutex;
    ClientSet Client::clients;

    TSP_DEFINE(Client, currentClient)

    /**
     * This must be called whenever a new thread is started, so that active threads can be tracked
     * so each thread has a Client object in TLS.
     */
    void Client::initThread(const char *desc, AbstractMessagingPort *mp) {
        invariant(currentClient.get() == 0);

        std::string fullDesc;
        if (mp != NULL) {
            fullDesc = str::stream() << desc << mp->connectionId();
        }
        else {
            fullDesc = desc;
        }

        setThreadName(fullDesc.c_str());
        mongo::lastError.initThread();

        // Create the client obj, attach to thread
        Client* client = new Client(fullDesc, getGlobalServiceContext(), mp);
        AuthorizationSession::set(client,
                                  getGlobalAuthorizationManager()->makeAuthorizationSession());

        currentClient.reset(client);

        // This makes the client visible to maintenance threads
        boost::lock_guard<boost::mutex> clientLock(clientsMutex);
        clients.insert(client);
    }

    Client::Client(const std::string& desc,
                   ServiceContext* serviceContext,
                   AbstractMessagingPort *p)
        : ClientBasic(serviceContext, p),
          _desc(desc),
          _threadId(boost::this_thread::get_id()),
          _connectionId(p ? p->connectionId() : 0),
          _inDirectClient(false),
          _txn(NULL) {
    }

    Client::~Client() {
        if ( ! inShutdown() ) {
            // we can't clean up safely once we're in shutdown
            {
                boost::lock_guard<boost::mutex> clientLock(clientsMutex);
                clients.erase(this);
            }
        }
    }

    void Client::shutdown() {
        if (!inShutdown()) {
            boost::lock_guard<boost::mutex> clientLock(clientsMutex);
            clients.erase(this);
        }
    }

    void Client::reportState(BSONObjBuilder& builder) {
        builder.append("desc", desc());

        std::stringstream ss;
        ss << _threadId;
        builder.append("threadId", ss.str());

        if (_connectionId) {
            builder.appendNumber("connectionId", _connectionId);
        }
    }

    void Client::setOperationContext(OperationContext* txn) {
        // We can only set the OperationContext once before resetting it.
        invariant(txn != NULL && _txn == NULL);

        boost::unique_lock<SpinLock> uniqueLock(_lock);
        _txn = txn;
    }

    void Client::resetOperationContext() {
        invariant(_txn != NULL);
        boost::unique_lock<SpinLock> uniqueLock(_lock);
        _txn = NULL;
    }

    std::string Client::clientAddress(bool includePort) const {
        if (!hasRemote()) {
            return "";
        }
        if (includePort) {
            return getRemote().toString();
        }
        return getRemote().host();
    }

    ClientBasic* ClientBasic::getCurrent() {
        return currentClient.get();
    }

    void saveGLEStats(const BSONObj& result, const std::string& conn) {
        // This can be called in mongod, which is unfortunate.  To fix this,
        // we can redesign how connection pooling works on mongod for sharded operations.
    }

} // namespace mongo
