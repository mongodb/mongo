/**
 *    Copyright (C) 2012 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands/mr.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/parallel.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/instance.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/range_preserver.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/stale_exception.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::endl;
using std::set;
using std::shared_ptr;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

namespace dps = ::mongo::dotted_path_support;

namespace mr {

AtomicUInt32 Config::JOB_NUMBER;

JSFunction::JSFunction(const std::string& type, const BSONElement& e) {
    _type = type;
    _code = e._asCode();

    if (e.type() == CodeWScope)
        _wantedScope = e.codeWScopeObject();
}

void JSFunction::init(State* state) {
    _scope = state->scope();
    verify(_scope);
    _scope->init(&_wantedScope);

    _func = _scope->createFunction(_code.c_str());
    uassert(13598, str::stream() << "couldn't compile code for: " << _type, _func);

    // install in JS scope so that it can be called in JS mode
    _scope->setFunction(_type.c_str(), _code.c_str());
}

void JSMapper::init(State* state) {
    _func.init(state);
    _params = state->config().mapParams;
}

/**
 * Applies the map function to an object, which should internally call emit()
 */
void JSMapper::map(const BSONObj& o) {
    Scope* s = _func.scope();
    verify(s);
    if (s->invoke(_func.func(), &_params, &o, 0, true))
        uasserted(9014, str::stream() << "map invoke failed: " << s->getError());
}

/**
 * Applies the finalize function to a tuple obj (key, val)
 * Returns tuple obj {_id: key, value: newval}
 */
BSONObj JSFinalizer::finalize(const BSONObj& o) {
    Scope* s = _func.scope();

    Scope::NoDBAccess no = s->disableDBAccess("can't access db inside finalize");
    s->invokeSafe(_func.func(), &o, 0);

    // don't want to use o.objsize() to size b
    // since there are many cases where the point of finalize
    // is converting many fields to 1
    BSONObjBuilder b;
    b.append(o.firstElement());
    s->append(b, "value", "__returnValue");
    return b.obj();
}

void JSReducer::init(State* state) {
    _func.init(state);
}

/**
 * Reduces a list of tuple objects (key, value) to a single tuple {"0": key, "1": value}
 */
BSONObj JSReducer::reduce(const BSONList& tuples) {
    if (tuples.size() <= 1)
        return tuples[0];
    BSONObj key;
    int endSizeEstimate = 16;
    _reduce(tuples, key, endSizeEstimate);

    BSONObjBuilder b(endSizeEstimate);
    b.appendAs(key.firstElement(), "0");
    _func.scope()->append(b, "1", "__returnValue");
    return b.obj();
}

/**
 * Reduces a list of tuple object (key, value) to a single tuple {_id: key, value: val}
 * Also applies a finalizer method if present.
 */
BSONObj JSReducer::finalReduce(const BSONList& tuples, Finalizer* finalizer) {
    BSONObj res;
    BSONObj key;

    if (tuples.size() == 1) {
        // 1 obj, just use it
        key = tuples[0];
        BSONObjBuilder b(key.objsize());
        BSONObjIterator it(key);
        b.appendAs(it.next(), "_id");
        b.appendAs(it.next(), "value");
        res = b.obj();
    } else {
        // need to reduce
        int endSizeEstimate = 16;
        _reduce(tuples, key, endSizeEstimate);
        BSONObjBuilder b(endSizeEstimate);
        b.appendAs(key.firstElement(), "_id");
        _func.scope()->append(b, "value", "__returnValue");
        res = b.obj();
    }

    if (finalizer) {
        res = finalizer->finalize(res);
    }

    return res;
}

/**
 * actually applies a reduce, to a list of tuples (key, value).
 * After the call, tuples will hold a single tuple {"0": key, "1": value}
 */
void JSReducer::_reduce(const BSONList& tuples, BSONObj& key, int& endSizeEstimate) {
    uassert(10074, "need values", tuples.size());

    int sizeEstimate = (tuples.size() * tuples.begin()->getField("value").size()) + 128;

    // need to build the reduce args: ( key, [values] )
    BSONObjBuilder reduceArgs(sizeEstimate);
    std::unique_ptr<BSONArrayBuilder> valueBuilder;
    unsigned n = 0;
    for (; n < tuples.size(); n++) {
        BSONObjIterator j(tuples[n]);
        BSONElement keyE = j.next();
        if (n == 0) {
            reduceArgs.append(keyE);
            key = keyE.wrap();
            valueBuilder.reset(new BSONArrayBuilder(reduceArgs.subarrayStart("tuples")));
        }

        BSONElement ee = j.next();

        uassert(13070, "value too large to reduce", ee.size() < (BSONObjMaxUserSize / 2));

        // If adding this element to the array would cause it to be too large, break. The
        // remainder of the tuples will be processed recursively at the end of this
        // function.
        if (valueBuilder->len() + ee.size() > BSONObjMaxUserSize) {
            verify(n > 1);  // if not, inf. loop
            break;
        }

        valueBuilder->append(ee);
    }
    verify(valueBuilder);
    valueBuilder->done();
    BSONObj args = reduceArgs.obj();

    Scope* s = _func.scope();

    s->invokeSafe(_func.func(), &args, 0);
    ++numReduces;

    if (s->type("__returnValue") == Array) {
        uasserted(10075, "reduce -> multiple not supported yet");
        return;
    }

    endSizeEstimate = key.objsize() + (args.objsize() / tuples.size());

    if (n == tuples.size())
        return;

    // the input list was too large, add the rest of elmts to new tuples and reduce again
    // note: would be better to use loop instead of recursion to avoid stack overflow
    BSONList x;
    for (; n < tuples.size(); n++) {
        x.push_back(tuples[n]);
    }
    BSONObjBuilder temp(endSizeEstimate);
    temp.append(key.firstElement());
    s->append(temp, "1", "__returnValue");
    x.push_back(temp.obj());
    _reduce(x, key, endSizeEstimate);
}

