// parallel.cpp
/*
 *    Copyright 2010 10gen Inc.
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

#include "mongo/client/parallel.h"

#include "mongo/client/connpool.h"
#include "mongo/client/constants.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/query/query_request.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_exception.h"

namespace mongo {

using std::shared_ptr;
using std::list;
using std::map;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

namespace dps = ::mongo::dotted_path_support;

LabeledLevel pc("pcursor", 2);

void ParallelSortClusteredCursor::init(OperationContext* txn) {
    if (_didInit)
        return;
    _didInit = true;

    if (!_qSpec.isEmpty()) {
        fullInit(txn);
    } else {
        // You can only get here by using the legacy constructor
        // TODO: Eliminate this
        _oldInit();
    }
}

/**
 * Throws a RecvStaleConfigException wrapping the stale error document in this cursor when the
 * ShardConfigStale flag is set or a command returns a ErrorCodes::SendStaleConfig error code.
 */
void throwCursorStale(DBClientCursor* cursor) {
    verify(cursor);

    if (cursor->hasResultFlag(ResultFlag_ShardConfigStale)) {
        BSONObj error;
        cursor->peekError(&error);
        throw RecvStaleConfigException("query returned a stale config error", error);
    }

    if (NamespaceString(cursor->getns()).isCommand()) {
        // Commands that care about versioning (like the count or geoNear command) sometimes
        // return with the stale config error code, but don't set the ShardConfigStale result
        // flag on the cursor.
        // TODO: Standardize stale config reporting.
        BSONObj res = cursor->peekFirst();
        if (res.hasField("code") && res["code"].Number() == ErrorCodes::SendStaleConfig) {
            throw RecvStaleConfigException("command returned a stale config error", res);
        }
    }
}

/**
 * Throws an exception wrapping the error document in this cursor when the error flag is set.
 */
static void throwCursorError(DBClientCursor* cursor) {
    verify(cursor);

    if (cursor->hasResultFlag(ResultFlag_ErrSet)) {
        BSONObj o = cursor->next();
        throw UserException(o["code"].numberInt(), o["$err"].str());
    }
}

// --------  ParallelSortClusteredCursor -----------

ParallelSortClusteredCursor::ParallelSortClusteredCursor(const QuerySpec& qSpec,
                                                         const CommandInfo& cInfo)
    : _qSpec(qSpec), _cInfo(cInfo), _totalTries(0) {
    _done = false;
    _didInit = false;

    _finishCons();
}

// LEGACY Constructor
ParallelSortClusteredCursor::ParallelSortClusteredCursor(const set<string>& servers,
                                                         const string& ns,
                                                         const Query& q,
                                                         int options,
                                                         const BSONObj& fields)
    : _servers(servers) {
    _sortKey = q.getSort().copy();
    _needToSkip = 0;

    _done = false;
    _didInit = false;

    // Populate legacy fields
    _ns = ns;
    _query = q.obj.getOwned();
    _options = options;
    _fields = fields.getOwned();
    _batchSize = 0;

    _finishCons();
}

void ParallelSortClusteredCursor::_finishCons() {
    _numServers = _servers.size();
    _lastFrom = 0;
    _cursors = 0;

    if (!_qSpec.isEmpty()) {
        _needToSkip = _qSpec.ntoskip();
        _cursors = 0;
        _sortKey = _qSpec.sort();
        _fields = _qSpec.fields();
    }

    // Partition sort key fields into (a) text meta fields and (b) all other fields.
    set<string> textMetaSortKeyFields;
    set<string> normalSortKeyFields;

    // Transform _sortKey fields {a:{$meta:"textScore"}} into {a:-1}, in order to apply the
    // merge sort for text metadata in the correct direction.
    BSONObjBuilder transformedSortKeyBuilder;

    BSONObjIterator sortKeyIt(_sortKey);
    while (sortKeyIt.more()) {
        BSONElement e = sortKeyIt.next();
        if (QueryRequest::isTextScoreMeta(e)) {
            textMetaSortKeyFields.insert(e.fieldName());
            transformedSortKeyBuilder.append(e.fieldName(), -1);
        } else {
            normalSortKeyFields.insert(e.fieldName());
            transformedSortKeyBuilder.append(e);
        }
    }
    _sortKey = transformedSortKeyBuilder.obj();

    // Verify that that all text metadata sort fields are in the projection.  For all other sort
    // fields, copy them into the projection if they are missing (and if projection is
    // negative).
    if (!_sortKey.isEmpty() && !_fields.isEmpty()) {
        BSONObjBuilder b;
        bool isNegative = false;
        {
            BSONObjIterator i(_fields);
            while (i.more()) {
                BSONElement e = i.next();
                b.append(e);

                string fieldName = e.fieldName();

                if (QueryRequest::isTextScoreMeta(e)) {
                    textMetaSortKeyFields.erase(fieldName);
                } else {
                    // exact field
                    bool found = normalSortKeyFields.erase(fieldName);

                    // subfields
                    set<string>::const_iterator begin =
                        normalSortKeyFields.lower_bound(fieldName + ".\x00");
                    set<string>::const_iterator end =
                        normalSortKeyFields.lower_bound(fieldName + ".\xFF");
                    normalSortKeyFields.erase(begin, end);

                    if (!e.trueValue()) {
                        uassert(13431,
                                "have to have sort key in projection and removing it",
                                !found && begin == end);
                    } else if (!e.isABSONObj()) {
                        isNegative = true;
                    }
                }
            }
        }

        if (isNegative) {
            for (set<string>::const_iterator it(normalSortKeyFields.begin()),
                 end(normalSortKeyFields.end());
                 it != end;
                 ++it) {
                b.append(*it, 1);
            }
        }

        _fields = b.obj();
    }

    if (!_qSpec.isEmpty()) {
        _qSpec.setFields(_fields);
    }

    uassert(
        17306, "have to have all text meta sort keys in projection", textMetaSortKeyFields.empty());
}

