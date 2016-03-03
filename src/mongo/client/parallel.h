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

/**
   tools for working in parallel/sharded/clustered environment
 */

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_connection.h"

namespace mongo {

class DBClientCursorHolder;
class OperationContext;
class StaleConfigException;
class ParallelConnectionMetadata;


class CommandInfo {
public:
    std::string versionedNS;
    BSONObj cmdFilter;

    CommandInfo() {}
    CommandInfo(const std::string& vns, const BSONObj& filter)
        : versionedNS(vns), cmdFilter(filter) {}

    bool isEmpty() {
        return versionedNS.size() == 0;
    }

    std::string toString() const {
        return str::stream() << "CInfo " << BSON("v_ns" << versionedNS << "filter" << cmdFilter);
    }
};

class DBClientCursor;
typedef std::shared_ptr<DBClientCursor> DBClientCursorPtr;

class ParallelConnectionState {
public:
    ParallelConnectionState() : count(0), done(false) {}

    // Please do not reorder. cursor destructor can use conn.
    // On a related note, never attempt to cleanup these pointers manually.
    std::shared_ptr<ShardConnection> conn;
    DBClientCursorPtr cursor;

    // Version information
    std::shared_ptr<ChunkManager> manager;
    std::shared_ptr<Shard> primary;

    // Cursor status information
    long long count;
    bool done;

    BSONObj toBSON() const;

    std::string toString() const {
        return str::stream() << "PCState : " << toBSON();
    }
};

typedef ParallelConnectionState PCState;
typedef std::shared_ptr<PCState> PCStatePtr;

class ParallelConnectionMetadata {
public:
    ParallelConnectionMetadata()
        : retryNext(false), initialized(false), finished(false), completed(false), errored(false) {}

    ~ParallelConnectionMetadata() {
        cleanup(true);
    }

    void cleanup(bool full = true);

    PCStatePtr pcState;

    bool retryNext;

    bool initialized;
    bool finished;
    bool completed;

    bool errored;

    BSONObj toBSON() const;

    std::string toString() const {
        return str::stream() << "PCMData : " << toBSON();
    }
};

typedef ParallelConnectionMetadata PCMData;
typedef std::shared_ptr<PCMData> PCMDataPtr;

/**
 * Runs a query in parallel across N servers, enforcing compatible chunk versions for queries
 * across all shards.
 *
 * If CommandInfo is provided, the ParallelCursor does not use the direct .$cmd namespace in the
 * query spec, but instead enforces versions across another namespace specified by CommandInfo.
 * This is to support commands like:
 * db.runCommand({ fileMD5 : "<coll name>" })
 *
 * There is a deprecated legacy mode as well which effectively does a merge-sort across a number
 * of servers, but does not correctly enforce versioning (used only in mapreduce).
 */
class ParallelSortClusteredCursor {
public:
    ParallelSortClusteredCursor(const QuerySpec& qSpec, const CommandInfo& cInfo = CommandInfo());

    // DEPRECATED legacy constructor for pure mergesort functionality - do not use
    ParallelSortClusteredCursor(const std::set<std::string>& servers,
                                const std::string& ns,
                                const Query& q,
                                int options = 0,
                                const BSONObj& fields = BSONObj());

    ~ParallelSortClusteredCursor();

    /** call before using */
    void init(OperationContext* txn);

    bool more();
    BSONObj next();

    /**
     * Returns the set of shards with open cursors.
     */
    void getQueryShardIds(std::set<ShardId>& shardIds);

    DBClientCursorPtr getShardCursor(const ShardId& shardId);

private:
    void fullInit(OperationContext* txn);
    void startInit(OperationContext* txn);
    void finishInit(OperationContext* txn);

    bool isCommand() {
        return NamespaceString(_qSpec.ns()).isCommand();
    }

    void _finishCons();

    void _markStaleNS(const NamespaceString& staleNS,
                      const StaleConfigException& e,
                      bool& forceReload,
                      bool& fullReload);
    void _handleStaleNS(OperationContext* txn,
                        const NamespaceString& staleNS,
                        bool forceReload,
                        bool fullReload);

    bool _didInit;
    bool _done;

    QuerySpec _qSpec;
    CommandInfo _cInfo;

    // Count round-trips req'd for namespaces and total
    std::map<std::string, int> _staleNSMap;

    int _totalTries;

    std::map<ShardId, PCMData> _cursorMap;

    // LEGACY BELOW
    int _numServers;
    int _lastFrom;
    std::set<std::string> _servers;
    BSONObj _sortKey;

    DBClientCursorHolder* _cursors;
    int _needToSkip;

    /**
     * Setups the shard version of the connection. When using a replica
     * set connection and the primary cannot be reached, the version
     * will not be set if the slaveOk flag is set.
     */
    void setupVersionAndHandleSlaveOk(OperationContext* txn,
                                      PCStatePtr state /* in & out */,
                                      const ShardId& shardId,
                                      std::shared_ptr<Shard> primary /* in */,
                                      const NamespaceString& ns,
                                      const std::string& vinfo,
                                      std::shared_ptr<ChunkManager> manager /* in */);

    // LEGACY init - Needed for map reduce
    void _oldInit();

    // LEGACY - Needed ONLY for _oldInit
    std::string _ns;
    BSONObj _query;
    int _options;
    BSONObj _fields;
    int _batchSize;
};


/**
 * Helper class to manage ownership of opened cursors while merging results.
 *
 * TODO:  Choose one set of ownership semantics so that this isn't needed - merge sort via
 * mapreduce is the main issue since it has no metadata and this holder owns the cursors.
 */
class DBClientCursorHolder {
public:
    DBClientCursorHolder() {}
    ~DBClientCursorHolder() {}

    void reset(DBClientCursor* cursor, ParallelConnectionMetadata* pcmData) {
        _cursor.reset(cursor);
        _pcmData.reset(pcmData);
    }

    DBClientCursor* get() {
        return _cursor.get();
    }
    ParallelConnectionMetadata* getMData() {
        return _pcmData.get();
    }

    void release() {
        _cursor.release();
        _pcmData.release();
    }

private:
    std::unique_ptr<DBClientCursor> _cursor;
    std::unique_ptr<ParallelConnectionMetadata> _pcmData;
};

void throwCursorStale(DBClientCursor* cursor);

}  // namespace mongo