Config::Config(const string& _dbname, const BSONObj& cmdObj) {
    dbname = _dbname;
    ns = dbname + "." + cmdObj.firstElement().valuestrsafe();

    verbose = cmdObj["verbose"].trueValue();
    jsMode = cmdObj["jsMode"].trueValue();
    splitInfo = 0;

    if (cmdObj.hasField("splitInfo")) {
        splitInfo = cmdObj["splitInfo"].Int();
    }

    jsMaxKeys = 500000;
    reduceTriggerRatio = 10.0;
    maxInMemSize = 500 * 1024;

    uassert(13602, "outType is no longer a valid option", cmdObj["outType"].eoo());

    outputOptions = parseOutputOptions(dbname, cmdObj);

    shardedFirstPass = false;
    if (cmdObj.hasField("shardedFirstPass") && cmdObj["shardedFirstPass"].trueValue()) {
        massert(16054,
                "shardedFirstPass should only use replace outType",
                outputOptions.outType == REPLACE);
        shardedFirstPass = true;
    }

    if (outputOptions.outType != INMEMORY) {  // setup temp collection name
        tempNamespace = str::stream()
            << (outputOptions.outDB.empty() ? dbname : outputOptions.outDB) << ".tmp.mr."
            << cmdObj.firstElement().String() << "_" << JOB_NUMBER.fetchAndAdd(1);
        incLong = tempNamespace + "_inc";
    }

    {
        // scope and code

        if (cmdObj["scope"].type() == Object)
            scopeSetup = cmdObj["scope"].embeddedObjectUserCheck();

        mapper.reset(new JSMapper(cmdObj["map"]));
        reducer.reset(new JSReducer(cmdObj["reduce"]));
        if (cmdObj["finalize"].type() && cmdObj["finalize"].trueValue())
            finalizer.reset(new JSFinalizer(cmdObj["finalize"]));

        if (cmdObj["mapparams"].type() == Array) {
            mapParams = cmdObj["mapparams"].embeddedObjectUserCheck();
        }
    }

    {
        // query options
        BSONElement q = cmdObj["query"];
        if (q.type() == Object)
            filter = q.embeddedObjectUserCheck();
        else
            uassert(13608, "query has to be blank or an Object", !q.trueValue());


        BSONElement s = cmdObj["sort"];
        if (s.type() == Object)
            sort = s.embeddedObjectUserCheck();
        else
            uassert(13609, "sort has to be blank or an Object", !s.trueValue());

        BSONElement collationElt = cmdObj["collation"];
        if (collationElt.type() == Object)
            collation = collationElt.embeddedObjectUserCheck();
        else
            uassert(40082,
                    str::stream()
                        << "mapReduce 'collation' parameter must be of type Object but found type: "
                        << typeName(collationElt.type()),
                    collationElt.eoo());

        if (cmdObj["limit"].isNumber())
            limit = cmdObj["limit"].numberLong();
        else
            limit = 0;
    }
}

/**
 * Clean up the temporary and incremental collections
 */
void State::dropTempCollections() {
    _db.dropCollection(_config.tempNamespace);
    // Always forget about temporary namespaces, so we don't cache lots of them
    ShardConnection::forgetNS(_config.tempNamespace);
    if (_useIncremental) {
        // We don't want to log the deletion of incLong as it isn't replicated. While
        // harmless, this would lead to a scary looking warning on the secondaries.
        bool shouldReplicateWrites = _txn->writesAreReplicated();
        _txn->setReplicatedWrites(false);
        ON_BLOCK_EXIT(&OperationContext::setReplicatedWrites, _txn, shouldReplicateWrites);

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            ScopedTransaction scopedXact(_txn, MODE_IX);
            Lock::DBLock lk(_txn->lockState(), nsToDatabaseSubstring(_config.incLong), MODE_X);
            if (Database* db = dbHolder().get(_txn, _config.incLong)) {
                WriteUnitOfWork wunit(_txn);
                db->dropCollection(_txn, _config.incLong);
                wunit.commit();
            }
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(_txn, "M/R dropTempCollections", _config.incLong)

        ShardConnection::forgetNS(_config.incLong);
    }
}

/**
 * Create temporary collection, set up indexes
 */
void State::prepTempCollection() {
    if (!_onDisk)
        return;

    dropTempCollections();
    if (_useIncremental) {
        // Create the inc collection and make sure we have index on "0" key.
        // Intentionally not replicating the inc collection to secondaries.
        bool shouldReplicateWrites = _txn->writesAreReplicated();
        _txn->setReplicatedWrites(false);
        ON_BLOCK_EXIT(&OperationContext::setReplicatedWrites, _txn, shouldReplicateWrites);

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            OldClientWriteContext incCtx(_txn, _config.incLong);
            WriteUnitOfWork wuow(_txn);
            Collection* incColl = incCtx.getCollection();
            invariant(!incColl);

            CollectionOptions options;
            options.setNoIdIndex();
            options.temp = true;
            incColl = incCtx.db()->createCollection(_txn, _config.incLong, options);
            invariant(incColl);

            BSONObj indexSpec = BSON("key" << BSON("0" << 1) << "ns" << _config.incLong << "name"
                                           << "_temp_0");
            Status status =
                incColl->getIndexCatalog()->createIndexOnEmptyCollection(_txn, indexSpec);
            if (!status.isOK()) {
                uasserted(17305,
                          str::stream() << "createIndex failed for mr incLong ns: "
                                        << _config.incLong
                                        << " err: "
                                        << status.code());
            }
            wuow.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(_txn, "M/R prepTempCollection", _config.incLong);
    }

    CollectionOptions finalOptions;
    vector<BSONObj> indexesToInsert;

    {
        // copy indexes and collection options into temporary storage
        OldClientWriteContext finalCtx(_txn, _config.outputOptions.finalNamespace);
        Collection* const finalColl = finalCtx.getCollection();
        if (finalColl) {
            finalOptions = finalColl->getCatalogEntry()->getCollectionOptions(_txn);

            IndexCatalog::IndexIterator ii =
                finalColl->getIndexCatalog()->getIndexIterator(_txn, true);
            // Iterate over finalColl's indexes.
            while (ii.more()) {
                IndexDescriptor* currIndex = ii.next();
                BSONObjBuilder b;
                b.append("ns", _config.tempNamespace);

                // Copy over contents of the index descriptor's infoObj.
                BSONObjIterator j(currIndex->infoObj());
                while (j.more()) {
                    BSONElement e = j.next();
                    if (str::equals(e.fieldName(), "_id") || str::equals(e.fieldName(), "ns"))
                        continue;
                    b.append(e);
                }
                indexesToInsert.push_back(b.obj());
            }
        }
    }

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        // create temp collection and insert the indexes from temporary storage
        OldClientWriteContext tempCtx(_txn, _config.tempNamespace);
        WriteUnitOfWork wuow(_txn);
        NamespaceString tempNss(_config.tempNamespace);
        uassert(ErrorCodes::NotMaster,
                "no longer master",
                repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(tempNss));
        Collection* tempColl = tempCtx.getCollection();
        invariant(!tempColl);

        CollectionOptions options = finalOptions;
        options.temp = true;
        tempColl = tempCtx.db()->createCollection(_txn, _config.tempNamespace, options);

        for (vector<BSONObj>::iterator it = indexesToInsert.begin(); it != indexesToInsert.end();
             ++it) {
            Status status = tempColl->getIndexCatalog()->createIndexOnEmptyCollection(_txn, *it);
            if (!status.isOK()) {
                if (status.code() == ErrorCodes::IndexAlreadyExists) {
                    continue;
                }
                uassertStatusOK(status);
            }
            // Log the createIndex operation.
            string logNs = nsToDatabase(_config.tempNamespace) + ".system.indexes";
            getGlobalServiceContext()->getOpObserver()->onCreateIndex(_txn, logNs, *it);
        }
        wuow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(_txn, "M/R prepTempCollection", _config.tempNamespace)
}

/**
 * For inline mode, appends results to output object.
 * Makes sure (key, value) tuple is formatted as {_id: key, value: val}
 */
