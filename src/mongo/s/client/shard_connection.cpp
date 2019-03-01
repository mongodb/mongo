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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard_connection.h"

#include <set>

#include "mongo/base/init.h"
#include "mongo/db/lasterror.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_connection_gen.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/version_manager.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"

namespace mongo {
namespace {

class ClientConnections;

/**
 * Class which tracks ClientConnections (the client connection pool) for each incoming connection,
 * allowing stats access.
 */
class ActiveClientConnections {
public:
    void add(const ClientConnections* cc) {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        _clientConnections.insert(cc);
    }

    void remove(const ClientConnections* cc) {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        _clientConnections.erase(cc);
    }

    void appendInfo(BSONObjBuilder* b) const;

private:
    mutable stdx::mutex _mutex;
    std::set<const ClientConnections*> _clientConnections;

} activeClientConnections;

/**
 * Holds all the actual db connections for a client to various servers 1 per thread, so doesn't have
 * to be thread safe.
 */
class ClientConnections {
    MONGO_DISALLOW_COPYING(ClientConnections);

public:
    struct Status {
        // May be read concurrently, but only written from this thread
        long long created = 0;
        DBClientBase* avail = nullptr;
    };

    ClientConnections() {
        // Start tracking client connections
        activeClientConnections.add(this);
    }

    ~ClientConnections() {
        // Stop tracking these client connections
        activeClientConnections.remove(this);

        releaseAll(true);
    }

    static ClientConnections* threadInstance() {
        if (!_perThread) {
            _perThread = stdx::make_unique<ClientConnections>();
        }
        return _perThread.get();
    }

    DBClientBase* get(const std::string& addr, const std::string& ns) {
        {
            // We want to report ns stats
            scoped_spinlock lock(_lock);
            if (ns.size() > 0)
                _seenNS.insert(ns);
        }

        Status* s = _getStatus(addr);

        std::unique_ptr<DBClientBase> c;
        if (s->avail) {
            c.reset(s->avail);
            s->avail = 0;

            // May throw an exception
            shardConnectionPool.onHandedOut(c.get());
        } else {
            c.reset(shardConnectionPool.get(addr));

            // After, so failed creation doesn't get counted
            s->created++;
        }

        return c.release();
    }

    void releaseAll(bool fromDestructor = false) {
        // Don't need spinlock protection because if not in the destructor, we don't modify
        // _hosts, and if in the destructor we are not accessible to external threads.
        for (HostMap::iterator i = _hosts.begin(); i != _hosts.end(); ++i) {
            const auto addr = i->first;
            Status* ss = i->second;
            invariant(ss);

            if (ss->avail) {
                // If we're shutting down, don't want to initiate release mechanism as it is
                // slow, and isn't needed since all connections will be closed anyway.
                if (globalInShutdownDeprecated()) {
                    if (versionManager.isVersionableCB(ss->avail)) {
                        versionManager.resetShardVersionCB(ss->avail);
                    }

                    delete ss->avail;
                } else {
                    release(addr, ss->avail);
                }

                ss->avail = 0;
            }

            if (fromDestructor) {
                delete ss;
            }
        }

        if (fromDestructor) {
            _hosts.clear();
        }
    }

    void done(const std::string& addr, DBClientBase* conn) {
        Status* s = _hosts[addr];
        verify(s);

        const bool isConnGood = shardConnectionPool.isConnectionGood(addr, conn);

        if (s->avail != NULL) {
            warning() << "Detected additional sharded connection in the "
                      << "thread local pool for " << addr;

            if (DBException::traceExceptions.load()) {
                // There shouldn't be more than one connection checked out to the same
                // host on the same thread.
                printStackTrace();
            }

            if (!isConnGood) {
                delete s->avail;
                s->avail = NULL;
            }

            // Let the internal pool handle the bad connection, this can also
            // update the lower bounds for the known good socket creation time
            // for this host.
            release(addr, conn);
            return;
        }

        if (!isConnGood) {
            // Let the internal pool handle the bad connection.
            release(addr, conn);
            return;
        }

        // Note: Although we try our best to clear bad connections as much as possible, some of them
        // can still slip through because of how ClientConnections are being used - as thread local
        // variables. This means that threads won't be able to see the s->avail connection of other
        // threads.
        s->avail = conn;
    }