void ParallelConnectionMetadata::cleanup(bool full) {
    if (full || errored)
        retryNext = false;

    if (!retryNext && pcState) {
        if (initialized && !errored) {
            verify(pcState->cursor);
            verify(pcState->conn);

            if (!finished && pcState->conn->ok()) {
                try {
                    // Complete the call if only halfway done
                    bool retry = false;
                    pcState->cursor->initLazyFinish(retry);
                } catch (std::exception&) {
                    warning() << "exception closing cursor";
                } catch (...) {
                    warning() << "unknown exception closing cursor";
                }
            }
        }

        // Double-check conn is closed
        if (pcState->conn) {
            pcState->conn->done();
        }

        pcState.reset();
    } else
        verify(finished || !initialized);

    initialized = false;
    finished = false;
    completed = false;
    errored = false;
}


BSONObj ParallelConnectionState::toBSON() const {
    BSONObj cursorPeek = BSON("no cursor"
                              << "");
    if (cursor) {
        vector<BSONObj> v;
        cursor->peek(v, 1);
        if (v.size() == 0)
            cursorPeek = BSON("no data"
                              << "");
        else
            cursorPeek = BSON("" << v[0]);
    }

    BSONObj stateObj =
        BSON("conn" << (conn ? (conn->ok() ? conn->conn().toString() : "(done)") : "") << "vinfo"
                    << (manager ? (str::stream() << manager->getns() << " @ "
                                                 << manager->getVersion().toString())
                                : primary->toString()));

    // Append cursor data if exists
    BSONObjBuilder stateB;
    stateB.appendElements(stateObj);
    if (!cursor)
        stateB.append("cursor", "(none)");
    else {
        vector<BSONObj> v;
        cursor->peek(v, 1);
        if (v.size() == 0)
            stateB.append("cursor", "(empty)");
        else
            stateB.append("cursor", v[0]);
    }

    stateB.append("count", count);
    stateB.append("done", done);

    return stateB.obj().getOwned();
}

BSONObj ParallelConnectionMetadata::toBSON() const {
    return BSON("state" << (pcState ? pcState->toBSON() : BSONObj()) << "retryNext" << retryNext
                        << "init"
                        << initialized
                        << "finish"
                        << finished
                        << "errored"
                        << errored);
}

void ParallelSortClusteredCursor::fullInit(OperationContext* txn) {
    startInit(txn);
    finishInit(txn);
}

void ParallelSortClusteredCursor::_markStaleNS(const NamespaceString& staleNS,
                                               const StaleConfigException& e,
                                               bool& forceReload,
                                               bool& fullReload) {
    fullReload = e.requiresFullReload();

    if (_staleNSMap.find(staleNS.ns()) == _staleNSMap.end())
        _staleNSMap[staleNS.ns()] = 1;

    int tries = ++_staleNSMap[staleNS.ns()];

    if (tries >= 5) {
        throw SendStaleConfigException(staleNS.ns(),
                                       str::stream() << "too many retries of stale version info",
                                       e.getVersionReceived(),
                                       e.getVersionWanted());
    }

    forceReload = tries > 2;
}

void ParallelSortClusteredCursor::_handleStaleNS(OperationContext* txn,
                                                 const NamespaceString& staleNS,
                                                 bool forceReload,
                                                 bool fullReload) {
    auto status = grid.catalogCache()->getDatabase(txn, staleNS.db().toString());
    if (!status.isOK()) {
        warning() << "cannot reload database info for stale namespace " << staleNS.ns();
        return;
    }

    shared_ptr<DBConfig> config = status.getValue();

    // Reload db if needed, make sure it works
    if (fullReload && !config->reload(txn)) {
        // We didn't find the db after reload, the db may have been dropped, reset this ptr
        config.reset();
    }

    if (!config) {
        warning() << "cannot reload database info for stale namespace " << staleNS.ns();
    } else {
        // Reload chunk manager, potentially forcing the namespace
        config->getChunkManagerIfExists(txn, staleNS.ns(), true, forceReload);
    }
}