void State::appendResults(BSONObjBuilder& final) {
    if (_onDisk) {
        if (!_config.outputOptions.outDB.empty()) {
            BSONObjBuilder loc;
            if (!_config.outputOptions.outDB.empty())
                loc.append("db", _config.outputOptions.outDB);
            if (!_config.outputOptions.collectionName.empty())
                loc.append("collection", _config.outputOptions.collectionName);
            final.append("result", loc.obj());
        } else {
            if (!_config.outputOptions.collectionName.empty())
                final.append("result", _config.outputOptions.collectionName);
        }

        if (_config.splitInfo > 0) {
            // add split points, used for shard
            BSONObj res;
            BSONObj idKey = BSON("_id" << 1);
            if (!_db.runCommand("admin",
                                BSON("splitVector" << _config.outputOptions.finalNamespace
                                                   << "keyPattern"
                                                   << idKey
                                                   << "maxChunkSizeBytes"
                                                   << _config.splitInfo),
                                res)) {
                uasserted(15921, str::stream() << "splitVector failed: " << res);
            }
            if (res.hasField("splitKeys"))
                final.append(res.getField("splitKeys"));
        }
        return;
    }

    if (_jsMode) {
        ScriptingFunction getResult = _scope->createFunction(
            "var map = _mrMap;"
            "var result = [];"
            "for (key in map) {"
            "  result.push({_id: key, value: map[key]});"
            "}"
            "return result;");
        _scope->invoke(getResult, 0, 0, 0, false);
        BSONObj obj = _scope->getObject("__returnValue");
        final.append("results", BSONArray(obj));
        return;
    }

    uassert(13604, "too much data for in memory map/reduce", _size < BSONObjMaxUserSize);

    BSONArrayBuilder b((int)(_size * 1.2));  // _size is data size, doesn't count overhead and keys

    for (const auto& entry : *_temp) {
        const BSONObj& key = entry.first;
        const BSONList& all = entry.second;

        verify(all.size() == 1);

        BSONObjIterator vi(all[0]);
        vi.next();

        BSONObjBuilder temp(b.subobjStart());
        temp.appendAs(key.firstElement(), "_id");
        temp.appendAs(vi.next(), "value");
        temp.done();
    }

    BSONArray res = b.arr();
    final.append("results", res);
}

/**
 * Does post processing on output collection.
 * This may involve replacing, merging or reducing.
 */
long long State::postProcessCollection(OperationContext* txn,
                                       CurOp* curOp,
                                       ProgressMeterHolder& pm) {
    if (_onDisk == false || _config.outputOptions.outType == Config::INMEMORY)
        return numInMemKeys();

    if (_config.outputOptions.outNonAtomic)
        return postProcessCollectionNonAtomic(txn, curOp, pm);

    invariant(!txn->lockState()->isLocked());

    ScopedTransaction transaction(txn, MODE_X);
    Lock::GlobalWrite lock(
        txn->lockState());  // TODO(erh): this is how it was, but seems it doesn't need to be global
    return postProcessCollectionNonAtomic(txn, curOp, pm);
}

//
// For SERVER-6116 - can't handle version errors in count currently
//

/**
 * Runs count and disables version errors.
 *
 * TODO: make count work with versioning
 */
unsigned long long _safeCount(OperationContext* txn,
                              // Can't be const b/c count isn't
                              /* const */ DBDirectClient& db,
                              const string& ns,
                              const BSONObj& query = BSONObj(),
                              int options = 0,
                              int limit = 0,
                              int skip = 0) {
    OperationShardingState::IgnoreVersioningBlock ignoreVersion(txn, NamespaceString(ns));
    return db.count(ns, query, options, limit, skip);
}

//
// End SERVER-6116
//

long long State::postProcessCollectionNonAtomic(OperationContext* txn,
                                                CurOp* curOp,
                                                ProgressMeterHolder& pm) {
    if (_config.outputOptions.finalNamespace == _config.tempNamespace)
        return _safeCount(txn, _db, _config.outputOptions.finalNamespace);

    if (_config.outputOptions.outType == Config::REPLACE ||
        _safeCount(txn, _db, _config.outputOptions.finalNamespace) == 0) {
        ScopedTransaction transaction(txn, MODE_X);
        Lock::GlobalWrite lock(txn->lockState());  // TODO(erh): why global???
        // replace: just rename from temp to final collection name, dropping previous collection
        _db.dropCollection(_config.outputOptions.finalNamespace);
        BSONObj info;

        if (!_db.runCommand("admin",
                            BSON("renameCollection" << _config.tempNamespace << "to"
                                                    << _config.outputOptions.finalNamespace
                                                    << "stayTemp"
                                                    << _config.shardedFirstPass),
                            info)) {
            uasserted(10076, str::stream() << "rename failed: " << info);
        }

        _db.dropCollection(_config.tempNamespace);
    } else if (_config.outputOptions.outType == Config::MERGE) {
        // merge: upsert new docs into old collection
        {
            const auto count = _safeCount(txn, _db, _config.tempNamespace, BSONObj());
            stdx::lock_guard<Client> lk(*txn->getClient());
            curOp->setMessage_inlock(
                "m/r: merge post processing", "M/R Merge Post Processing Progress", count);
        }
        unique_ptr<DBClientCursor> cursor = _db.query(_config.tempNamespace, BSONObj());
        while (cursor->more()) {
            ScopedTransaction scopedXact(_txn, MODE_IX);
            Lock::DBLock lock(_txn->lockState(),
                              nsToDatabaseSubstring(_config.outputOptions.finalNamespace),
                              MODE_X);
            BSONObj o = cursor->nextSafe();
            Helpers::upsert(_txn, _config.outputOptions.finalNamespace, o);
            pm.hit();
        }
        _db.dropCollection(_config.tempNamespace);
        pm.finished();
    } else if (_config.outputOptions.outType == Config::REDUCE) {
        // reduce: apply reduce op on new result and existing one
        BSONList values;

        {
            const auto count = _safeCount(txn, _db, _config.tempNamespace, BSONObj());
            stdx::lock_guard<Client> lk(*txn->getClient());
            curOp->setMessage_inlock(
                "m/r: reduce post processing", "M/R Reduce Post Processing Progress", count);
        }
        unique_ptr<DBClientCursor> cursor = _db.query(_config.tempNamespace, BSONObj());
        while (cursor->more()) {
            ScopedTransaction transaction(txn, MODE_X);
            Lock::GlobalWrite lock(txn->lockState());  // TODO(erh) why global?
            BSONObj temp = cursor->nextSafe();
            BSONObj old;

            bool found;
            {
                const std::string& finalNamespace = _config.outputOptions.finalNamespace;
                OldClientContext tx(txn, finalNamespace);
                Collection* coll = getCollectionOrUassert(tx.db(), finalNamespace);
                found = Helpers::findOne(_txn, coll, temp["_id"].wrap(), old, true);
            }

            if (found) {
                // need to reduce
                values.clear();
                values.push_back(temp);
                values.push_back(old);
                Helpers::upsert(_txn,
                                _config.outputOptions.finalNamespace,
                                _config.reducer->finalReduce(values, _config.finalizer.get()));
            } else {
                Helpers::upsert(_txn, _config.outputOptions.finalNamespace, temp);
            }
            pm.hit();
        }
        pm.finished();
    }

    return _safeCount(txn, _db, _config.outputOptions.finalNamespace);
}

/**
 * Insert doc in collection. This should be replicated.
 */
