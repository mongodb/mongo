/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard_connection.h"

#include <set>

#include "mongo/db/commands.h"
#include "mongo/db/lasterror.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/version_manager.h"
#include "mongo/s/grid.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

using std::unique_ptr;
using std::map;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

namespace {

class ClientConnections;

/**
 * Class which tracks ClientConnections (the client connection pool) for each incoming
 * connection, allowing stats access.
 */
class ActiveClientConnections {
public:
    void add(const ClientConnections* cc) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _clientConnections.insert(cc);
    }

    void remove(const ClientConnections* cc) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _clientConnections.erase(cc);
    }

    void appendInfo(BSONObjBuilder& b);

private:
    stdx::mutex _mutex;
    set<const ClientConnections*> _clientConnections;

} activeClientConnections;

/**
 * Command to allow access to the sharded conn pool information in mongos.
 */
class ShardedPoolStats : public Command {
public:
    ShardedPoolStats() : Command("shardConnPoolStats") {}
    virtual void help(stringstream& help) const {
        help << "stats about the shard connection pool";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }

    // Same privs as connPoolStats
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::connPoolStats);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     mongo::BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     mongo::BSONObjBuilder& result) {
        // Connection information
        executor::ConnectionPoolStats stats{};
        shardConnectionPool.appendConnectionStats(&stats);
        stats.appendToBSON(result);

        // Thread connection information
        activeClientConnections.appendInfo(result);

        return true;
    }

} shardedPoolStatsCmd;

/**
 * holds all the actual db connections for a client to various servers 1 per thread, so
 * doesn't have to be thread safe.
 */
class ClientConnections {
    MONGO_DISALLOW_COPYING(ClientConnections);

public:
    struct Status {
        Status() : created(0), avail(0) {}

        // May be read concurrently, but only written from
        // this thread.
        long long created;
        DBClientBase* avail;
    };

    // Gets or creates the status object for the host
    Status* _getStatus(const string& addr) {
        scoped_spinlock lock(_lock);
        Status*& temp = _hosts[addr];
        if (!temp) {
            temp = new Status();
        }

        return temp;
    }

    ClientConnections() {
        // Start tracking client connections
        activeClientConnections.add(this);
    }

    ~ClientConnections() {
        // Stop tracking these client connections
        activeClientConnections.remove(this);

        releaseAll(true);
    }