void ParallelSortClusteredCursor::setupVersionAndHandleSlaveOk(
    OperationContext* txn,
    PCStatePtr state,
    const ShardId& shardId,
    std::shared_ptr<Shard> primary,
    const NamespaceString& ns,
    const string& vinfo,
    std::shared_ptr<ChunkManager> manager) {
    if (manager) {
        state->manager = manager;
    } else if (primary) {
        state->primary = primary;
    }

    verify(!primary || shardId == primary->getId());

    // Setup conn
    if (!state->conn) {
        const auto shard = grid.shardRegistry()->getShard(txn, shardId);
        state->conn.reset(new ShardConnection(shard->getConnString(), ns.ns(), manager));
    }

    const DBClientBase* rawConn = state->conn->getRawConn();
    bool allowShardVersionFailure = rawConn->type() == ConnectionString::SET &&
        DBClientReplicaSet::isSecondaryQuery(_qSpec.ns(), _qSpec.query(), _qSpec.options());

    // Skip shard version checking if primary is known to be down.
    if (allowShardVersionFailure) {
        const DBClientReplicaSet* replConn = dynamic_cast<const DBClientReplicaSet*>(rawConn);
        invariant(replConn);
        ReplicaSetMonitorPtr rsMonitor = ReplicaSetMonitor::get(replConn->getSetName());
        if (!rsMonitor->isKnownToHaveGoodPrimary()) {
            state->conn->donotCheckVersion();

            // A side effect of this short circuiting is the mongos will not be able figure out
            // that the primary is now up on it's own and has to rely on other threads to refresh
            // node states.

            OCCASIONALLY {
                const DBClientReplicaSet* repl = dynamic_cast<const DBClientReplicaSet*>(rawConn);
                dassert(repl);
                warning() << "Primary for " << repl->getServerAddress()
                          << " was down before, bypassing setShardVersion."
                          << " The local replica set view and targeting may be stale.";
            }

            return;
        }
    }

    try {
        if (state->conn->setVersion()) {
            LOG(pc) << "needed to set remote version on connection to value "
                    << "compatible with " << vinfo;
        }
    } catch (const DBException& dbExcep) {
        auto errCode = dbExcep.getCode();
        if (allowShardVersionFailure &&
            (ErrorCodes::isNotMasterError(ErrorCodes::fromInt(errCode)) ||
             errCode == ErrorCodes::FailedToSatisfyReadPreference ||
             errCode == ErrorCodes::SocketException)) {
            // It's okay if we don't set the version when talking to a secondary, we can
            // be stale in any case.

            OCCASIONALLY {
                const DBClientReplicaSet* repl =
                    dynamic_cast<const DBClientReplicaSet*>(state->conn->getRawConn());
                dassert(repl);
                warning() << "Cannot contact primary for " << repl->getServerAddress()
                          << " to check shard version."
                          << " The local replica set view and targeting may be stale.";
            }
        } else {
            throw;
        }
    }
}