void State::insert(const string& ns, const BSONObj& o) {
    verify(_onDisk);

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        OldClientWriteContext ctx(_txn, ns);
        WriteUnitOfWork wuow(_txn);
        NamespaceString nss(ns);
        uassert(ErrorCodes::NotMaster,
                "no longer master",
                repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nss));
        Collection* coll = getCollectionOrUassert(ctx.db(), ns);

        BSONObjBuilder b;
        if (!o.hasField("_id")) {
            b.appendOID("_id", NULL, true);
        }
        b.appendElements(o);
        BSONObj bo = b.obj();

        StatusWith<BSONObj> res = fixDocumentForInsert(bo);
        uassertStatusOK(res.getStatus());
        if (!res.getValue().isEmpty()) {
            bo = res.getValue();
        }

        // TODO: Consider whether to pass OpDebug for stats tracking under SERVER-23261.
        OpDebug* const nullOpDebug = nullptr;
        uassertStatusOK(coll->insertDocument(_txn, bo, nullOpDebug, true));
        wuow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(_txn, "M/R insert", ns);
}

/**
 * Insert doc into the inc collection. This should not be replicated.
 */
void State::_insertToInc(BSONObj& o) {
    verify(_onDisk);

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        OldClientWriteContext ctx(_txn, _config.incLong);
        WriteUnitOfWork wuow(_txn);
        Collection* coll = getCollectionOrUassert(ctx.db(), _config.incLong);
        bool shouldReplicateWrites = _txn->writesAreReplicated();
        _txn->setReplicatedWrites(false);
        ON_BLOCK_EXIT(&OperationContext::setReplicatedWrites, _txn, shouldReplicateWrites);

        // The documents inserted into the incremental collection are of the form
        // {"0": <key>, "1": <value>}, so we cannot call fixDocumentForInsert(o) here because the
        // check that the document has an "_id" field would fail. Instead, we directly verify that
        // the size of the document to insert is smaller than 16MB.
        if (o.objsize() > BSONObjMaxUserSize) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "object to insert too large for incremental collection"
                                    << ". size in bytes: "
                                    << o.objsize()
                                    << ", max size: "
                                    << BSONObjMaxUserSize);
        }

        // TODO: Consider whether to pass OpDebug for stats tracking under SERVER-23261.
        OpDebug* const nullOpDebug = nullptr;
        uassertStatusOK(coll->insertDocument(_txn, o, nullOpDebug, true, false));
        wuow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(_txn, "M/R insertToInc", _config.incLong);
}

State::State(OperationContext* txn, const Config& c)
    : _config(c), _db(txn), _useIncremental(true), _txn(txn), _size(0), _dupCount(0), _numEmits(0) {
    _temp.reset(new InMemory());
    _onDisk = _config.outputOptions.outType != Config::INMEMORY;
}

bool State::sourceExists() {
    return _db.exists(_config.ns);
}

long long State::incomingDocuments() {
    return _safeCount(
        _txn, _db, _config.ns, _config.filter, QueryOption_SlaveOk, (unsigned)_config.limit);
}

State::~State() {
    if (_onDisk) {
        try {
            dropTempCollections();
        } catch (std::exception& e) {
            error() << "couldn't cleanup after map reduce: " << e.what() << endl;
        }
    }
    if (_scope && !_scope->isKillPending() && _scope->getError().empty()) {
        // cleanup js objects
        try {
            ScriptingFunction cleanup =
                _scope->createFunction("delete _emitCt; delete _keyCt; delete _mrMap;");
            _scope->invoke(cleanup, 0, 0, 0, true);
        } catch (const DBException&) {
            // not important because properties will be reset if scope is reused
            LOG(1) << "MapReduce terminated during state destruction" << endl;
        }
    }
}

/**
 * Initialize the mapreduce operation, creating the inc collection
 */
void State::init() {
    // setup js
    const string userToken =
        AuthorizationSession::get(ClientBasic::getCurrent())->getAuthenticatedUserNamesToken();
    _scope.reset(globalScriptEngine->newScopeForCurrentThread());
    _scope->registerOperation(_txn);
    _scope->setLocalDB(_config.dbname);
    _scope->loadStored(_txn, true);

    if (!_config.scopeSetup.isEmpty())
        _scope->init(&_config.scopeSetup);

    _config.mapper->init(this);
    _config.reducer->init(this);
    if (_config.finalizer)
        _config.finalizer->init(this);
    _scope->setBoolean("_doFinal", _config.finalizer.get() != 0);

    switchMode(_config.jsMode);  // set up js-mode based on Config

    // global JS map/reduce hashmap
    // we use a standard JS object which means keys are only simple types
    // we could also add a real hashmap from a library and object comparison methods
    // for increased performance, we may want to look at v8 Harmony Map support
    // _scope->setObject("_mrMap", BSONObj(), false);
    ScriptingFunction init = _scope->createFunction(
        "_emitCt = 0;"
        "_keyCt = 0;"
        "_dupCt = 0;"
        "_redCt = 0;"
        "if (typeof(_mrMap) === 'undefined') {"
        "  _mrMap = {};"
        "}");
    _scope->invoke(init, 0, 0, 0, true);

    // js function to run reduce on all keys
    // redfunc = _scope->createFunction("for (var key in hashmap) {  print('Key is ' + key);
    // list = hashmap[key]; ret = reduce(key, list); print('Value is ' + ret); };");
    _reduceAll = _scope->createFunction(
        "var map = _mrMap;"
        "var list, ret;"
        "for (var key in map) {"
        "  list = map[key];"
        "  if (list.length != 1) {"
        "    ret = _reduce(key, list);"
        "    map[key] = [ret];"
        "    ++_redCt;"
        "  }"
        "}"
        "_dupCt = 0;");
    massert(16717, "error initializing JavaScript reduceAll function", _reduceAll != 0);

    _reduceAndEmit = _scope->createFunction(
        "var map = _mrMap;"
        "var list, ret;"
        "for (var key in map) {"
        "  list = map[key];"
        "  if (list.length == 1)"
        "    ret = list[0];"
        "  else {"
        "    ret = _reduce(key, list);"
        "    ++_redCt;"
        "  }"
        "  emit(key, ret);"
        "}"
        "delete _mrMap;");
    massert(16718, "error initializing JavaScript reduce/emit function", _reduceAndEmit != 0);

    _reduceAndFinalize = _scope->createFunction(
        "var map = _mrMap;"
        "var list, ret;"
        "for (var key in map) {"
        "  list = map[key];"
        "  if (list.length == 1) {"
        "    if (!_doFinal) { continue; }"
        "    ret = list[0];"
        "  }"
        "  else {"
        "    ret = _reduce(key, list);"
        "    ++_redCt;"
        "  }"
        "  if (_doFinal)"
        "    ret = _finalize(key, ret);"
        "  map[key] = ret;"
        "}");
    massert(16719, "error creating JavaScript reduce/finalize function", _reduceAndFinalize != 0);

    _reduceAndFinalizeAndInsert = _scope->createFunction(
        "var map = _mrMap;"
        "var list, ret;"
        "for (var key in map) {"
        "  list = map[key];"
        "  if (list.length == 1)"
        "    ret = list[0];"
        "  else {"
        "    ret = _reduce(key, list);"
        "    ++_redCt;"
        "  }"
        "  if (_doFinal)"
        "    ret = _finalize(key, ret);"
        "  _nativeToTemp({_id: key, value: ret});"
        "}");
    massert(16720, "error initializing JavaScript functions", _reduceAndFinalizeAndInsert != 0);
}