    void releaseAll(bool fromDestructor = false) {
        // Don't need spinlock protection because if not in the destructor, we don't modify
        // _hosts, and if in the destructor we are not accessible to external threads.
        for (HostMap::iterator i = _hosts.begin(); i != _hosts.end(); ++i) {
            const string addr = i->first;
            Status* ss = i->second;
            invariant(ss);

            if (ss->avail) {
                // If we're shutting down, don't want to initiate release mechanism as it is
                // slow, and isn't needed since all connections will be closed anyway.
                if (inShutdown()) {
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

    DBClientBase* get(const string& addr, const string& ns) {
        {
            // We want to report ns stats
            scoped_spinlock lock(_lock);
            if (ns.size() > 0)
                _seenNS.insert(ns);
        }

        Status* s = _getStatus(addr);

        unique_ptr<DBClientBase> c;
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

    void done(const string& addr, DBClientBase* conn) {
        Status* s = _hosts[addr];
        verify(s);

        const bool isConnGood = shardConnectionPool.isConnectionGood(addr, conn);

        if (s->avail != NULL) {
            warning() << "Detected additional sharded connection in the "
                      << "thread local pool for " << addr;

            if (DBException::traceExceptions) {
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

        // Note: Although we try our best to clear bad connections as much as possible,
        // some of them can still slip through because of how ClientConnections are being
        // used - as thread local variables. This means that threads won't be able to
        // see the s->avail connection of other threads.

        s->avail = conn;
    }

    void checkVersions(OperationContext* txn, const string& ns) {
        vector<ShardId> all;
        grid.shardRegistry()->getAllShardIds(&all);

        // Don't report exceptions here as errors in GetLastError
        LastError::Disabled ignoreForGLE(&LastError::get(cc()));

        // Now only check top-level shard connections
        for (const ShardId& shardId : all) {
            try {
                const auto shard = grid.shardRegistry()->getShard(txn, shardId);
                if (!shard) {
                    continue;
                }

                string sconnString = shard->getConnString().toString();
                Status* s = _getStatus(sconnString);

                if (!s->avail) {
                    s->avail = shardConnectionPool.get(sconnString);
                    s->created++;  // After, so failed creation doesn't get counted
                }

                versionManager.checkShardVersionCB(txn, s->avail, ns, false, 1);
            } catch (const DBException& ex) {
                warning() << "problem while initially checking shard versions on"
                          << " " << shardId << causedBy(ex);

                // NOTE: This is only a heuristic, to avoid multiple stale version retries
                // across multiple shards, and does not affect correctness.
            }
        }
    }

    void release(const string& addr, DBClientBase* conn) {
        shardConnectionPool.release(addr, conn);
    }

    /**
     * Appends info about the client connection pool to a BOBuilder
     * Safe to call with activeClientConnections lock
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
        for (set<string>::const_iterator i = _seenNS.begin(); i != _seenNS.end(); ++i) {
            nsArrB.append(*i);
        }
        nsArrB.done();
    }

    // Protects only the creation of new entries in the _hosts and _seenNS map
    // from external threads.  Reading _hosts / _seenNS in this thread doesn't
    // need protection.
    mutable SpinLock _lock;
    typedef map<string, Status*, DBConnectionPool::serverNameCompare> HostMap;
    HostMap _hosts;
    set<string> _seenNS;

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

    void forgetNS(const string& ns) {
        scoped_spinlock lock(_lock);
        _seenNS.erase(ns);
    }

    // -----

    static thread_specific_ptr<ClientConnections> _perThread;

    static ClientConnections* threadInstance() {
        ClientConnections* cc = _perThread.get();
        if (!cc) {
            cc = new ClientConnections();
            _perThread.reset(cc);
        }
        return cc;
    }
};


void ActiveClientConnections::appendInfo(BSONObjBuilder& b) {
    BSONArrayBuilder arr(64 * 1024);  // There may be quite a few threads

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        for (set<const ClientConnections*>::const_iterator i = _clientConnections.begin();
             i != _clientConnections.end();
             ++i) {
            BSONObjBuilder bb(arr.subobjStart());
            (*i)->appendInfo(bb);
            bb.done();
        }
    }

    b.appendArray("threads", arr.obj());
}

thread_specific_ptr<ClientConnections> ClientConnections::_perThread;

}  // namespace

// The global connection pool
DBConnectionPool shardConnectionPool;

// Different between mongos and mongod
void usingAShardConnection(const string& addr);

ShardConnection::ShardConnection(const ConnectionString& connectionString,
                                 const string& ns,
                                 std::shared_ptr<ChunkManager> manager)
    : _cs(connectionString), _ns(ns), _manager(manager), _finishedInit(false) {
    invariant(_cs.isValid());

    // Make sure we specified a manager for the correct namespace
    if (_ns.size() && _manager) {
        invariant(_manager->getns() == _ns);
    }

    _conn = ClientConnections::threadInstance()->get(_cs.toString(), _ns);
    usingAShardConnection(_cs.toString());
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
        auto txn = client.getOperationContext();
        invariant(txn);
        _setVersion = versionManager.checkShardVersionCB(txn, this, false, 1);
    } else {
        // Make sure we didn't specify a manager for a non-versionable connection (i.e. config)
        verify(!_manager);
        _setVersion = false;
    }
}

void ShardConnection::done() {
    if (_conn) {
        ClientConnections::threadInstance()->done(_cs.toString(), _conn);
        _conn = 0;
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

void ShardConnection::checkMyConnectionVersions(OperationContext* txn, const string& ns) {
    ClientConnections::threadInstance()->checkVersions(txn, ns);
}

void ShardConnection::releaseMyConnections() {
    ClientConnections::threadInstance()->releaseAll();
}

void ShardConnection::clearPool() {
    shardConnectionPool.clear();
    ClientConnections::threadInstance()->clearPool();
}

void ShardConnection::forgetNS(const string& ns) {
    ClientConnections::threadInstance()->forgetNS(ns);
}

}  // namespace mongo
