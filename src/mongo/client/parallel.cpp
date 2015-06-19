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
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/version_manager.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::endl;
using std::list;
using std::map;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

LabeledLevel pc("pcursor", 2);

void ParallelSortClusteredCursor::init() {
    if (_didInit)
        return;
    _didInit = true;

    if (!_qSpec.isEmpty()) {
        fullInit();
    } else {
        // You can only get here by using the legacy constructor
        // TODO: Eliminate this
        _oldInit();
    }
}

string ParallelSortClusteredCursor::getNS() {
    if (!_qSpec.isEmpty())
        return _qSpec.ns();
    return _ns;
}

/**
 * Throws a RecvStaleConfigException wrapping the stale error document in this cursor when the
 * ShardConfigStale flag is set or a command returns a SendStaleConfigCode error code.
 */
static void throwCursorStale(DBClientCursor* cursor) {
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
        if (res.hasField("code") && res["code"].Number() == SendStaleConfigCode) {
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

void ParallelSortClusteredCursor::explain(BSONObjBuilder& b) {
    // Note: by default we filter out allPlans and oldPlan in the shell's
    // explain() function. If you add any recursive structures, make sure to
    // edit the JS to make sure everything gets filtered.

    // Return single shard output if we're versioned but not sharded, or
    // if we specified only a single shard
    // TODO:  We should really make this simpler - all queries via mongos
    // *always* get the same explain format
    if (!isSharded()) {
        map<string, list<BSONObj>> out;
        _explain(out);
        verify(out.size() == 1);
        list<BSONObj>& l = out.begin()->second;
        verify(l.size() == 1);
        b.appendElements(*(l.begin()));
        return;
    }

    b.append("clusteredType", type());

    string cursorType;
    BSONObj indexBounds;
    BSONObj oldPlan;

    long long millis = 0;
    double numExplains = 0;

    long long nReturned = 0;
    long long keysExamined = 0;
    long long docsExamined = 0;

    map<string, list<BSONObj>> out;
    {
        _explain(out);

        BSONObjBuilder x(b.subobjStart("shards"));
        for (map<string, list<BSONObj>>::iterator i = out.begin(); i != out.end(); ++i) {
            const ShardId& shardId = i->first;
            list<BSONObj> l = i->second;
            BSONArrayBuilder y(x.subarrayStart(shardId));
            for (list<BSONObj>::iterator j = l.begin(); j != l.end(); ++j) {
                BSONObj temp = *j;

                // If appending the next output from the shard is going to make the BSON
                // too large, then don't add it. We make sure the BSON doesn't get bigger
                // than the allowable "user size" for a BSONObj. This leaves a little bit
                // of extra space which mongos can use to add extra data.
                if ((x.len() + temp.objsize()) > BSONObjMaxUserSize) {
                    y.append(BSON("warning"
                                  << "shard output omitted due to nearing 16 MB limit"));
                    break;
                }

                y.append(temp);

                if (temp.hasField("executionStats")) {
                    // Here we assume that the shard gave us back explain 2.0 style output.
                    BSONObj execStats = temp["executionStats"].Obj();
                    if (execStats.hasField("nReturned")) {
                        nReturned += execStats["nReturned"].numberLong();
                    }
                    if (execStats.hasField("totalKeysExamined")) {
                        keysExamined += execStats["totalKeysExamined"].numberLong();
                    }
                    if (execStats.hasField("totalDocsExamined")) {
                        docsExamined += execStats["totalDocsExamined"].numberLong();
                    }
                    if (execStats.hasField("executionTimeMillis")) {
                        millis += execStats["executionTimeMillis"].numberLong();
                    }
                } else {
                    // Here we assume that the shard gave us back explain 1.0 style output.
                    if (temp.hasField("n")) {
                        nReturned += temp["n"].numberLong();
                    }
                    if (temp.hasField("nscanned")) {
                        keysExamined += temp["nscanned"].numberLong();
                    }
                    if (temp.hasField("nscannedObjects")) {
                        docsExamined += temp["nscannedObjects"].numberLong();
                    }
                    if (temp.hasField("millis")) {
                        millis += temp["millis"].numberLong();
                    }
                    if (String == temp["cursor"].type()) {
                        if (cursorType.empty()) {
                            cursorType = temp["cursor"].String();
                        } else if (cursorType != temp["cursor"].String()) {
                            cursorType = "multiple";
                        }
                    }
                    if (Object == temp["indexBounds"].type()) {
                        indexBounds = temp["indexBounds"].Obj();
                    }
                    if (Object == temp["oldPlan"].type()) {
                        oldPlan = temp["oldPlan"].Obj();
                    }
                }

                numExplains++;
            }
            y.done();
        }
        x.done();
    }

    if (!cursorType.empty()) {
        b.append("cursor", cursorType);
    }

    b.appendNumber("n", nReturned);
    b.appendNumber("nscanned", keysExamined);
    b.appendNumber("nscannedObjects", docsExamined);

    b.appendNumber("millisShardTotal", millis);
    b.append("millisShardAvg",
             numExplains ? static_cast<int>(static_cast<double>(millis) / numExplains) : 0);
    b.append("numQueries", (int)numExplains);
    b.append("numShards", (int)out.size());

    if (out.size() == 1) {
        b.append("indexBounds", indexBounds);
        if (!oldPlan.isEmpty()) {
            // this is to stay in compliance with mongod behavior
            // we should make this cleaner, i.e. {} == nothing
            b.append("oldPlan", oldPlan);
        }
    } else {
        // TODO: this is lame...
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
        if (LiteParsedQuery::isTextScoreMeta(e)) {
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

                if (LiteParsedQuery::isTextScoreMeta(e)) {
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
                    warning() << "exception closing cursor" << endl;
                } catch (...) {
                    warning() << "unknown exception closing cursor" << endl;
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
                        << "init" << initialized << "finish" << finished << "errored" << errored);
}

BSONObj ParallelSortClusteredCursor::toBSON() const {
    BSONObjBuilder b;

    b.append("tries", _totalTries);

    {
        BSONObjBuilder bb;
        for (map<ShardId, PCMData>::const_iterator i = _cursorMap.begin(), end = _cursorMap.end();
             i != end;
             ++i) {
            const auto shard = grid.shardRegistry()->getShard(i->first);
            if (!shard) {
                continue;
            }

            bb.append(shard->toString(), i->second.toBSON());
        }
        b.append("cursors", bb.obj().getOwned());
    }

    {
        BSONObjBuilder bb;
        for (map<string, int>::const_iterator i = _staleNSMap.begin(), end = _staleNSMap.end();
             i != end;
             ++i) {
            bb.append(i->first, i->second);
        }
        b.append("staleTries", bb.obj().getOwned());
    }

    return b.obj().getOwned();
}

string ParallelSortClusteredCursor::toString() const {
    return str::stream() << "PCursor : " << toBSON();
}

void ParallelSortClusteredCursor::fullInit() {
    startInit();
    finishInit();
}

void ParallelSortClusteredCursor::_markStaleNS(const NamespaceString& staleNS,
                                               const StaleConfigException& e,
                                               bool& forceReload,
                                               bool& fullReload) {
    fullReload = e.requiresFullReload();

    if (_staleNSMap.find(staleNS) == _staleNSMap.end())
        _staleNSMap[staleNS] = 1;

    int tries = ++_staleNSMap[staleNS];

    if (tries >= 5) {
        throw SendStaleConfigException(staleNS,
                                       str::stream() << "too many retries of stale version info",
                                       e.getVersionReceived(),
                                       e.getVersionWanted());
    }

    forceReload = tries > 2;
}

void ParallelSortClusteredCursor::_handleStaleNS(const NamespaceString& staleNS,
                                                 bool forceReload,
                                                 bool fullReload) {
    auto status = grid.catalogCache()->getDatabase(staleNS.db().toString());
    if (!status.isOK()) {
        warning() << "cannot reload database info for stale namespace " << staleNS;
        return;
    }

    shared_ptr<DBConfig> config = status.getValue();

    // Reload db if needed, make sure it works
    if (fullReload && !config->reload()) {
        // We didn't find the db after reload, the db may have been dropped, reset this ptr
        config.reset();
    }

    if (!config) {
        warning() << "cannot reload database info for stale namespace " << staleNS;
    } else {
        // Reload chunk manager, potentially forcing the namespace
        config->getChunkManagerIfExists(staleNS, true, forceReload);
    }
}

void ParallelSortClusteredCursor::setupVersionAndHandleSlaveOk(PCStatePtr state,
                                                               const ShardId& shardId,
                                                               std::shared_ptr<Shard> primary,
                                                               const NamespaceString& ns,
                                                               const string& vinfo,
                                                               ChunkManagerPtr manager) {
    if (manager) {
        state->manager = manager;
    } else if (primary) {
        state->primary = primary;
    }

    verify(!primary || shardId == primary->getId());

    // Setup conn
    if (!state->conn) {
        const auto shard = grid.shardRegistry()->getShard(shardId);
        state->conn.reset(new ShardConnection(shard->getConnString(), ns, manager));
    }

    const DBClientBase* rawConn = state->conn->getRawConn();
    bool allowShardVersionFailure = rawConn->type() == ConnectionString::SET &&
        DBClientReplicaSet::isSecondaryQuery(_qSpec.ns(), _qSpec.query(), _qSpec.options());
    bool connIsDown = rawConn->isFailed();
    if (allowShardVersionFailure && !connIsDown) {
        // If the replica set connection believes that it has a valid primary that is up,
        // confirm that the replica set monitor agrees that the suspected primary is indeed up.
        const DBClientReplicaSet* replConn = dynamic_cast<const DBClientReplicaSet*>(rawConn);
        invariant(replConn);
        ReplicaSetMonitorPtr rsMonitor = ReplicaSetMonitor::get(replConn->getSetName());
        if (!rsMonitor->isHostUp(replConn->getSuspectedPrimaryHostAndPort())) {
            connIsDown = true;
        }
    }

    if (allowShardVersionFailure && connIsDown) {
        // If we're doing a secondary-allowed query and the primary is down, don't attempt to
        // set the shard version.

        state->conn->donotCheckVersion();

        // A side effect of this short circuiting is the mongos will not be able figure out that
        // the primary is now up on it's own and has to rely on other threads to refresh node
        // states.

        OCCASIONALLY {
            const DBClientReplicaSet* repl = dynamic_cast<const DBClientReplicaSet*>(rawConn);
            dassert(repl);
            warning() << "Primary for " << repl->getServerAddress()
                      << " was down before, bypassing setShardVersion."
                      << " The local replica set view and targeting may be stale." << endl;
        }
    } else {
        try {
            if (state->conn->setVersion()) {
                // It's actually okay if we set the version here, since either the
                // manager will be verified as compatible, or if the manager doesn't
                // exist, we don't care about version consistency
                LOG(pc) << "needed to set remote version on connection to value "
                        << "compatible with " << vinfo << endl;
            }
        } catch (const DBException&) {
            if (allowShardVersionFailure) {
                // It's okay if we don't set the version when talking to a secondary, we can
                // be stale in any case.

                OCCASIONALLY {
                    const DBClientReplicaSet* repl =
                        dynamic_cast<const DBClientReplicaSet*>(state->conn->getRawConn());
                    dassert(repl);
                    warning() << "Cannot contact primary for " << repl->getServerAddress()
                              << " to check shard version."
                              << " The local replica set view and targeting may be stale." << endl;
                }
            } else {
                throw;
            }
        }
    }
}

void ParallelSortClusteredCursor::startInit() {
    const bool returnPartial = (_qSpec.options() & QueryOption_PartialResults);
    const NamespaceString ns(!_cInfo.isEmpty() ? _cInfo.versionedNS : _qSpec.ns());

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
    LOG(pc) << prefix << " pcursor over " << _qSpec << " and " << _cInfo << endl;

    set<ShardId> shardIds;
    string vinfo;

    {
        shared_ptr<DBConfig> config;

        auto status = grid.catalogCache()->getDatabase(ns.db().toString());
        if (status.isOK()) {
            config = status.getValue();
            config->getChunkManagerOrPrimary(ns, manager, primary);
        }
    }

    if (manager) {
        if (MONGO_unlikely(shouldLog(pc))) {
            vinfo = str::stream() << "[" << manager->getns() << " @ "
                                  << manager->getVersion().toString() << "]";
        }

        manager->getShardIdsForQuery(shardIds,
                                     !_cInfo.isEmpty() ? _cInfo.cmdFilter : _qSpec.filter());
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
                    << " as the connection is no longer required by " << vinfo << endl;

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
                << mdata.toBSON() << endl;

        // This may be the first time connecting to this shard, if so we can get an error here
        try {
            if (mdata.initialized) {
                invariant(mdata.pcState);

                PCStatePtr state = mdata.pcState;

                bool compatiblePrimary = true;
                bool compatibleManager = true;

                if (primary && !state->primary)
                    warning() << "Collection becoming unsharded detected" << endl;
                if (manager && !state->manager)
                    warning() << "Collection becoming sharded detected" << endl;
                if (primary && state->primary && primary != state->primary)
                    warning() << "Weird shift of primary detected" << endl;

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

            setupVersionAndHandleSlaveOk(state, shardId, primary, ns, vinfo, manager);

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
                bool success = false;

                if (nsGetCollection(ns) == "$cmd") {
                    /* TODO: remove this when config servers don't use
                     * SyncClusterConnection anymore. This is needed
                     * because SyncConn doesn't allow the call() method
                     * to be used for commands.
                     */
                    success = state->cursor->initCommand();
                } else {
                    success = state->cursor->init();
                }

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
                    << ", current connection state is " << mdata.toBSON() << endl;
        } catch (StaleConfigException& e) {
            // Our version isn't compatible with the current version anymore on at least one shard,
            // need to retry immediately
            NamespaceString staleNS(e.getns());

            // For legacy reasons, this may not be set in the exception :-(
            if (staleNS.size() == 0)
                staleNS = ns;  // ns is the *versioned* namespace, be careful of this

            // Probably need to retry fully
            bool forceReload, fullReload;
            _markStaleNS(staleNS, e, forceReload, fullReload);

            int logLevel = fullReload ? 0 : 1;
            LOG(pc + logLevel) << "stale config of ns " << staleNS
                               << " during initialization, will retry with forced : " << forceReload
                               << ", full : " << fullReload << causedBy(e) << endl;

            // This is somewhat strange
            if (staleNS != ns)
                warning() << "versioned ns " << ns << " doesn't match stale config namespace "
                          << staleNS << endl;

            _handleStaleNS(staleNS, forceReload, fullReload);

            // Restart with new chunk manager
            startInit();
            return;
        } catch (SocketException& e) {
            warning() << "socket exception when initializing on " << shardId
                      << ", current connection state is " << mdata.toBSON() << causedBy(e);
            e._shard = shardId;
            mdata.errored = true;
            if (returnPartial) {
                mdata.cleanup(true);
                continue;
            }
            throw;
        } catch (DBException& e) {
            warning() << "db exception when initializing on " << shardId
                      << ", current connection state is " << mdata.toBSON() << causedBy(e);
            e._shard = shardId;
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

void ParallelSortClusteredCursor::finishInit() {
    bool returnPartial = (_qSpec.options() & QueryOption_PartialResults);
    bool specialVersion = _cInfo.versionedNS.size() > 0;
    string ns = specialVersion ? _cInfo.versionedNS : _qSpec.ns();

    bool retry = false;
    map<string, StaleConfigException> staleNSExceptions;

    LOG(pc) << "finishing over " << _cursorMap.size() << " shards" << endl;

    for (map<ShardId, PCMData>::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end;
         ++i) {
        const ShardId& shardId = i->first;
        PCMData& mdata = i->second;

        LOG(pc) << "finishing on shard " << shardId << ", current connection state is "
                << mdata.toBSON() << endl;

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
                        << mdata.toBSON() << endl;
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
                    << ", full : " << fullReload << causedBy(exception) << endl;

                // This is somewhat strange
                if (staleNS != ns)
                    warning() << "versioned ns " << ns << " doesn't match stale config namespace "
                              << staleNS << endl;

                _handleStaleNS(staleNS, forceReload, fullReload);
            }
        }

        // Re-establish connections we need to
        startInit();
        finishInit();
        return;
    }

    // Sanity check and clean final connections
    map<ShardId, PCMData>::iterator i = _cursorMap.begin();
    while (i != _cursorMap.end()) {
        PCMData& mdata = i->second;

        // Erase empty stuff
        if (!mdata.pcState) {
            log() << "PCursor erasing empty state " << mdata.toBSON() << endl;
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
            const auto shard = grid.shardRegistry()->getShard(i->first);
            _servers.insert(shard->getConnString().toString());
        }

        index++;
    }

    _numServers = _cursorMap.size();
}

bool ParallelSortClusteredCursor::isSharded() {
    // LEGACY is always unsharded
    if (_qSpec.isEmpty())
        return false;
    // We're always sharded if the number of cursors != 1
    // TODO: Kept this way for compatibility with getPrimary(), but revisit
    if (_cursorMap.size() != 1)
        return true;
    // Return if the single cursor is sharded
    return NULL != _cursorMap.begin()->second.pcState->manager;
}

int ParallelSortClusteredCursor::getNumQueryShards() {
    return _cursorMap.size();
}

std::shared_ptr<Shard> ParallelSortClusteredCursor::getQueryShard() {
    return grid.shardRegistry()->getShard(_cursorMap.begin()->first);
}

void ParallelSortClusteredCursor::getQueryShardIds(set<ShardId>& shardIds) {
    for (map<ShardId, PCMData>::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end;
         ++i) {
        shardIds.insert(i->first);
    }
}

std::shared_ptr<Shard> ParallelSortClusteredCursor::getPrimary() {
    if (isSharded())
        return std::shared_ptr<Shard>();
    return _cursorMap.begin()->second.pcState->primary;
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
            log() << finishedQueries << " finished queries." << endl;
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
                staleConfigExs.push_back(
                    str::stream() << "stale config detected for "
                                  << RecvStaleConfigException(_ns,
                                                              "ParallelCursor::_init",
                                                              ChunkVersion(0, 0, OID()),
                                                              ChunkVersion(0, 0, OID())).what()
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
                              << (retry ? ", retrying" : "") << endl;
                    _cursors[i].reset(NULL, NULL);

                    if (!retry) {
                        socketExs.push_back(str::stream()
                                            << "error querying server: " << servers[i]);
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
            warning() << errMsg.str() << endl;
        }
    }

    if (retries > 0)
        log() << "successfully finished parallel query after " << retries << " retries" << endl;
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

void ParallelSortClusteredCursor::setBatchSize(int newBatchSize) {
    for (int i = 0; i < _numServers; i++) {
        if (_cursors[i].get())
            _cursors[i].get()->setBatchSize(newBatchSize);
    }
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
        int comp = best.woSortOrder(me, _sortKey, true);
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

void ParallelSortClusteredCursor::_explain(map<string, list<BSONObj>>& out) {
    set<ShardId> shardIds;
    getQueryShardIds(shardIds);

    for (const ShardId& shardId : shardIds) {
        list<BSONObj>& l = out[shardId];
        l.push_back(getShardCursor(shardId)->peekFirst().getOwned());
    }
}

// -----------------
// ---- Future -----
// -----------------

Future::CommandResult::CommandResult(const string& server,
                                     const string& db,
                                     const BSONObj& cmd,
                                     int options,
                                     DBClientBase* conn,
                                     bool useShardedConn)
    : _server(server),
      _db(db),
      _options(options),
      _cmd(cmd),
      _conn(conn),
      _useShardConn(useShardedConn),
      _done(false) {
    init();
}

void Future::CommandResult::init() {
    try {
        if (!_conn) {
            if (_useShardConn) {
                _connHolder.reset(new ShardConnection(
                    uassertStatusOK(ConnectionString::parse(_server)), "", NULL));
            } else {
                _connHolder.reset(new ScopedDbConnection(_server));
            }

            _conn = _connHolder->get();
        }

        if (_conn->lazySupported()) {
            _cursor.reset(
                new DBClientCursor(_conn, _db + ".$cmd", _cmd, -1 /*limit*/, 0, NULL, _options, 0));
            _cursor->initLazy();
        } else {
            _done = true;  // we set _done first because even if there is an error we're done
            _ok = _conn->runCommand(_db, _cmd, _res, _options);
        }
    } catch (std::exception& e) {
        error() << "Future::spawnCommand (part 1) exception: " << e.what() << endl;
        _ok = false;
        _done = true;
    }
}

bool Future::CommandResult::join(int maxRetries) {
    if (_done)
        return _ok;


    _ok = false;
    for (int i = 1; i <= maxRetries; i++) {
        try {
            bool retry = false;
            bool finished = _cursor->initLazyFinish(retry);

            // Shouldn't need to communicate with server any more
            if (_connHolder)
                _connHolder->done();

            uassert(
                14812, str::stream() << "Error running command on server: " << _server, finished);
            massert(14813, "Command returned nothing", _cursor->more());

            // Rethrow stale config errors stored in this cursor for correct handling
            throwCursorStale(_cursor.get());

            _res = _cursor->nextSafe();
            _ok = _res["ok"].trueValue();

            break;
        } catch (RecvStaleConfigException& e) {
            verify(versionManager.isVersionableCB(_conn));

            // For legacy reasons, we may not always have a namespace :-(
            string staleNS = e.getns();
            if (staleNS.size() == 0)
                staleNS = _db;

            if (i >= maxRetries) {
                error() << "Future::spawnCommand (part 2) stale config exception" << causedBy(e)
                        << endl;
                throw e;
            }

            if (i >= maxRetries / 2) {
                if (!versionManager.forceRemoteCheckShardVersionCB(staleNS)) {
                    error() << "Future::spawnCommand (part 2) no config detected" << causedBy(e)
                            << endl;
                    throw e;
                }
            }

            // We may not always have a collection, since we don't know from a generic command what
            // collection is supposed to be acted on, if any
            if (nsGetCollection(staleNS).size() == 0) {
                warning() << "no collection namespace in stale config exception "
                          << "for lazy command " << _cmd << ", could not refresh " << staleNS
                          << endl;
            } else {
                versionManager.checkShardVersionCB(_conn, staleNS, false, 1);
            }

            LOG(i > 1 ? 0 : 1) << "retrying lazy command" << causedBy(e) << endl;

            verify(_conn->lazySupported());
            _done = false;
            init();
            continue;
        } catch (std::exception& e) {
            error() << "Future::spawnCommand (part 2) exception: " << causedBy(e) << endl;
            break;
        }
    }

    _done = true;
    return _ok;
}

shared_ptr<Future::CommandResult> Future::spawnCommand(const string& server,
                                                       const string& db,
                                                       const BSONObj& cmd,
                                                       int options,
                                                       DBClientBase* conn,
                                                       bool useShardConn) {
    shared_ptr<Future::CommandResult> res(
        new Future::CommandResult(server, db, cmd, options, conn, useShardConn));
    return res;
}
}