void State::switchMode(bool jsMode) {
    _jsMode = jsMode;
    if (jsMode) {
        // emit function that stays in JS
        _scope->setFunction("emit",
                            "function(key, value) {"
                            "  if (typeof(key) === 'object') {"
                            "    _bailFromJS(key, value);"
                            "    return;"
                            "  }"
                            "  ++_emitCt;"
                            "  var map = _mrMap;"
                            "  var list = map[key];"
                            "  if (!list) {"
                            "    ++_keyCt;"
                            "    list = [];"
                            "    map[key] = list;"
                            "  }"
                            "  else"
                            "    ++_dupCt;"
                            "  list.push(value);"
                            "}");
        _scope->injectNative("_bailFromJS", _bailFromJS, this);
    } else {
        // emit now populates C++ map
        _scope->injectNative("emit", fast_emit, this);
    }
}

void State::bailFromJS() {
    LOG(1) << "M/R: Switching from JS mode to mixed mode" << endl;

    // reduce and reemit into c++
    switchMode(false);
    _scope->invoke(_reduceAndEmit, 0, 0, 0, true);
    // need to get the real number emitted so far
    _numEmits = _scope->getNumberInt("_emitCt");
    _config.reducer->numReduces = _scope->getNumberInt("_redCt");
}

Collection* State::getCollectionOrUassert(Database* db, StringData ns) {
    Collection* out = db ? db->getCollection(ns) : NULL;
    uassert(18697, "Collection unexpectedly disappeared: " + ns.toString(), out);
    return out;
}

/**
 * Applies last reduce and finalize on a list of tuples (key, val)
 * Inserts single result {_id: key, value: val} into temp collection
 */
void State::finalReduce(BSONList& values) {
    if (!_onDisk || values.size() == 0)
        return;

    BSONObj res = _config.reducer->finalReduce(values, _config.finalizer.get());
    insert(_config.tempNamespace, res);
}

BSONObj _nativeToTemp(const BSONObj& args, void* data) {
    State* state = (State*)data;
    BSONObjIterator it(args);
    state->insert(state->_config.tempNamespace, it.next().Obj());
    return BSONObj();
}

//        BSONObj _nativeToInc( const BSONObj& args, void* data ) {
//            State* state = (State*) data;
//            BSONObjIterator it(args);
//            const BSONObj& obj = it.next().Obj();
//            state->_insertToInc(const_cast<BSONObj&>(obj));
//            return BSONObj();
//        }

/**
 * Applies last reduce and finalize.
 * After calling this method, the temp collection will be completed.
 * If inline, the results will be in the in memory map
 */
void State::finalReduce(OperationContext* txn, CurOp* curOp, ProgressMeterHolder& pm) {
    if (_jsMode) {
        // apply the reduce within JS
        if (_onDisk) {
            _scope->injectNative("_nativeToTemp", _nativeToTemp, this);
            _scope->invoke(_reduceAndFinalizeAndInsert, 0, 0, 0, true);
            return;
        } else {
            _scope->invoke(_reduceAndFinalize, 0, 0, 0, true);
            return;
        }
    }

    if (!_onDisk) {
        // all data has already been reduced, just finalize
        if (_config.finalizer) {
            long size = 0;
            for (InMemory::iterator i = _temp->begin(); i != _temp->end(); ++i) {
                BSONObj key = i->first;
                BSONList& all = i->second;

                verify(all.size() == 1);

                BSONObj res = _config.finalizer->finalize(all[0]);

                all.clear();
                all.push_back(res);
                size += res.objsize();
            }
            _size = size;
        }
        return;
    }

    // use index on "0" to pull sorted data
    verify(_temp->size() == 0);
    BSONObj sortKey = BSON("0" << 1);

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        OldClientWriteContext incCtx(_txn, _config.incLong);
        WriteUnitOfWork wuow(_txn);
        Collection* incColl = getCollectionOrUassert(incCtx.db(), _config.incLong);

        bool foundIndex = false;
        IndexCatalog::IndexIterator ii = incColl->getIndexCatalog()->getIndexIterator(_txn, true);
        // Iterate over incColl's indexes.
        while (ii.more()) {
            IndexDescriptor* currIndex = ii.next();
            BSONObj x = currIndex->infoObj();
            if (sortKey.woCompare(x["key"].embeddedObject()) == 0) {
                foundIndex = true;
                break;
            }
        }

        verify(foundIndex);
        wuow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(_txn, "finalReduce", _config.incLong);

    unique_ptr<AutoGetCollectionForRead> ctx(new AutoGetCollectionForRead(_txn, _config.incLong));

    BSONObj prev;
    BSONList all;

    {
        const auto count = _db.count(_config.incLong, BSONObj(), QueryOption_SlaveOk);
        stdx::lock_guard<Client> lk(*_txn->getClient());
        verify(pm ==
               curOp->setMessage_inlock("m/r: (3/3) final reduce to collection",
                                        "M/R: (3/3) Final Reduce Progress",
                                        count));
    }

    const NamespaceString nss(_config.incLong);
    const ExtensionsCallbackReal extensionsCallback(_txn, &nss);

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setSort(sortKey);

    auto statusWithCQ = CanonicalQuery::canonicalize(txn, std::move(qr), extensionsCallback);
    verify(statusWithCQ.isOK());
    std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    Collection* coll = getCollectionOrUassert(ctx->getDb(), _config.incLong);
    invariant(coll);

    auto statusWithPlanExecutor = getExecutor(
        _txn, coll, std::move(cq), PlanExecutor::YIELD_AUTO, QueryPlannerParams::NO_TABLE_SCAN);
    verify(statusWithPlanExecutor.isOK());

    unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

    // iterate over all sorted objects
    BSONObj o;
    PlanExecutor::ExecState state;
    while (PlanExecutor::ADVANCED == (state = exec->getNext(&o, NULL))) {
        o = o.getOwned();  // we will be accessing outside of the lock
        pm.hit();

        if (dps::compareObjectsAccordingToSort(o, prev, sortKey) == 0) {
            // object is same as previous, add to array
            all.push_back(o);
            if (pm->hits() % 100 == 0) {
                _txn->checkForInterrupt();
            }
            continue;
        }

        exec->saveState();

        ctx.reset();

        // reduce a finalize array
        finalReduce(all);

        ctx.reset(new AutoGetCollectionForRead(_txn, _config.incLong));

        all.clear();
        prev = o;
        all.push_back(o);

        if (!exec->restoreState()) {
            uasserted(34375, "Plan executor killed during mapReduce final reduce");
        }

        _txn->checkForInterrupt();
    }

    uassert(34428,
            "Plan executor error during mapReduce command: " + WorkingSetCommon::toStatusString(o),
            PlanExecutor::IS_EOF == state);

    ctx.reset();
    // reduce and finalize last array
    finalReduce(all);
    ctx.reset(new AutoGetCollectionForRead(_txn, _config.incLong));

    pm.finished();
}

