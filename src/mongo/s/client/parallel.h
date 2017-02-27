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

#include "mongo/client/dbclientinterface.h"
#include "mongo/client/query.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class ChunkManager;
class DBClientCursor;
class DBClientCursorHolder;
class OperationContext;
struct ParallelConnectionMetadata;
struct ParallelConnectionState;
class StaleConfigException;

struct CommandInfo {
    CommandInfo() = default;

    CommandInfo(const std::string& vns, const BSONObj& filter, const BSONObj& collation)
        : versionedNS(vns), cmdFilter(filter), cmdCollation(collation) {}

    bool isEmpty() const {
        return versionedNS.empty();
    }

    std::string toString() const {
        return str::stream() << "CInfo "
                             << BSON("v_ns" << versionedNS << "filter" << cmdFilter << "collation"
                                            << cmdCollation);
    }

    std::string versionedNS;
    BSONObj cmdFilter;
    BSONObj cmdCollation;
};

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
    ParallelSortClusteredCursor(const QuerySpec& qSpec, const CommandInfo& cInfo);

    // DEPRECATED legacy constructor for pure mergesort functionality - do not use
    ParallelSortClusteredCursor(const std::set<std::string>& servers,
                                const std::string& ns,
                                const Query& q,
                                int options = 0,
                                const BSONObj& fields = BSONObj());

    ~ParallelSortClusteredCursor();

    void init(OperationContext* txn);

    bool more();

    BSONObj next();

    /**
     * Returns the set of shards with open cursors.
     */
    void getQueryShardIds(std::set<ShardId>& shardIds) const;

    std::shared_ptr<DBClientCursor> getShardCursor(const ShardId& shardId) const;

private:
    using ShardCursorsMap = std::map<ShardId, ParallelConnectionMetadata>;

    void fullInit(OperationContext* txn);
    void startInit(OperationContext* txn);
    void finishInit(OperationContext* txn);

    bool isCommand() {
        return NamespaceString(_qSpec.ns()).isCommand();
    }

    void _finishCons();

    void _markStaleNS(OperationContext* txn,
                      const NamespaceString& staleNS,
                      const StaleConfigException& e,
                      bool& forceReload);
    void _handleStaleNS(OperationContext* txn, const NamespaceString& staleNS, bool forceReload);

    bool _didInit;
    bool _done;

    QuerySpec _qSpec;
    CommandInfo _cInfo;

    // Count round-trips req'd for namespaces and total
    std::map<std::string, int> _staleNSMap;

    int _totalTries;

    ShardCursorsMap _cursorMap;

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
                                      std::shared_ptr<ParallelConnectionState> state /* in & out */,
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
 * Throws a RecvStaleConfigException wrapping the stale error document in this cursor when the
 * ShardConfigStale flag is set or a command returns a ErrorCodes::SendStaleConfig error code.
 */
void throwCursorStale(DBClientCursor* cursor);

}  // namespace mongo