void ParallelSortClusteredCursor::startInit(OperationContext* txn) {
    const bool returnPartial = (_qSpec.options() & QueryOption_PartialResults);
    const NamespaceString nss(!_cInfo.isEmpty() ? _cInfo.versionedNS : _qSpec.ns());

    shared_ptr<ChunkManager> manager;
    shared_ptr<Shard> primary;

    string prefix;
    if (MONGO_unlikely(shouldLog(pc))) {
        if (_totalTries > 0) {
            prefix = str::stream() << "retrying (" << _totalTries << " tries)";
        } else {
            prefix = "creating";
        }
    }
    LOG(pc) << prefix << " pcursor over " << _qSpec << " and " << _cInfo;

    set<ShardId> shardIds;
    string vinfo;

    {
        shared_ptr<DBConfig> config;

        auto status = grid.catalogCache()->getDatabase(txn, nss.db().toString());
        if (status.getStatus().code() != ErrorCodes::NamespaceNotFound) {
            config = uassertStatusOK(status);
            config->getChunkManagerOrPrimary(txn, nss.ns(), manager, primary);
        }
    }

    if (manager) {
        if (MONGO_unlikely(shouldLog(pc))) {
            vinfo = str::stream() << "[" << manager->getns() << " @ "
                                  << manager->getVersion().toString() << "]";
        }

        manager->getShardIdsForQuery(
            txn, !_cInfo.isEmpty() ? _cInfo.cmdFilter : _qSpec.filter(), &shardIds);
    } else if (primary) {
        if (MONGO_unlikely(shouldLog(pc))) {
            vinfo = str::stream() << "[unsharded @ " << primary->toString() << "]";
        }

        shardIds.insert(primary->getId());
    }

    // Close all cursors on extra shards first, as these will be invalid
    for (map<ShardId, PCMData>::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end;
         ++i) {
        if (shardIds.find(i->first) == shardIds.end()) {
            LOG(pc) << "closing cursor on shard " << i->first
                    << " as the connection is no longer required by " << vinfo;

            i->second.cleanup(true);
        }
    }

    LOG(pc) << "initializing over " << shardIds.size() << " shards required by " << vinfo;

    // Don't retry indefinitely for whatever reason
    _totalTries++;
    uassert(15986, "too many retries in total", _totalTries < 10);

    for (const ShardId& shardId : shardIds) {
        PCMData& mdata = _cursorMap[shardId];

        LOG(pc) << "initializing on shard " << shardId << ", current connection state is "
                << mdata.toBSON();

        // This may be the first time connecting to this shard, if so we can get an error here
        try {
            if (mdata.initialized) {
                invariant(mdata.pcState);

                PCStatePtr state = mdata.pcState;

                bool compatiblePrimary = true;
                bool compatibleManager = true;

                if (primary && !state->primary)
                    warning() << "Collection becoming unsharded detected";
                if (manager && !state->manager)
                    warning() << "Collection becoming sharded detected";
                if (primary && state->primary && primary != state->primary)
                    warning() << "Weird shift of primary detected";

                compatiblePrimary = primary && state->primary && primary == state->primary;
                compatibleManager =
                    manager && state->manager && manager->compatibleWith(*state->manager, shardId);

                if (compatiblePrimary || compatibleManager) {
                    // If we're compatible, don't need to retry unless forced
                    if (!mdata.retryNext)
                        continue;
                    // Do partial cleanup
                    mdata.cleanup(false);
                } else {
                    // Force total cleanup of connection if no longer compatible
                    mdata.cleanup(true);
                }
            } else {
                // Cleanup connection if we're not yet initialized
                mdata.cleanup(false);
            }

            mdata.pcState.reset(new PCState());
            PCStatePtr state = mdata.pcState;

            setupVersionAndHandleSlaveOk(txn, state, shardId, primary, nss, vinfo, manager);

            const string& ns = _qSpec.ns();

            // Setup cursor
            if (!state->cursor) {
                //
                // Here we decide whether to split the query limits up for multiple shards.
                // NOTE: There's a subtle issue here, in that it's possible we target a single
                // shard first, but are stale, and then target multiple shards, or vice-versa.
                // In both these cases, we won't re-use the old cursor created here, since the
                // shard version must have changed on the single shard between queries.
                //

                if (shardIds.size() > 1) {
                    // Query limits split for multiple shards

                    state->cursor.reset(new DBClientCursor(
                        state->conn->get(),
                        ns,
                        _qSpec.query(),
                        isCommand() ? 1 : 0,  // nToReturn (0 if query indicates multi)
                        0,                    // nToSkip
                        // Does this need to be a ptr?
                        _qSpec.fields().isEmpty() ? 0 : _qSpec.fieldsData(),  // fieldsToReturn
                        _qSpec.options(),                                     // options
                        // NtoReturn is weird.
                        // If zero, it means use default size, so we do that for all cursors
                        // If positive, it's the batch size (we don't want this cursor limiting
                        // results), that's done at a higher level
                        // If negative, it's the batch size, but we don't create a cursor - so we
                        // don't want to create a child cursor either.
                        // Either way, if non-zero, we want to pull back the batch size + the skip
                        // amount as quickly as possible.  Potentially, for a cursor on a single
                        // shard or if we keep better track of chunks, we can actually add the skip
                        // value into the cursor and/or make some assumptions about the return value
                        // size ( (batch size + skip amount) / num_servers ).
                        _qSpec.ntoreturn() == 0 ? 0 : (_qSpec.ntoreturn() > 0
                                                           ? _qSpec.ntoreturn() + _qSpec.ntoskip()
                                                           : _qSpec.ntoreturn() -
                                                               _qSpec.ntoskip())));  // batchSize
                } else {
                    // Single shard query

                    state->cursor.reset(new DBClientCursor(
                        state->conn->get(),
                        ns,
                        _qSpec.query(),
                        _qSpec.ntoreturn(),  // nToReturn
                        _qSpec.ntoskip(),    // nToSkip
                        // Does this need to be a ptr?
                        _qSpec.fields().isEmpty() ? 0 : _qSpec.fieldsData(),  // fieldsToReturn
                        _qSpec.options(),                                     // options
                        0));                                                  // batchSize
                }
            }

            bool lazyInit = state->conn->get()->lazySupported();
            if (lazyInit) {
                // Need to keep track if this is a second or third try for replica sets
                state->cursor->initLazy(mdata.retryNext);
                mdata.retryNext = false;
                mdata.initialized = true;
            } else {
                bool success = state->cursor->init();

                // Without full initialization, throw an exception
                uassert(15987,
                        str::stream() << "could not fully initialize cursor on shard " << shardId
                                      << ", current connection state is "
                                      << mdata.toBSON().toString(),
                        success);

                mdata.retryNext = false;
                mdata.initialized = true;
                mdata.finished = true;
            }


            LOG(pc) << "initialized " << (isCommand() ? "command " : "query ")
                    << (lazyInit ? "(lazily) " : "(full) ") << "on shard " << shardId
                    << ", current connection state is " << mdata.toBSON();
        } catch (StaleConfigException& e) {
            // Our version isn't compatible with the current version anymore on at least one shard,
            // need to retry immediately
            NamespaceString staleNS(e.getns());

            // For legacy reasons, this may not be set in the exception :-(
            if (staleNS.size() == 0)
                staleNS = nss;  // ns is the *versioned* namespace, be careful of this

            // Probably need to retry fully
            bool forceReload, fullReload;
            _markStaleNS(staleNS, e, forceReload, fullReload);

            int logLevel = fullReload ? 0 : 1;
            LOG(pc + logLevel) << "stale config of ns " << staleNS
                               << " during initialization, will retry with forced : " << forceReload
                               << ", full : " << fullReload << causedBy(e);

            // This is somewhat strange
            if (staleNS != nss)
                warning() << "versioned ns " << nss.ns() << " doesn't match stale config namespace "
                          << staleNS;

            _handleStaleNS(txn, staleNS, forceReload, fullReload);

            // Restart with new chunk manager
            startInit(txn);
            return;
        } catch (SocketException& e) {
            warning() << "socket exception when initializing on " << shardId
                      << ", current connection state is " << mdata.toBSON() << causedBy(e);
            e._shard = shardId.toString();
            mdata.errored = true;
            if (returnPartial) {
                mdata.cleanup(true);
                continue;
            }
            throw;
        } catch (DBException& e) {
            warning() << "db exception when initializing on " << shardId
                      << ", current connection state is " << mdata.toBSON() << causedBy(e);
            e._shard = shardId.toString();
            mdata.errored = true;
            if (returnPartial && e.getCode() == 15925 /* From above! */) {
                mdata.cleanup(true);
                continue;
            }
            throw;
        } catch (std::exception& e) {
            warning() << "exception when initializing on " << shardId
                      << ", current connection state is " << mdata.toBSON() << causedBy(e);
            mdata.errored = true;
            throw;
        } catch (...) {
            warning() << "unknown exception when initializing on " << shardId
                      << ", current connection state is " << mdata.toBSON();
            mdata.errored = true;
            throw;
        }
    }

    // Sanity check final init'ed connections
    for (map<ShardId, PCMData>::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end;
         ++i) {
        const ShardId& shardId = i->first;
        PCMData& mdata = i->second;

        if (!mdata.pcState) {
            continue;
        }

        // Make sure all state is in shards
        invariant(shardIds.find(shardId) != shardIds.end());
        invariant(mdata.initialized == true);

        if (!mdata.completed) {
            invariant(mdata.pcState->conn->ok());
        }

        invariant(mdata.pcState->cursor);
        invariant(mdata.pcState->primary || mdata.pcState->manager);
        invariant(!mdata.retryNext);

        if (mdata.completed) {
            invariant(mdata.finished);
        }

        if (mdata.finished) {
            invariant(mdata.initialized);
        }

        if (!returnPartial) {
            invariant(mdata.initialized);
        }
    }
}