/**
 * Attempts to reduce objects in the memory map.
 * A new memory map will be created to hold the results.
 * If applicable, objects with unique key may be dumped to inc collection.
 * Input and output objects are both {"0": key, "1": val}
 */
void State::reduceInMemory() {
    if (_jsMode) {
        // in js mode the reduce is applied when writing to collection
        return;
    }

    unique_ptr<InMemory> n(new InMemory());  // for new data
    long nSize = 0;
    _dupCount = 0;

    for (InMemory::iterator i = _temp->begin(); i != _temp->end(); ++i) {
        BSONList& all = i->second;

        if (all.size() == 1) {
            // only 1 value for this key
            if (_onDisk) {
                // this key has low cardinality, so just write to collection
                _insertToInc(*(all.begin()));
            } else {
                // add to new map
                nSize += _add(n.get(), all[0]);
            }
        } else if (all.size() > 1) {
            // several values, reduce and add to map
            BSONObj res = _config.reducer->reduce(all);
            nSize += _add(n.get(), res);
        }
    }

    // swap maps
    _temp.reset(n.release());
    _size = nSize;
}

/**
 * Dumps the entire in memory map to the inc collection.
 */
void State::dumpToInc() {
    if (!_onDisk)
        return;

    for (InMemory::iterator i = _temp->begin(); i != _temp->end(); i++) {
        BSONList& all = i->second;
        if (all.size() < 1)
            continue;

        for (BSONList::iterator j = all.begin(); j != all.end(); j++)
            _insertToInc(*j);
    }
    _temp->clear();
    _size = 0;
}

/**
 * Adds object to in memory map
 */
void State::emit(const BSONObj& a) {
    _numEmits++;
    _size += _add(_temp.get(), a);
}

int State::_add(InMemory* im, const BSONObj& a) {
    BSONList& all = (*im)[a];
    all.push_back(a);
    if (all.size() > 1) {
        ++_dupCount;
    }

    return a.objsize() + 16;
}

void State::reduceAndSpillInMemoryStateIfNeeded() {
    // Make sure no DB locks are held, because this method manages its own locking and
    // write units of work.
    invariant(!_txn->lockState()->isLocked());

    if (_jsMode) {
        // try to reduce if it is beneficial
        int dupCt = _scope->getNumberInt("_dupCt");
        int keyCt = _scope->getNumberInt("_keyCt");

        if (keyCt > _config.jsMaxKeys) {
            // too many keys for JS, switch to mixed
            _bailFromJS(BSONObj(), this);
            // then fall through to check map size
        } else if (dupCt > (keyCt * _config.reduceTriggerRatio)) {
            // reduce now to lower mem usage
            Timer t;
            _scope->invoke(_reduceAll, 0, 0, 0, true);
            LOG(3) << "  MR - did reduceAll: keys=" << keyCt << " dups=" << dupCt
                   << " newKeys=" << _scope->getNumberInt("_keyCt") << " time=" << t.millis()
                   << "ms" << endl;
            return;
        }
    }

    if (_jsMode)
        return;

    if (_size > _config.maxInMemSize || _dupCount > (_temp->size() * _config.reduceTriggerRatio)) {
        // attempt to reduce in memory map, if memory is too high or we have many duplicates
        long oldSize = _size;
        Timer t;
        reduceInMemory();
        LOG(3) << "  MR - did reduceInMemory: size=" << oldSize << " dups=" << _dupCount
               << " newSize=" << _size << " time=" << t.millis() << "ms" << endl;

        // if size is still high, or values are not reducing well, dump
        if (_onDisk && (_size > _config.maxInMemSize || _size > oldSize / 2)) {
            dumpToInc();
            LOG(3) << "  MR - dumping to db" << endl;
        }
    }
}

/**
 * emit that will be called by js function
 */
BSONObj fast_emit(const BSONObj& args, void* data) {
    uassert(10077, "fast_emit takes 2 args", args.nFields() == 2);
    uassert(13069,
            "an emit can't be more than half max bson size",
            args.objsize() < (BSONObjMaxUserSize / 2));

    State* state = (State*)data;
    if (args.firstElement().type() == Undefined) {
        BSONObjBuilder b(args.objsize());
        b.appendNull("");
        BSONObjIterator i(args);
        i.next();
        b.append(i.next());
        state->emit(b.obj());
    } else {
        state->emit(args);
    }
    return BSONObj();
}

/**
 * function is called when we realize we cant use js mode for m/r on the 1st key
 */
BSONObj _bailFromJS(const BSONObj& args, void* data) {
    State* state = (State*)data;
    state->bailFromJS();

    // emit this particular key if there is one
    if (!args.isEmpty()) {
        fast_emit(args, data);
    }
    return BSONObj();
}

/**
 * This class represents a map/reduce command executed on a single server
 */
class MapReduceCommand : public Command {
public:
    MapReduceCommand() : Command("mapReduce", false, "mapreduce") {}

    virtual bool slaveOk() const {
        return repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
            repl::ReplicationCoordinator::modeReplSet;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
    }