    void checkVersions(OperationContext* opCtx, const std::string& ns) {
        auto const shardRegistry = Grid::get(opCtx)->shardRegistry();

        std::vector<ShardId> all;
        shardRegistry->getAllShardIdsNoReload(&all);

        // Don't report exceptions here as errors in GetLastError
        LastError::Disabled ignoreForGLE(&LastError::get(cc()));

        // Now only check top-level shard connections
        for (const ShardId& shardId : all) {
            try {
                auto shardStatus = shardRegistry->getShard(opCtx, shardId);
                if (!shardStatus.isOK()) {
                    invariant(shardStatus == ErrorCodes::ShardNotFound);
                    continue;
                }
                const auto shard = shardStatus.getValue();

                const auto sconnString = shard->getConnString().toString();

                Status* s = _getStatus(sconnString);
                if (!s->avail) {
                    s->avail = shardConnectionPool.get(sconnString);
                    s->created++;  // After, so failed creation doesn't get counted
                }

                versionManager.checkShardVersionCB(opCtx, s->avail, ns, false, 1);
            } catch (const DBException& ex) {
                warning() << "Problem while initially checking shard versions on"
                          << " " << shardId << causedBy(redact(ex));

                // NOTE: This is only a heuristic, to avoid multiple stale version retries across
                // multiple shards, and does not affect correctness.
            }
        }
    }

    void release(const std::string& addr, DBClientBase* conn) {
        shardConnectionPool.release(addr, conn);
    }

    void forgetNS(const std::string& ns) {
        scoped_spinlock lock(_lock);
        _seenNS.erase(ns);
    }

    /**
     * Clears the connections kept by this pool (ie, not including the global pool)
     */
    void clearPool() {
        for (HostMap::iterator iter = _hosts.begin(); iter != _hosts.end(); ++iter) {
            if (iter->second->avail != NULL) {
                delete iter->second->avail;
            }
            delete iter->second;
        }

        _hosts.clear();
    }

    /**
     * Appends info about the client connection pool to a BSONObjBuilder. Safe to call with
     * activeClientConnections lock.
     */
    void appendInfo(BSONObjBuilder& b) const {
        scoped_spinlock lock(_lock);

        BSONArrayBuilder hostsArrB(b.subarrayStart("hosts"));
        for (HostMap::const_iterator i = _hosts.begin(); i != _hosts.end(); ++i) {
            BSONObjBuilder bb(hostsArrB.subobjStart());
            bb.append("host", i->first);
            bb.append("created", i->second->created);
            bb.appendBool("avail", static_cast<bool>(i->second->avail));
            bb.done();
        }
        hostsArrB.done();

        BSONArrayBuilder nsArrB(b.subarrayStart("seenNS"));
        for (const auto& ns : _seenNS) {
            nsArrB.append(ns);
        }
        nsArrB.done();
    }

private:
    /**
     * Gets or creates the status object for the host.
     */
    Status* _getStatus(const std::string& addr) {
        scoped_spinlock lock(_lock);
        Status*& temp = _hosts[addr];
        if (!temp) {
            temp = new Status();
        }

        return temp;
    }

    static thread_local std::unique_ptr<ClientConnections> _perThread;

    // Protects only the creation of new entries in the _hosts and _seenNS map from external
    // threads. Reading _hosts/_seenNS in this thread doesn't need protection.
    mutable SpinLock _lock;

    using HostMap = std::map<std::string, Status*, DBConnectionPool::serverNameCompare>;
    HostMap _hosts;
    std::set<std::string> _seenNS;
};

void ActiveClientConnections::appendInfo(BSONObjBuilder* b) const {
    // Preallocate the buffer because there may be quite a few threads to report
    BSONArrayBuilder arr(64 * 1024);

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        for (const auto* conn : _clientConnections) {
            BSONObjBuilder bb(arr.subobjStart());
            conn->appendInfo(bb);
            bb.doneFast();
        }
    }