void ParallelSortClusteredCursor::finishInit(OperationContext* txn) {
    bool returnPartial = (_qSpec.options() & QueryOption_PartialResults);
    bool specialVersion = _cInfo.versionedNS.size() > 0;
    string ns = specialVersion ? _cInfo.versionedNS : _qSpec.ns();

    bool retry = false;
    map<string, StaleConfigException> staleNSExceptions;

    LOG(pc) << "finishing over " << _cursorMap.size() << " shards";

    for (map<ShardId, PCMData>::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end;
         ++i) {
        const ShardId& shardId = i->first;
        PCMData& mdata = i->second;

        LOG(pc) << "finishing on shard " << shardId << ", current connection state is "
                << mdata.toBSON();

        // Ignore empty conns for now
        if (!mdata.pcState)
            continue;

        PCStatePtr state = mdata.pcState;

        try {
            // Sanity checks
            if (!mdata.completed)
                verify(state->conn && state->conn->ok());
            verify(state->cursor);
            verify(state->manager || state->primary);
            verify(!state->manager || !state->primary);


            // If we weren't init'ing lazily, ignore this
            if (!mdata.finished) {
                mdata.finished = true;

                // Mark the cursor as non-retry by default
                mdata.retryNext = false;

                if (!state->cursor->initLazyFinish(mdata.retryNext)) {
                    if (!mdata.retryNext) {
                        uassert(15988, "error querying server", false);
                    } else {
                        retry = true;
                        continue;
                    }
                }

                mdata.completed = false;
            }

            if (!mdata.completed) {
                mdata.completed = true;

                // Make sure we didn't get an error we should rethrow
                // TODO : Refactor this to something better
                throwCursorStale(state->cursor.get());
                throwCursorError(state->cursor.get());

                // Finalize state
                state->cursor->attach(state->conn.get());  // Closes connection for us

                LOG(pc) << "finished on shard " << shardId << ", current connection state is "
                        << mdata.toBSON();
            }
        } catch (RecvStaleConfigException& e) {
            retry = true;

            string staleNS = e.getns();
            // For legacy reasons, ns may not always be set in exception :-(
            if (staleNS.size() == 0)
                staleNS = ns;  // ns is versioned namespace, be careful of this

            // Will retry all at once
            staleNSExceptions[staleNS] = e;

            // Fully clear this cursor, as it needs to be re-established
            mdata.cleanup(true);
            continue;
        } catch (SocketException& e) {
            warning() << "socket exception when finishing on " << shardId
                      << ", current connection state is " << mdata.toBSON() << causedBy(e);
            mdata.errored = true;
            if (returnPartial) {
                mdata.cleanup(true);
                continue;
            }
            throw;
        } catch (DBException& e) {
            // NOTE: RECV() WILL NOT THROW A SOCKET EXCEPTION - WE GET THIS AS ERROR 15988 FROM
            // ABOVE
            if (e.getCode() == 15988) {
                warning() << "exception when receiving data from " << shardId
                          << ", current connection state is " << mdata.toBSON() << causedBy(e);

                mdata.errored = true;
                if (returnPartial) {
                    mdata.cleanup(true);
                    continue;
                }
                throw;
            } else {
                // the InvalidBSON exception indicates that the BSON is malformed ->
                // don't print/call "mdata.toBSON()" to avoid unexpected errors e.g. a segfault
                if (e.getCode() == 22)
                    warning() << "bson is malformed :: db exception when finishing on " << shardId
                              << causedBy(e);
                else
                    warning() << "db exception when finishing on " << shardId
                              << ", current connection state is " << mdata.toBSON() << causedBy(e);
                mdata.errored = true;
                throw;
            }
        } catch (std::exception& e) {
            warning() << "exception when finishing on " << shardId
                      << ", current connection state is " << mdata.toBSON() << causedBy(e);
            mdata.errored = true;
            throw;
        } catch (...) {
            warning() << "unknown exception when finishing on " << shardId
                      << ", current connection state is " << mdata.toBSON();
            mdata.errored = true;
            throw;
        }
    }

    // Retry logic for single refresh of namespaces / retry init'ing connections
    if (retry) {
        // Refresh stale namespaces
        if (staleNSExceptions.size()) {
            for (map<string, StaleConfigException>::iterator i = staleNSExceptions.begin(),
                                                             end = staleNSExceptions.end();
                 i != end;
                 ++i) {
                NamespaceString staleNS(i->first);
                const StaleConfigException& exception = i->second;

                bool forceReload, fullReload;
                _markStaleNS(staleNS, exception, forceReload, fullReload);

                int logLevel = fullReload ? 0 : 1;
                LOG(pc + logLevel)
                    << "stale config of ns " << staleNS
                    << " on finishing query, will retry with forced : " << forceReload
                    << ", full : " << fullReload << causedBy(exception);

                // This is somewhat strange
                if (staleNS != ns)
                    warning() << "versioned ns " << ns << " doesn't match stale config namespace "
                              << staleNS;

                _handleStaleNS(txn, staleNS, forceReload, fullReload);
            }
        }

        // Re-establish connections we need to
        startInit(txn);
        finishInit(txn);
        return;
    }

    // Sanity check and clean final connections
    map<ShardId, PCMData>::iterator i = _cursorMap.begin();
    while (i != _cursorMap.end()) {
        PCMData& mdata = i->second;

        // Erase empty stuff
        if (!mdata.pcState) {
            log() << "PCursor erasing empty state " << mdata.toBSON();
            _cursorMap.erase(i++);
            continue;
        } else
            ++i;

        // Make sure all state is in shards
        verify(mdata.initialized == true);
        verify(mdata.finished == true);
        verify(mdata.completed == true);
        verify(!mdata.pcState->conn->ok());
        verify(mdata.pcState->cursor);
        verify(mdata.pcState->primary || mdata.pcState->manager);
    }

    // TODO : More cleanup of metadata?

    // LEGACY STUFF NOW

    _cursors = new DBClientCursorHolder[_cursorMap.size()];

    // Put the cursors in the legacy format
    int index = 0;
    for (map<ShardId, PCMData>::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end;
         ++i) {
        PCMData& mdata = i->second;

        _cursors[index].reset(mdata.pcState->cursor.get(), &mdata);

        {
            const auto shard = grid.shardRegistry()->getShard(txn, i->first);
            _servers.insert(shard->getConnString().toString());
        }

        index++;
    }

    _numServers = _cursorMap.size();
}