    virtual void help(stringstream& help) const {
        help << "Run a map/reduce operation on the server.\n";
        help << "Note this is used for aggregation, not querying, in MongoDB.\n";
        help << "http://dochub.mongodb.org/core/mapreduce";
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return mrSupportsWriteConcern(cmd);
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        addPrivilegesRequiredForMapReduce(this, dbname, cmdObj, out);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmd,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        Timer t;

        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmd))
            maybeDisableValidation.emplace(txn);

        auto client = txn->getClient();

        if (client->isInDirectClient()) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::IllegalOperation, "Cannot run mapReduce command from eval()"));
        }

        auto curOp = CurOp::get(txn);

        Config config(dbname, cmd);

        LOG(1) << "mr ns: " << config.ns << endl;

        uassert(16149, "cannot run map reduce without the js engine", globalScriptEngine);

        // Prevent sharding state from changing during the MR.
        unique_ptr<RangePreserver> rangePreserver;
        ScopedCollectionMetadata collMetadata;
        {
            AutoGetCollectionForRead ctx(txn, config.ns);

            Collection* collection = ctx.getCollection();
            if (collection) {
                rangePreserver.reset(new RangePreserver(collection));
            }

            // Get metadata before we check our version, to make sure it doesn't increment
            // in the meantime.  Need to do this in the same lock scope as the block.
            if (ShardingState::get(txn)->needCollectionMetadata(txn, config.ns)) {
                collMetadata = CollectionShardingState::get(txn, config.ns)->getMetadata();
            }
        }

        // Ensure that the RangePreserver is freed under the lock. This is necessary since the
        // RangePreserver's destructor unpins a ClientCursor, and access to the CursorManager must
        // be done under the lock.
        ON_BLOCK_EXIT([txn, &config, &rangePreserver] {
            if (rangePreserver) {
                AutoGetCollectionForRead ctx(txn, config.ns);
                rangePreserver.reset();
            }
        });

        bool shouldHaveData = false;

        BSONObjBuilder countsBuilder;
        BSONObjBuilder timingBuilder;
        State state(txn, config);
        if (!state.sourceExists()) {
            errmsg = "ns doesn't exist";
            return false;
        }
        if (state.isOnDisk()) {
            // this means that it will be doing a write operation, make sure we are on Master
            // ideally this check should be in slaveOk(), but at that point config is not known
            NamespaceString nss(config.ns);
            if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nss)) {
                errmsg = "not master";
                return false;
            }
        }

        try {
            state.init();
            state.prepTempCollection();
            ON_BLOCK_EXIT_OBJ(state, &State::dropTempCollections);

            int progressTotal = 0;
            bool showTotal = true;
            if (state.config().filter.isEmpty()) {
                progressTotal = state.incomingDocuments();
            } else {
                showTotal = false;
                // Set an arbitrary total > 0 so the meter will be activated.
                progressTotal = 1;
            }

            stdx::unique_lock<Client> lk(*txn->getClient());
            ProgressMeter& progress(curOp->setMessage_inlock(
                "m/r: (1/3) emit phase", "M/R: (1/3) Emit Progress", progressTotal));
            lk.unlock();
            progress.showTotal(showTotal);
            ProgressMeterHolder pm(progress);

            // See cast on next line to 32 bit unsigned
            wassert(config.limit < 0x4000000);

            long long mapTime = 0;
            long long reduceTime = 0;
            long long numInputs = 0;

            {
                // We've got a cursor preventing migrations off, now re-establish our
                // useful cursor.

                const NamespaceString nss(config.ns);

                // Need lock and context to use it
                unique_ptr<ScopedTransaction> scopedXact(new ScopedTransaction(txn, MODE_IS));
                unique_ptr<AutoGetDb> scopedAutoDb(new AutoGetDb(txn, nss.db(), MODE_S));

                auto qr = stdx::make_unique<QueryRequest>(nss);
                qr->setFilter(config.filter);
                qr->setSort(config.sort);
                qr->setCollation(config.collation);

                const ExtensionsCallbackReal extensionsCallback(txn, &nss);

                auto statusWithCQ =
                    CanonicalQuery::canonicalize(txn, std::move(qr), extensionsCallback);
                if (!statusWithCQ.isOK()) {
                    uasserted(17238, "Can't canonicalize query " + config.filter.toString());
                    return 0;
                }
                std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

                unique_ptr<PlanExecutor> exec;
                {
                    Database* db = scopedAutoDb->getDb();
                    Collection* coll = State::getCollectionOrUassert(db, config.ns);
                    invariant(coll);

                    auto statusWithPlanExecutor =
                        getExecutor(txn, coll, std::move(cq), PlanExecutor::YIELD_AUTO);
                    if (!statusWithPlanExecutor.isOK()) {
                        uasserted(17239,
                                  "Can't get executor for query " + config.filter.toString());
                        return 0;
                    }

                    exec = std::move(statusWithPlanExecutor.getValue());
                }

                {
                    stdx::lock_guard<Client>(*txn->getClient());
                    CurOp::get(txn)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
                }

                Timer mt;

                // go through each doc
                BSONObj o;
                PlanExecutor::ExecState execState;
                while (PlanExecutor::ADVANCED == (execState = exec->getNext(&o, NULL))) {
                    // check to see if this is a new object we don't own yet
                    // because of a chunk migration
                    if (collMetadata) {
                        ShardKeyPattern kp(collMetadata->getKeyPattern());
                        if (!collMetadata->keyBelongsToMe(kp.extractShardKeyFromDoc(o))) {
                            continue;
                        }
                    }

                    // do map
                    if (config.verbose)
                        mt.reset();
                    config.mapper->map(o);
                    if (config.verbose)
                        mapTime += mt.micros();

                    // Check if the state accumulated so far needs to be written to a
                    // collection. This may yield the DB lock temporarily and then
                    // acquire it again.
                    //
                    numInputs++;
                    if (numInputs % 100 == 0) {
                        Timer t;

                        // TODO: As an optimization, we might want to do the save/restore
                        // state and yield inside the reduceAndSpillInMemoryState method, so
                        // it only happens if necessary.
                        exec->saveState();

                        scopedAutoDb.reset();
                        scopedXact.reset();

                        state.reduceAndSpillInMemoryStateIfNeeded();

                        scopedXact.reset(new ScopedTransaction(txn, MODE_IS));
                        scopedAutoDb.reset(new AutoGetDb(txn, nss.db(), MODE_S));

                        if (!exec->restoreState()) {
                            return appendCommandStatus(
                                result,
                                Status(ErrorCodes::OperationFailed,
                                       str::stream()
                                           << "Executor killed during mapReduce command"));
                        }

                        reduceTime += t.micros();

                        txn->checkForInterrupt();
                    }

                    pm.hit();

                    if (config.limit && numInputs >= config.limit)
                        break;
                }

                if (PlanExecutor::DEAD == execState || PlanExecutor::FAILURE == execState) {
                    return appendCommandStatus(
                        result,
                        Status(ErrorCodes::OperationFailed,
                               str::stream() << "Executor error during mapReduce command: "
                                             << WorkingSetCommon::toStatusString(o)));
                }

                // Record the indexes used by the PlanExecutor.
                PlanSummaryStats stats;
                Explain::getSummaryStats(*exec, &stats);

                // TODO SERVER-23261: Confirm whether this is the correct place to gather all
                // metrics. There is no harm adding here for the time being.
                curOp->debug().setPlanSummaryMetrics(stats);

                Collection* coll = scopedAutoDb->getDb()->getCollection(config.ns);
                invariant(coll);  // 'exec' hasn't been killed, so collection must be alive.
                coll->infoCache()->notifyOfQuery(txn, stats.indexesUsed);

                if (curOp->shouldDBProfile(curOp->elapsedMillis())) {
                    BSONObjBuilder execStatsBob;
                    Explain::getWinningPlanStats(exec.get(), &execStatsBob);
                    curOp->debug().execStats = execStatsBob.obj();
                }
            }
            pm.finished();

            txn->checkForInterrupt();

            // update counters
            countsBuilder.appendNumber("input", numInputs);
            countsBuilder.appendNumber("emit", state.numEmits());
            if (state.numEmits())
                shouldHaveData = true;

            timingBuilder.appendNumber("mapTime", mapTime / 1000);
            timingBuilder.append("emitLoop", t.millis());

            {
                stdx::lock_guard<Client> lk(*txn->getClient());
                curOp->setMessage_inlock("m/r: (2/3) final reduce in memory",
                                         "M/R: (2/3) Final In-Memory Reduce Progress");
            }
            Timer rt;
            // do reduce in memory
            // this will be the last reduce needed for inline mode
            state.reduceInMemory();
            // if not inline: dump the in memory map to inc collection, all data is on disk
            state.dumpToInc();
            // final reduce
            state.finalReduce(txn, curOp, pm);
            reduceTime += rt.micros();
            countsBuilder.appendNumber("reduce", state.numReduces());
            timingBuilder.appendNumber("reduceTime", reduceTime / 1000);
            timingBuilder.append("mode", state.jsMode() ? "js" : "mixed");

            long long finalCount = state.postProcessCollection(txn, curOp, pm);
            state.appendResults(result);

            timingBuilder.appendNumber("total", t.millis());
            result.appendNumber("timeMillis", t.millis());
            countsBuilder.appendNumber("output", finalCount);
            if (config.verbose)
                result.append("timing", timingBuilder.obj());
            result.append("counts", countsBuilder.obj());

            if (finalCount == 0 && shouldHaveData) {
                result.append("cmd", cmd);
                errmsg = "there were emits but no data!";
                return false;
            }
        } catch (SendStaleConfigException& e) {
            log() << "mr detected stale config, should retry" << causedBy(e) << endl;
            throw e;
        }
        // TODO:  The error handling code for queries is v. fragile,
        // *requires* rethrow AssertionExceptions - should probably fix.
        catch (AssertionException& e) {
            log() << "mr failed, removing collection" << causedBy(e) << endl;
            throw e;
        } catch (std::exception& e) {
            log() << "mr failed, removing collection" << causedBy(e) << endl;
            throw e;
        } catch (...) {
            log() << "mr failed for unknown reason, removing collection" << endl;
            throw;
        }

        return true;
    }

} mapReduceCommand;