    b->appendArray("threads", arr.obj());
}

thread_local std::unique_ptr<ClientConnections> ClientConnections::_perThread;

// This must run after CLI/config --setParameter has been parsed to be useful.
MONGO_INITIALIZER_WITH_PREREQUISITES(InitializeShardedConnectionPool,
                                     ("EndPostStartupOptionStorage"))
(InitializerContext* context) {
    shardConnectionPool.setName("sharded connection pool");
    shardConnectionPool.setMaxPoolSize(gMaxShardedConnsPerHost);
    shardConnectionPool.setMaxInUse(gMaxShardedInUseConnsPerHost);
    shardConnectionPool.setIdleTimeout(gShardedConnPoolIdleTimeout);

    return Status::OK();
}

}  // namespace

DBConnectionPool shardConnectionPool;

ShardConnection::ShardConnection(OperationContext* opCtx,
                                 const ConnectionString& connectionString,
                                 const std::string& ns,
                                 std::shared_ptr<ChunkManager> manager)
    : _cs(connectionString), _ns(ns), _manager(manager) {
    invariant(_cs.isValid());

    // This code should never run under a cross-shard transaction
    invariant(!TransactionRouter::get(opCtx));

    // Make sure we specified a manager for the correct namespace
    if (_ns.size() && _manager) {
        invariant(_manager->getns().ns() == _ns);
    }

    auto csString = _cs.toString();

    _conn = ClientConnections::threadInstance()->get(csString, _ns);
    if (isMongos()) {
        // In mongos, we record this connection as having been used for useful work to provide
        // useful information in getLastError.
        ClusterLastErrorInfo::get(opCtx->getClient())->addShardHost(csString);
    }
}

ShardConnection::~ShardConnection() {
    if (_conn) {
        if (_conn->isFailed()) {
            if (_conn->getSockCreationMicroSec() == DBClientBase::INVALID_SOCK_CREATION_TIME) {
                kill();
            } else {
                // The pool takes care of deleting the failed connection - this
                // will also trigger disposal of older connections in the pool
                done();
            }
        } else {
            // see done() comments above for why we log this line
            log() << "sharded connection to " << _conn->getServerAddress()
                  << " not being returned to the pool";

            kill();
        }
    }
}

void ShardConnection::_finishInit() {
    if (_finishedInit)
        return;
    _finishedInit = true;

    if (versionManager.isVersionableCB(_conn)) {
        auto& client = cc();
        auto opCtx = client.getOperationContext();
        invariant(opCtx);
        _setVersion = versionManager.checkShardVersionCB(opCtx, this, false, 1);
    } else {
        // Make sure we didn't specify a manager for a non-versionable connection (i.e. config)
        invariant(!_manager);
        _setVersion = false;
    }
}

void ShardConnection::done() {
    if (_conn) {
        ClientConnections::threadInstance()->done(_cs.toString(), _conn);
        _conn = nullptr;
        _finishedInit = true;
    }
}

void ShardConnection::kill() {
    if (_conn) {
        if (versionManager.isVersionableCB(_conn)) {
            versionManager.resetShardVersionCB(_conn);
        }

        if (_conn->isFailed()) {
            // Let the pool know about the bad connection and also delegate disposal to it.
            ClientConnections::threadInstance()->done(_cs.toString(), _conn);
        } else {
            delete _conn;
        }

        _conn = 0;
        _finishedInit = true;
    }
}

void ShardConnection::reportActiveClientConnections(BSONObjBuilder* builder) {
    activeClientConnections.appendInfo(builder);
}

void ShardConnection::checkMyConnectionVersions(OperationContext* opCtx, const std::string& ns) {
    ClientConnections::threadInstance()->checkVersions(opCtx, ns);
}

void ShardConnection::releaseMyConnections() {
    ClientConnections::threadInstance()->releaseAll();
}

void ShardConnection::clearPool() {
    shardConnectionPool.clear();
    ClientConnections::threadInstance()->clearPool();
}

void ShardConnection::forgetNS(const std::string& ns) {
    ClientConnections::threadInstance()->forgetNS(ns);
}

}  // namespace mongo