void ParallelSortClusteredCursor::getQueryShardIds(set<ShardId>& shardIds) {
    for (map<ShardId, PCMData>::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end;
         ++i) {
        shardIds.insert(i->first);
    }
}

DBClientCursorPtr ParallelSortClusteredCursor::getShardCursor(const ShardId& shardId) {
    map<ShardId, PCMData>::iterator i = _cursorMap.find(shardId);

    if (i == _cursorMap.end())
        return DBClientCursorPtr();
    else
        return i->second.pcState->cursor;
}

// DEPRECATED (but still used by map/reduce)
void ParallelSortClusteredCursor::_oldInit() {
    // make sure we're not already initialized
    verify(!_cursors);
    _cursors = new DBClientCursorHolder[_numServers];

    bool returnPartial = (_options & QueryOption_PartialResults);

    vector<string> serverHosts(_servers.begin(), _servers.end());
    set<int> retryQueries;
    int finishedQueries = 0;

    vector<shared_ptr<ShardConnection>> conns;
    vector<string> servers;

    // Since we may get all sorts of errors, record them all as they come and throw them later if
    // necessary
    vector<string> staleConfigExs;
    vector<string> socketExs;
    vector<string> otherExs;
    bool allConfigStale = false;

    int retries = -1;

    // Loop through all the queries until we've finished or gotten a socket exception on all of them
    // We break early for non-socket exceptions, and socket exceptions if we aren't returning
    // partial results
    do {
        retries++;

        bool firstPass = retryQueries.size() == 0;

        if (!firstPass) {
            log() << "retrying " << (returnPartial ? "(partial) " : "")
                  << "parallel connection to ";
            for (set<int>::const_iterator it = retryQueries.begin(); it != retryQueries.end();
                 ++it) {
                log() << serverHosts[*it] << ", ";
            }
            log() << finishedQueries << " finished queries.";
        }

        size_t num = 0;
        for (vector<string>::const_iterator it = serverHosts.begin(); it != serverHosts.end();
             ++it) {
            size_t i = num++;

            const string& serverHost = *it;

            // If we're not retrying this cursor on later passes, continue
            if (!firstPass && retryQueries.find(i) == retryQueries.end())
                continue;

            const string errLoc = " @ " + serverHost;

            if (firstPass) {
                // This may be the first time connecting to this shard, if so we can get an error
                // here
                try {
                    conns.push_back(shared_ptr<ShardConnection>(new ShardConnection(
                        uassertStatusOK(ConnectionString::parse(serverHost)), _ns)));
                } catch (std::exception& e) {
                    socketExs.push_back(e.what() + errLoc);
                    if (!returnPartial) {
                        num--;
                        break;
                    }

                    conns.push_back(shared_ptr<ShardConnection>());
                    continue;
                }

                servers.push_back(serverHost);
            }

            if (conns[i]->setVersion()) {
                conns[i]->done();

                // Version is zero b/c this is deprecated codepath
                staleConfigExs.push_back(str::stream()
                                         << "stale config detected for "
                                         << RecvStaleConfigException(_ns,
                                                                     "ParallelCursor::_init",
                                                                     ChunkVersion(0, 0, OID()),
                                                                     ChunkVersion(0, 0, OID()))
                                                .what()
                                         << errLoc);
                break;
            }

            LOG(5) << "ParallelSortClusteredCursor::init server:" << serverHost << " ns:" << _ns
                   << " query:" << _query << " fields:" << _fields << " options: " << _options;

            if (!_cursors[i].get())
                _cursors[i].reset(
                    new DBClientCursor(conns[i]->get(),
                                       _ns,
                                       _query,
                                       0,                                 // nToReturn
                                       0,                                 // nToSkip
                                       _fields.isEmpty() ? 0 : &_fields,  // fieldsToReturn
                                       _options,
                                       _batchSize == 0 ? 0 : _batchSize + _needToSkip  // batchSize
                                       ),
                    NULL);

            try {
                _cursors[i].get()->initLazy(!firstPass);
            } catch (SocketException& e) {
                socketExs.push_back(e.what() + errLoc);
                _cursors[i].reset(NULL, NULL);
                conns[i]->done();
                if (!returnPartial)
                    break;
            } catch (std::exception& e) {
                otherExs.push_back(e.what() + errLoc);
                _cursors[i].reset(NULL, NULL);
                conns[i]->done();
                break;
            }
        }

        // Go through all the potentially started cursors and finish initializing them or log any
        // errors and potentially retry
        // TODO:  Better error classification would make this easier, errors are indicated in all
        // sorts of ways here that we need to trap.
        for (size_t i = 0; i < num; i++) {
            const string errLoc = " @ " + serverHosts[i];

            if (!_cursors[i].get() || (!firstPass && retryQueries.find(i) == retryQueries.end())) {
                if (conns[i])
                    conns[i].get()->done();
                continue;
            }

            verify(conns[i]);
            retryQueries.erase(i);

            bool retry = false;

            try {
                if (!_cursors[i].get()->initLazyFinish(retry)) {
                    warning() << "invalid result from " << conns[i]->getHost()
                              << (retry ? ", retrying" : "");
                    _cursors[i].reset(NULL, NULL);

                    if (!retry) {
                        socketExs.push_back(str::stream() << "error querying server: "
                                                          << servers[i]);
                        conns[i]->done();
                    } else {
                        retryQueries.insert(i);
                    }

                    continue;
                }
            } catch (StaleConfigException& e) {
                // Our stored configuration data is actually stale, we need to reload it
                // when we throw our exception
                allConfigStale = true;

                staleConfigExs.push_back(
                    (string) "stale config detected when receiving response for " + e.what() +
                    errLoc);
                _cursors[i].reset(NULL, NULL);
                conns[i]->done();
                continue;
            } catch (SocketException& e) {
                socketExs.push_back(e.what() + errLoc);
                _cursors[i].reset(NULL, NULL);
                conns[i]->done();
                continue;
            } catch (std::exception& e) {
                otherExs.push_back(e.what() + errLoc);
                _cursors[i].reset(NULL, NULL);
                conns[i]->done();
                continue;
            }

            try {
                _cursors[i].get()->attach(conns[i].get());  // this calls done on conn
                // Rethrow stale config or other errors
                throwCursorStale(_cursors[i].get());
                throwCursorError(_cursors[i].get());

                finishedQueries++;
            } catch (StaleConfigException& e) {
                // Our stored configuration data is actually stale, we need to reload it
                // when we throw our exception
                allConfigStale = true;

                staleConfigExs.push_back((string) "stale config detected for " + e.what() + errLoc);
                _cursors[i].reset(NULL, NULL);
                conns[i]->done();
                continue;
            } catch (std::exception& e) {
                otherExs.push_back(e.what() + errLoc);
                _cursors[i].reset(NULL, NULL);
                conns[i]->done();
                continue;
            }
        }

        // Don't exceed our max retries, should not happen
        verify(retries < 5);
    } while (retryQueries.size() > 0 /* something to retry */ &&
             (socketExs.size() == 0 || returnPartial) /* no conn issues */ &&
             staleConfigExs.size() == 0 /* no config issues */ &&
             otherExs.size() == 0 /* no other issues */);

    // Assert that our conns are all closed!
    for (vector<shared_ptr<ShardConnection>>::iterator i = conns.begin(); i < conns.end(); ++i) {
        verify(!(*i) || !(*i)->ok());
    }

    // Handle errors we got during initialization.
    // If we're returning partial results, we can ignore socketExs, but nothing else
    // Log a warning in any case, so we don't lose these messages
    bool throwException = (socketExs.size() > 0 && !returnPartial) || staleConfigExs.size() > 0 ||
        otherExs.size() > 0;

    if (socketExs.size() > 0 || staleConfigExs.size() > 0 || otherExs.size() > 0) {
        vector<string> errMsgs;

        errMsgs.insert(errMsgs.end(), staleConfigExs.begin(), staleConfigExs.end());
        errMsgs.insert(errMsgs.end(), otherExs.begin(), otherExs.end());
        errMsgs.insert(errMsgs.end(), socketExs.begin(), socketExs.end());

        stringstream errMsg;
        errMsg << "could not initialize cursor across all shards because : ";
        for (vector<string>::iterator i = errMsgs.begin(); i != errMsgs.end(); i++) {
            if (i != errMsgs.begin())
                errMsg << " :: and :: ";
            errMsg << *i;
        }

        if (throwException && staleConfigExs.size() > 0) {
            // Version is zero b/c this is deprecated codepath
            throw RecvStaleConfigException(
                _ns, errMsg.str(), ChunkVersion(0, 0, OID()), ChunkVersion(0, 0, OID()));
        } else if (throwException) {
            throw DBException(errMsg.str(), 14827);
        } else {
            warning() << errMsg.str();
        }
    }

    if (retries > 0)
        log() << "successfully finished parallel query after " << retries << " retries";
}