/**
 * This class represents a map/reduce command executed on the output server of a sharded env
 */
class MapReduceFinishCommand : public Command {
public:
    void help(stringstream& h) const {
        h << "internal";
    }
    MapReduceFinishCommand() : Command("mapreduce.shardedfinish") {}
    virtual bool slaveOk() const {
        return repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
            repl::ReplicationCoordinator::modeReplSet;
    }
    virtual bool slaveOverrideOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::CommandNotSupported,
                       str::stream() << "Can not execute mapReduce with output database " << dbname
                                     << " which lives on config servers"));
        }

        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj))
            maybeDisableValidation.emplace(txn);

        ShardedConnectionInfo::addHook();

        // legacy name
        string shardedOutputCollection = cmdObj["shardedOutputCollection"].valuestrsafe();
        verify(shardedOutputCollection.size() > 0);
        string inputNS;
        if (cmdObj["inputDB"].type() == String) {
            inputNS = cmdObj["inputDB"].String() + "." + shardedOutputCollection;
        } else {
            inputNS = dbname + "." + shardedOutputCollection;
        }

        CurOp* curOp = CurOp::get(txn);

        Config config(dbname, cmdObj.firstElement().embeddedObjectUserCheck());
        State state(txn, config);
        state.init();

        // no need for incremental collection because records are already sorted
        state._useIncremental = false;
        config.incLong = config.tempNamespace;

        BSONObj shardCounts = cmdObj["shardCounts"].embeddedObjectUserCheck();
        BSONObj counts = cmdObj["counts"].embeddedObjectUserCheck();

        stdx::unique_lock<Client> lk(*txn->getClient());
        ProgressMeterHolder pm(curOp->setMessage_inlock("m/r: merge sort and reduce",
                                                        "M/R Merge Sort and Reduce Progress"));
        lk.unlock();
        set<string> servers;

        {
            // parse per shard results
            BSONObjIterator i(shardCounts);
            while (i.more()) {
                BSONElement e = i.next();
                std::string server = e.fieldName();
                servers.insert(server);

                if (!grid.shardRegistry()->getShard(txn, server)) {
                    return appendCommandStatus(
                        result,
                        Status(ErrorCodes::ShardNotFound,
                               str::stream() << "Shard not found for server: " << server));
                }
            }
        }

        state.prepTempCollection();
        ON_BLOCK_EXIT_OBJ(state, &State::dropTempCollections);

        BSONList values;
        if (!config.outputOptions.outDB.empty()) {
            BSONObjBuilder loc;
            if (!config.outputOptions.outDB.empty())
                loc.append("db", config.outputOptions.outDB);
            if (!config.outputOptions.collectionName.empty())
                loc.append("collection", config.outputOptions.collectionName);
            result.append("result", loc.obj());
        } else {
            if (!config.outputOptions.collectionName.empty())
                result.append("result", config.outputOptions.collectionName);
        }

        auto status = grid.catalogCache()->getDatabase(txn, dbname);
        if (!status.isOK()) {
            return appendCommandStatus(result, status.getStatus());
        }

        shared_ptr<DBConfig> confOut = status.getValue();

        vector<shared_ptr<Chunk>> chunks;
        if (confOut->isSharded(config.outputOptions.finalNamespace)) {
            shared_ptr<ChunkManager> cm =
                confOut->getChunkManager(txn, config.outputOptions.finalNamespace);

            // Fetch result from other shards 1 chunk at a time. It would be better to do
            // just one big $or query, but then the sorting would not be efficient.
            const string shardName = ShardingState::get(txn)->getShardName();
            const ChunkMap& chunkMap = cm->getChunkMap();

            for (ChunkMap::const_iterator it = chunkMap.begin(); it != chunkMap.end(); ++it) {
                shared_ptr<Chunk> chunk = it->second;
                if (chunk->getShardId() == shardName) {
                    chunks.push_back(chunk);
                }
            }
        }

        long long inputCount = 0;
        unsigned int index = 0;
        BSONObj query;
        BSONArrayBuilder chunkSizes;
        while (true) {
            shared_ptr<Chunk> chunk;
            if (chunks.size() > 0) {
                chunk = chunks[index];
                BSONObjBuilder b;
                b.appendAs(chunk->getMin().firstElement(), "$gte");
                b.appendAs(chunk->getMax().firstElement(), "$lt");
                query = BSON("_id" << b.obj());
                //                        chunkSizes.append(min);
            }

            // reduce from each shard for a chunk
            BSONObj sortKey = BSON("_id" << 1);
            ParallelSortClusteredCursor cursor(
                servers, inputNS, Query(query).sort(sortKey), QueryOption_NoCursorTimeout);
            cursor.init(txn);
            int chunkSize = 0;

            while (cursor.more() || !values.empty()) {
                BSONObj t;
                if (cursor.more()) {
                    t = cursor.next().getOwned();
                    ++inputCount;

                    if (values.size() == 0) {
                        values.push_back(t);
                        continue;
                    }

                    if (dps::compareObjectsAccordingToSort(t, *(values.begin()), sortKey) == 0) {
                        values.push_back(t);
                        continue;
                    }
                }

                BSONObj res = config.reducer->finalReduce(values, config.finalizer.get());
                chunkSize += res.objsize();
                if (state.isOnDisk())
                    state.insert(config.tempNamespace, res);
                else
                    state.emit(res);
                values.clear();
                if (!t.isEmpty())
                    values.push_back(t);
            }

            if (chunk) {
                chunkSizes.append(chunk->getMin());
                chunkSizes.append(chunkSize);
            }
            if (++index >= chunks.size())
                break;
        }

        // Forget temporary input collection, if output is sharded collection
        ShardConnection::forgetNS(inputNS);

        result.append("chunkSizes", chunkSizes.arr());

        long long outputCount = state.postProcessCollection(txn, curOp, pm);
        state.appendResults(result);

        BSONObjBuilder countsB(32);
        countsB.append("input", inputCount);
        countsB.append("reduce", state.numReduces());
        countsB.append("output", outputCount);
        result.append("counts", countsB.obj());

        return 1;
    }
} mapReduceFinishCommand;
}
}