ParallelSortClusteredCursor::~ParallelSortClusteredCursor() {
    // WARNING: Commands (in particular M/R) connect via _oldInit() directly to shards
    bool isDirectShardCursor = _cursorMap.empty();

    // Non-direct shard cursors are owned by the _cursorMap, so we release
    // them in the array here.  Direct shard cursors clean themselves.
    if (!isDirectShardCursor) {
        for (int i = 0; i < _numServers; i++)
            _cursors[i].release();
    }

    delete[] _cursors;
    _cursors = 0;

    // Clear out our metadata after removing legacy cursor data
    _cursorMap.clear();

    // Just to be sure
    _done = true;
}

bool ParallelSortClusteredCursor::more() {
    if (_needToSkip > 0) {
        int n = _needToSkip;
        _needToSkip = 0;

        while (n > 0 && more()) {
            next();
            n--;
        }

        _needToSkip = n;
    }

    for (int i = 0; i < _numServers; i++) {
        if (_cursors[i].get() && _cursors[i].get()->more())
            return true;
    }
    return false;
}

BSONObj ParallelSortClusteredCursor::next() {
    BSONObj best = BSONObj();
    int bestFrom = -1;

    for (int j = 0; j < _numServers; j++) {
        // Iterate _numServers times, starting one past the last server we used.
        // This means we actually start at server #1, not #0, but shouldn't matter

        int i = (j + _lastFrom + 1) % _numServers;

        // Check to see if the cursor is finished
        if (!_cursors[i].get() || !_cursors[i].get()->more()) {
            if (_cursors[i].getMData())
                _cursors[i].getMData()->pcState->done = true;
            continue;
        }

        // We know we have at least one result in this cursor
        BSONObj me = _cursors[i].get()->peekFirst();

        // If this is the first non-empty cursor, save the result as best
        if (bestFrom < 0) {
            best = me;
            bestFrom = i;
            if (_sortKey.isEmpty())
                break;
            continue;
        }

        // Otherwise compare the result to the current best result
        int comp = dps::compareObjectsAccordingToSort(best, me, _sortKey, true);
        if (comp < 0)
            continue;

        best = me;
        bestFrom = i;
    }

    _lastFrom = bestFrom;

    uassert(10019, "no more elements", bestFrom >= 0);
    _cursors[bestFrom].get()->next();

    // Make sure the result data won't go away after the next call to more()
    if (!_cursors[bestFrom].get()->moreInCurrentBatch()) {
        best = best.getOwned();
    }

    if (_cursors[bestFrom].getMData())
        _cursors[bestFrom].getMData()->pcState->count++;

    return best;
}

}  // namespace mongo
