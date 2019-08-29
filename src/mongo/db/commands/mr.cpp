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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands/mr.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/parallel.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/stale_exception.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {

using std::set;
using std::shared_ptr;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

using IndexVersion = IndexDescriptor::IndexVersion;

namespace dps = ::mongo::dotted_path_support;

namespace mr {
namespace {

Rarely mapParamsDeprecationSampler;  // Used to occasionally log deprecation messages.

/**
 * Runs a count against the namespace specified by 'ns'. If the caller holds the global write lock,
 * then this function does not acquire any additional locks.
 */
unsigned long long collectionCount(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   bool callerHoldsGlobalLock) {
    boost::optional<AutoGetCollectionForReadCommand> ctx;

    Collection* coll = nullptr;

    // If the global write lock is held, we must avoid using AutoGetCollectionForReadCommand as it
    // may lead to deadlock when waiting for a majority snapshot to be committed. See SERVER-24596.
    if (callerHoldsGlobalLock) {
        auto databaseHolder = DatabaseHolder::get(opCtx);
        auto db = databaseHolder->getDb(opCtx, nss.ns());
        if (db) {
            coll = db->getCollection(opCtx, nss);
        }
    } else {
        ctx.emplace(opCtx, nss);
        coll = ctx->getCollection();
    }

    return coll ? coll->numRecords(opCtx) : 0;
}

/**
 * Emit that will be called by a js function.
 */
BSONObj fastEmit(const BSONObj& args, void* data) {
    uassert(10077, "emit takes 2 args", args.nFields() == 2);
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
 * This function is called when we realize we cant use js mode for m/r on the 1st key.
 */
BSONObj _bailFromJS(const BSONObj& args, void* data) {
    State* state = (State*)data;
    state->bailFromJS();

    // emit this particular key if there is one
    if (!args.isEmpty()) {
        fastEmit(args, data);
    }
    return BSONObj();
}

template <class AutoT>
void assertCollectionNotNull(const NamespaceString& nss, AutoT& autoT) {
    uassert(18698, "Collection unexpectedly disappeared: " + nss.ns(), autoT.getCollection());
}

/**
 * Clean up the temporary and incremental collections
 */
void dropTempCollections(OperationContext* cleanupOpCtx,
                         const NamespaceString& tempNamespace,
                         const NamespaceString& incLong) {
    // Make sure we enforce prepare conflicts before writing.
    EnforcePrepareConflictsBlock enforcePrepare(cleanupOpCtx);

    if (!tempNamespace.isEmpty()) {
        writeConflictRetry(
            cleanupOpCtx,
            "M/R dropTempCollections",
            tempNamespace.ns(),
            [cleanupOpCtx, &tempNamespace] {
                AutoGetDb autoDb(cleanupOpCtx, tempNamespace.db(), MODE_X);
                if (auto db = autoDb.getDb()) {
                    if (auto collection = db->getCollection(cleanupOpCtx, tempNamespace)) {
                        uassert(ErrorCodes::PrimarySteppedDown,
                                str::stream() << "no longer primary while dropping temporary "
                                                 "collection for mapReduce: "
                                              << tempNamespace.ns(),
                                repl::ReplicationCoordinator::get(cleanupOpCtx)
                                    ->canAcceptWritesFor(cleanupOpCtx, tempNamespace));
                        BackgroundOperation::assertNoBgOpInProgForNs(tempNamespace.ns());
                        IndexBuildsCoordinator::get(cleanupOpCtx)
                            ->assertNoIndexBuildInProgForCollection(collection->uuid());
                        WriteUnitOfWork wunit(cleanupOpCtx);
                        uassertStatusOK(db->dropCollection(cleanupOpCtx, tempNamespace));
                        wunit.commit();
                    }
                }
            });
        // Always forget about temporary namespaces, so we don't cache lots of them
        ShardConnection::forgetNS(tempNamespace.ns());
    }
    if (!incLong.isEmpty()) {
        writeConflictRetry(
            cleanupOpCtx, "M/R dropTempCollections", incLong.ns(), [cleanupOpCtx, &incLong] {
                Lock::DBLock lk(cleanupOpCtx, incLong.db(), MODE_X);
                auto databaseHolder = DatabaseHolder::get(cleanupOpCtx);
                if (auto db = databaseHolder->getDb(cleanupOpCtx, incLong.ns())) {
                    if (auto collection = db->getCollection(cleanupOpCtx, incLong)) {
                        BackgroundOperation::assertNoBgOpInProgForNs(incLong.ns());
                        IndexBuildsCoordinator::get(cleanupOpCtx)
                            ->assertNoIndexBuildInProgForCollection(collection->uuid());
                        WriteUnitOfWork wunit(cleanupOpCtx);
                        uassertStatusOK(db->dropCollection(cleanupOpCtx, incLong));
                        wunit.commit();
                    }
                }
            });

        ShardConnection::forgetNS(incLong.ns());
    }
}

}  // namespace

AtomicWord<unsigned> Config::jobNumber;

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

    s->invokeSafe(_func.func(), &o, nullptr);

    // We don't want to use o.objsize() to size b since there are many cases where the point of
    // finalize is converting many fields to 1
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

    s->invokeSafe(_func.func(), &args, nullptr);
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
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "'mapReduce' must be of type String",
            cmdObj.firstElement().type() == BSONType::String);
    nss = NamespaceString(dbname, cmdObj.firstElement().valueStringData());
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid namespace: " << nss.ns(),
            nss.isValid());

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

    outputOptions = mr::parseOutputOptions(dbname, cmdObj);

    shardedFirstPass = false;
    if (cmdObj.hasField("shardedFirstPass") && cmdObj["shardedFirstPass"].trueValue()) {
        massert(16054,
                "shardedFirstPass should only use replace outType",
                outputOptions.outType == mr::OutputType::kReplace);
        shardedFirstPass = true;
    }

    if (outputOptions.outType != mr::OutputType::kInMemory) {
        // Create names for the temp collection and the incremental collection. The incremental
        // collection goes in the "local" database, so that it doesn't get replicated.
        const std::string& outDBName = outputOptions.outDB.empty() ? dbname : outputOptions.outDB;
        const std::string tmpCollDesc = str::stream()
            << "tmp.mr." << cmdObj.firstElement().valueStringData() << "_"
            << jobNumber.fetchAndAdd(1);
        tempNamespace = NamespaceString(outDBName, tmpCollDesc);

        // The name of the incremental collection includes the name of the database that we put
        // temporary collection in, to make it easier to see which incremental database is paired
        // with which temporary database when debugging.
        incLong =
            NamespaceString("local", str::stream() << tmpCollDesc << "_" << outDBName << "_inc");
    }

    {
        // scope and code

        if (cmdObj["scope"].type() == Object)
            scopeSetup = cmdObj["scope"].embeddedObjectUserCheck().getOwned();

        mapper.reset(new JSMapper(cmdObj["map"]));
        reducer.reset(new JSReducer(cmdObj["reduce"]));
        if (cmdObj["finalize"].type() && cmdObj["finalize"].trueValue())
            finalizer.reset(new JSFinalizer(cmdObj["finalize"]));

        // DEPRECATED
        if (auto mapParamsElem = cmdObj["mapparams"]) {
            if (mapParamsDeprecationSampler.tick()) {
                warning() << "The mapparams option to MapReduce is deprecated.";
            }
            if (mapParamsElem.type() == Array) {
                mapParams = mapParamsElem.embeddedObjectUserCheck().getOwned();
            }
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
 * Create temporary collection, set up indexes
 */
void State::prepTempCollection() {
    if (!_onDisk)
        return;

    // Make sure we enforce prepare conflicts before writing.
    EnforcePrepareConflictsBlock enforcePrepare(_opCtx);

    dropTempCollections(
        _opCtx, _config.tempNamespace, _useIncremental ? _config.incLong : NamespaceString());

    if (_useIncremental) {
        // Create the inc collection and make sure we have index on "0" key. The inc collection is
        // in the "local" database, so it does not get replicated to secondaries.
        writeConflictRetry(_opCtx, "M/R prepTempCollection", _config.incLong.ns(), [this] {
            AutoGetOrCreateDb autoGetIncCollDb(_opCtx, _config.incLong.db(), MODE_X);
            auto const db = autoGetIncCollDb.getDb();
            invariant(!db->getCollection(_opCtx, _config.incLong));

            CollectionOptions options;
            options.setNoIdIndex();
            options.temp = true;
            options.uuid.emplace(UUID::gen());

            WriteUnitOfWork wuow(_opCtx);
            auto incColl = db->createCollection(
                _opCtx, _config.incLong, options, false /* force no _id index */);

            auto rawIndexSpec = BSON("key" << BSON("0" << 1) << "name"
                                           << "_temp_0");
            auto indexSpec = uassertStatusOK(index_key_validate::validateIndexSpec(
                _opCtx, rawIndexSpec, serverGlobalParams.featureCompatibility));

            uassertStatusOKWithContext(
                incColl->getIndexCatalog()->createIndexOnEmptyCollection(_opCtx, indexSpec),
                str::stream() << "createIndex failed for mr incLong ns " << _config.incLong.ns());
            wuow.commit();

            CollectionShardingRuntime::get(_opCtx, _config.incLong)
                ->setFilteringMetadata(_opCtx, CollectionMetadata());
        });
    }

    CollectionOptions finalOptions;
    vector<BSONObj> indexesToInsert;

    {
        // Copy indexes and collection options into temporary storage
        AutoGetCollection autoGetFinalColl(_opCtx, _config.outputOptions.finalNamespace, MODE_IS);

        auto const finalColl = autoGetFinalColl.getCollection();
        if (finalColl) {
            finalOptions =
                DurableCatalog::get(_opCtx)->getCollectionOptions(_opCtx, finalColl->ns());

            std::unique_ptr<IndexCatalog::IndexIterator> ii =
                finalColl->getIndexCatalog()->getIndexIterator(_opCtx, true);
            // Iterate over finalColl's indexes.
            while (ii->more()) {
                const IndexDescriptor* currIndex = ii->next()->descriptor();
                BSONObjBuilder b;

                // Copy over contents of the index descriptor's infoObj.
                BSONObjIterator j(currIndex->infoObj());
                while (j.more()) {
                    BSONElement e = j.next();
                    if (e.fieldNameStringData() == "_id")
                        continue;
                    b.append(e);
                }
                indexesToInsert.push_back(b.obj());
            }
        }
    }

    writeConflictRetry(_opCtx, "M/R prepTempCollection", _config.tempNamespace.ns(), [&] {
        // Create temp collection and insert the indexes from temporary storage
        AutoGetOrCreateDb autoGetFinalDb(_opCtx, _config.tempNamespace.db(), MODE_X);
        auto const db = autoGetFinalDb.getDb();
        invariant(!db->getCollection(_opCtx, _config.tempNamespace));

        uassert(
            ErrorCodes::PrimarySteppedDown,
            str::stream() << "no longer primary while creating temporary collection for mapReduce: "
                          << _config.tempNamespace.ns(),
            repl::ReplicationCoordinator::get(_opCtx)->canAcceptWritesFor(_opCtx,
                                                                          _config.tempNamespace));

        CollectionOptions options = finalOptions;
        options.temp = true;

        // If a UUID for the final output collection was sent by mongos (i.e., the final output
        // collection is sharded), use the UUID mongos sent when creating the temp collection.
        // When the temp collection is renamed to the final output collection, the UUID will be
        // preserved.
        options.uuid.emplace(_config.finalOutputCollUUID ? *_config.finalOutputCollUUID
                                                         : UUID::gen());

        // Override createCollection's prohibition on creating new replicated collections without an
        // _id index.
        const bool buildIdIndex = (options.autoIndexId == CollectionOptions::YES ||
                                   options.autoIndexId == CollectionOptions::DEFAULT);

        WriteUnitOfWork wuow(_opCtx);
        auto const tempColl =
            db->createCollection(_opCtx, _config.tempNamespace, options, buildIdIndex);

        for (const auto& indexToInsert : indexesToInsert) {
            try {
                uassertStatusOK(tempColl->getIndexCatalog()->createIndexOnEmptyCollection(
                    _opCtx, indexToInsert));
            } catch (const ExceptionFor<ErrorCodes::IndexAlreadyExists>&) {
                continue;
            }

            // Log the createIndex operation.
            _opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                _opCtx, _config.tempNamespace, tempColl->uuid(), indexToInsert, false);
        }
        wuow.commit();

        CollectionShardingRuntime::get(_opCtx, _config.tempNamespace)
            ->setFilteringMetadata(_opCtx, CollectionMetadata());
    });
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
                                BSON("splitVector" << _config.outputOptions.finalNamespace.ns()
                                                   << "keyPattern" << idKey << "maxChunkSizeBytes"
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
        _scope->invoke(getResult, nullptr, nullptr, 0, false);
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
long long State::postProcessCollection(OperationContext* opCtx, CurOp* curOp) {
    if (_onDisk == false || _config.outputOptions.outType == mr::OutputType::kInMemory)
        return numInMemKeys();

    bool holdingGlobalLock = false;
    if (_config.outputOptions.outNonAtomic)
        return postProcessCollectionNonAtomic(opCtx, curOp, holdingGlobalLock);

    invariant(!opCtx->lockState()->isLocked());

    // This must be global because we may write across different databases.
    Lock::GlobalWrite lock(opCtx);
    holdingGlobalLock = true;
    return postProcessCollectionNonAtomic(opCtx, curOp, holdingGlobalLock);
}

long long State::postProcessCollectionNonAtomic(OperationContext* opCtx,
                                                CurOp* curOp,
                                                bool callerHoldsGlobalLock) {
    // Make sure we enforce prepare conflicts before writing.
    EnforcePrepareConflictsBlock enforcePrepare(opCtx);

    if (_config.outputOptions.finalNamespace == _config.tempNamespace)
        return collectionCount(opCtx, _config.outputOptions.finalNamespace, callerHoldsGlobalLock);

    if (_config.outputOptions.outType == mr::OutputType::kReplace ||
        collectionCount(opCtx, _config.outputOptions.finalNamespace, callerHoldsGlobalLock) == 0) {
        // This must be global because we may write across different databases.
        Lock::GlobalWrite lock(opCtx);
        // replace: just rename from temp to final collection name, dropping previous collection
        _db.dropCollection(_config.outputOptions.finalNamespace.ns());
        BSONObj info;

        if (!_db.runCommand("admin",
                            BSON("renameCollection" << _config.tempNamespace.ns() << "to"
                                                    << _config.outputOptions.finalNamespace.ns()
                                                    << "stayTemp" << _config.shardedFirstPass),
                            info)) {
            uasserted(10076, str::stream() << "rename failed: " << info);
        }

        _db.dropCollection(_config.tempNamespace.ns());
    } else if (_config.outputOptions.outType == mr::OutputType::kMerge) {
        // merge: upsert new docs into old collection
        const auto count = collectionCount(opCtx, _config.tempNamespace, callerHoldsGlobalLock);

        ProgressMeterHolder pm;
        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            pm.set(curOp->setProgress_inlock("M/R Merge Post Processing", count));
        }

        unique_ptr<DBClientCursor> cursor = _db.query(_config.tempNamespace, BSONObj());
        while (cursor->more()) {
            Lock::DBLock lock(opCtx, _config.outputOptions.finalNamespace.db(), MODE_X);
            BSONObj o = cursor->nextSafe();
            Helpers::upsert(opCtx, _config.outputOptions.finalNamespace.ns(), o);
            pm.hit();
        }
        _db.dropCollection(_config.tempNamespace.ns());
        pm.finished();
    } else if (_config.outputOptions.outType == mr::OutputType::kReduce) {
        // reduce: apply reduce op on new result and existing one
        BSONList values;

        const auto count = collectionCount(opCtx, _config.tempNamespace, callerHoldsGlobalLock);

        ProgressMeterHolder pm;
        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            pm.set(curOp->setProgress_inlock("M/R Reduce Post Processing", count));
        }

        unique_ptr<DBClientCursor> cursor = _db.query(_config.tempNamespace, BSONObj());
        while (cursor->more()) {
            // This must be global because we may write across different databases.
            Lock::GlobalWrite lock(opCtx);
            BSONObj temp = cursor->nextSafe();
            BSONObj old;

            const bool found = [&] {
                AutoGetCollection autoColl(opCtx, _config.outputOptions.finalNamespace, MODE_IS);
                assertCollectionNotNull(_config.outputOptions.finalNamespace, autoColl);
                return Helpers::findOne(
                    opCtx, autoColl.getCollection(), temp["_id"].wrap(), old, true);
            }();

            if (found) {
                // need to reduce
                values.clear();
                values.push_back(temp);
                values.push_back(old);
                Helpers::upsert(opCtx,
                                _config.outputOptions.finalNamespace.ns(),
                                _config.reducer->finalReduce(values, _config.finalizer.get()));
            } else {
                Helpers::upsert(opCtx, _config.outputOptions.finalNamespace.ns(), temp);
            }
            pm.hit();
        }
        pm.finished();
    }

    return collectionCount(opCtx, _config.outputOptions.finalNamespace, callerHoldsGlobalLock);
}

/**
 * Insert doc in collection. This should be replicated.
 */
void State::insert(const NamespaceString& nss, const BSONObj& o) {
    invariant(_onDisk);

    // Make sure we enforce prepare conflicts before writing.
    EnforcePrepareConflictsBlock enforcePrepare(_opCtx);

    writeConflictRetry(_opCtx, "M/R insert", nss.ns(), [this, &nss, &o] {
        AutoGetCollection autoColl(_opCtx, nss, MODE_IX);
        uassert(
            ErrorCodes::PrimarySteppedDown,
            str::stream() << "no longer primary while inserting mapReduce result into collection: "
                          << nss << ": " << redact(o),
            repl::ReplicationCoordinator::get(_opCtx)->canAcceptWritesFor(_opCtx, nss));
        assertCollectionNotNull(nss, autoColl);

        WriteUnitOfWork wuow(_opCtx);
        BSONObjBuilder b;
        if (!o.hasField("_id")) {
            b.appendOID("_id", nullptr, true);
        }
        b.appendElements(o);
        BSONObj bo = b.obj();

        auto fixedDoc = uassertStatusOK(fixDocumentForInsert(_opCtx->getServiceContext(), bo));
        if (!fixedDoc.isEmpty()) {
            bo = fixedDoc;
        }

        // TODO: Consider whether to pass OpDebug for stats tracking under SERVER-23261.
        OpDebug* const nullOpDebug = nullptr;
        uassertStatusOK(autoColl.getCollection()->insertDocument(
            _opCtx, InsertStatement(bo), nullOpDebug, true));
        wuow.commit();
    });
}

/**
 * Insert doc into the inc collection. The inc collection is in the "local" database, so this insert
 * will not be replicated.
 */
void State::_insertToInc(BSONObj& o) {
    verify(_onDisk);

    // Make sure we enforce prepare conflicts before writing.
    EnforcePrepareConflictsBlock enforcePrepare(_opCtx);

    writeConflictRetry(_opCtx, "M/R insertToInc", _config.incLong.ns(), [this, &o] {
        AutoGetCollection autoColl(_opCtx, _config.incLong, MODE_IX);
        assertCollectionNotNull(_config.incLong, autoColl);

        WriteUnitOfWork wuow(_opCtx);
        // The documents inserted into the incremental collection are of the form
        // {"0": <key>, "1": <value>}, so we cannot call fixDocumentForInsert(o) here because the
        // check that the document has an "_id" field would fail. Instead, we directly verify that
        // the size of the document to insert is smaller than 16MB.
        if (o.objsize() > BSONObjMaxUserSize) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "object to insert too large for incremental collection"
                                    << ". size in bytes: " << o.objsize()
                                    << ", max size: " << BSONObjMaxUserSize);
        }

        // TODO: Consider whether to pass OpDebug for stats tracking under SERVER-23261.
        OpDebug* const nullOpDebug = nullptr;
        uassertStatusOK(autoColl.getCollection()->insertDocument(
            _opCtx, InsertStatement(o), nullOpDebug, false));
        wuow.commit();
    });
}

State::State(OperationContext* opCtx, const Config& c)
    : _config(c),
      _db(opCtx),
      _useIncremental(true),
      _opCtx(opCtx),
      _size(0),
      _dupCount(0),
      _numEmits(0) {
    _temp.reset(new InMemory());
    _onDisk = _config.outputOptions.outType != mr::OutputType::kInMemory;
}

bool State::sourceExists() {
    return _db.exists(_config.nss.ns());
}

State::~State() {
    if (_onDisk) {
        try {
            // If we're here because the map-reduce got interrupted, any attempt to drop temporary
            // collections within the same operation context is guaranteed to fail as soon as we try
            // to take the X-lock for the database. (An UninterruptibleLockGuard would allow
            // dropTempCollections() to take the locks it needs, but taking an X-lock in
            // UninterruptibleLockGuard context is not allowed.)
            //
            // We don't want every single interrupted map-reduce to leak temporary collections that
            // will stick around until a server restart, so we execute the cleanup as though it's a
            // new operation, by constructing a new Client and OperationContext. It's possible that
            // the new operation will also get interrupted, but dropTempCollections() is short
            // lived, so the odds are acceptably low.
            auto cleanupClient = _opCtx->getServiceContext()->makeClient("M/R cleanup");
            AlternativeClientRegion acr(cleanupClient);
            auto cleanupOpCtx = cc().makeOperationContext();
            dropTempCollections(cleanupOpCtx.get(),
                                _config.tempNamespace,
                                _useIncremental ? _config.incLong : NamespaceString());
        } catch (...) {
            error() << "Unable to drop temporary collection created by mapReduce: "
                    << _config.tempNamespace
                    << ". This collection will be removed automatically "
                       "the next time the server starts up. "
                    << exceptionToStatus();
        }
    }
    if (_scope && !_scope->isKillPending() && _scope->getError().empty()) {
        // cleanup js objects
        try {
            ScriptingFunction cleanup =
                _scope->createFunction("delete _emitCt; delete _keyCt; delete _mrMap;");
            _scope->invoke(cleanup, nullptr, nullptr, 0, true);
        } catch (const DBException&) {
            // not important because properties will be reset if scope is reused
            LOG(1) << "MapReduce terminated during state destruction";
        }
    }
}

/**
 * Initialize the mapreduce operation, creating the inc collection
 */
void State::init() {
    // setup js
    _scope.reset(getGlobalScriptEngine()->newScopeForCurrentThread());
    _scope->requireOwnedObjects();
    _scope->registerOperation(_opCtx);
    _scope->setLocalDB(_config.dbname);
    _scope->loadStored(_opCtx, true);

    if (!_config.scopeSetup.isEmpty())
        _scope->init(&_config.scopeSetup);

    _config.mapper->init(this);
    _config.reducer->init(this);
    if (_config.finalizer)
        _config.finalizer->init(this);
    _scope->setBoolean("_doFinal", _config.finalizer.get() != nullptr);

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
    _scope->invoke(init, nullptr, nullptr, 0, true);

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
        // Emit function that stays in JS
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
        // Emit now populates C++ map
        _scope->injectNative("emit", fastEmit, this);
    }
}

void State::bailFromJS() {
    LOG(1) << "M/R: Switching from JS mode to mixed mode";

    // reduce and reemit into c++
    switchMode(false);
    _scope->invoke(_reduceAndEmit, nullptr, nullptr, 0, true);
    // need to get the real number emitted so far
    _numEmits = _scope->getNumberInt("_emitCt");
    _config.reducer->numReduces = _scope->getNumberInt("_redCt");
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
void State::finalReduce(OperationContext* opCtx, CurOp* curOp) {
    if (_jsMode) {
        // apply the reduce within JS
        if (_onDisk) {
            _scope->injectNative("_nativeToTemp", _nativeToTemp, this);
            _scope->invoke(_reduceAndFinalizeAndInsert, nullptr, nullptr, 0, true);
            return;
        } else {
            _scope->invoke(_reduceAndFinalize, nullptr, nullptr, 0, true);
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

    {
        AutoGetCollection autoIncColl(_opCtx, _config.incLong, MODE_IS);
        assertCollectionNotNull(_config.incLong, autoIncColl);

        bool foundIndex = false;
        std::unique_ptr<IndexCatalog::IndexIterator> ii =
            autoIncColl.getCollection()->getIndexCatalog()->getIndexIterator(_opCtx, true);
        // Iterate over incColl's indexes.
        while (ii->more()) {
            const IndexDescriptor* currIndex = ii->next()->descriptor();
            BSONObj x = currIndex->infoObj();
            if (sortKey.woCompare(x["key"].embeddedObject()) == 0) {
                foundIndex = true;
                break;
            }
        }

        invariant(foundIndex);
    }

    boost::optional<AutoGetCollectionForReadCommand> ctx;
    ctx.emplace(_opCtx, _config.incLong);
    assertCollectionNotNull(_config.incLong, *ctx);

    BSONObj prev;
    BSONList all;

    const auto count = _db.count(_config.incLong.ns(), BSONObj(), QueryOption_SlaveOk);
    ProgressMeterHolder pm;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        pm.set(curOp->setProgress_inlock("M/R: (3/3) Final Reduce", count));
    }

    const ExtensionsCallbackReal extensionsCallback(_opCtx, &_config.incLong);

    auto qr = std::make_unique<QueryRequest>(_config.incLong);
    qr->setSort(sortKey);

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(qr),
                                     expCtx,
                                     extensionsCallback,
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    verify(statusWithCQ.isOK());
    std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    {
        auto exec = uassertStatusOK(getExecutor(_opCtx,
                                                ctx->getCollection(),
                                                std::move(cq),
                                                PlanExecutor::YIELD_AUTO,
                                                QueryPlannerParams::NO_TABLE_SCAN));

        // iterate over all sorted objects
        BSONObj o;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&o, nullptr))) {
            o = o.getOwned();  // we will be accessing outside of the lock
            pm.hit();

            if (dps::compareObjectsAccordingToSort(o, prev, sortKey) == 0) {
                // object is same as previous, add to array
                all.push_back(o);
                if (pm->hits() % 100 == 0) {
                    _opCtx->checkForInterrupt();
                }
                continue;
            }

            exec->saveState();

            ctx.reset();

            // reduce a finalize array
            finalReduce(all);
            ctx.emplace(_opCtx, _config.incLong);

            all.clear();
            prev = o;
            all.push_back(o);

            _opCtx->checkForInterrupt();
            exec->restoreState();
        }

        uassert(34428,
                "Plan executor error during mapReduce command: " +
                    WorkingSetCommon::toStatusString(o),
                PlanExecutor::IS_EOF == state);
    }
    ctx.reset();

    // reduce and finalize last array
    finalReduce(all);
    ctx.emplace(_opCtx, _config.incLong);

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
    invariant(!_opCtx->lockState()->isLocked());

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
            _scope->invoke(_reduceAll, nullptr, nullptr, 0, true);
            LOG(3) << "  MR - did reduceAll: keys=" << keyCt << " dups=" << dupCt
                   << " newKeys=" << _scope->getNumberInt("_keyCt") << " time=" << t.millis()
                   << "ms";
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
               << " newSize=" << _size << " time=" << t.millis() << "ms";

        // if size is still high, or values are not reducing well, dump
        if (_onDisk && (_size > _config.maxInMemSize || _size > oldSize / 2)) {
            dumpToInc();
            LOG(3) << "  MR - dumping to db";
        }
    }
}


bool runMapReduce(OperationContext* opCtx,
                  const string& dbname,
                  const BSONObj& cmd,
                  string& errmsg,
                  BSONObjBuilder& result) {
    Timer t;

    boost::optional<DisableDocumentValidation> maybeDisableValidation;
    if (shouldBypassDocumentValidationForCommand(cmd))
        maybeDisableValidation.emplace(opCtx);

    auto client = opCtx->getClient();

    if (client->isInDirectClient()) {
        uasserted(ErrorCodes::IllegalOperation, "Cannot run mapReduce command from eval()");
    }

    auto curOp = CurOp::get(opCtx);

    const Config config(dbname, cmd);

    LOG(1) << "mr ns: " << config.nss;

    uassert(16149, "cannot run map reduce without the js engine", getGlobalScriptEngine());

    const auto metadata = [&] {
        AutoGetCollectionForReadCommand autoColl(opCtx, config.nss);
        return CollectionShardingState::get(opCtx, config.nss)
            ->getOrphansFilter(opCtx, autoColl.getCollection());
    }();

    bool shouldHaveData = false;

    BSONObjBuilder countsBuilder;
    BSONObjBuilder timingBuilder;
    try {
        State state(opCtx, config);
        if (!state.sourceExists()) {
            uasserted(ErrorCodes::NamespaceNotFound,
                      str::stream() << "namespace does not exist: " << config.nss);
        }

        state.init();
        state.prepTempCollection();

        int64_t progressTotal = 0;
        bool showTotal = true;
        if (state.config().filter.isEmpty()) {
            const bool holdingGlobalLock = false;
            const auto count = collectionCount(opCtx, config.nss, holdingGlobalLock);
            progressTotal = (config.limit && static_cast<unsigned long long>(config.limit) < count)
                ? config.limit
                : count;
        } else {
            showTotal = false;
            // Set an arbitrary total > 0 so the meter will be activated.
            progressTotal = 1;
        }

        ProgressMeterHolder pm;
        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            pm.set(curOp->setProgress_inlock("M/R: (1/3) Emit Phase", progressTotal));
        }
        pm->showTotal(showTotal);

        long long mapTime = 0;
        long long reduceTime = 0;
        long long numInputs = 0;

        {
            // We've got a cursor preventing migrations off, now re-establish our useful cursor

            // Need lock and context to use it
            boost::optional<AutoGetCollection> scopedAutoColl;
            scopedAutoColl.emplace(opCtx, config.nss, MODE_S);
            assertCollectionNotNull(config.nss, *scopedAutoColl);

            if (state.isOnDisk()) {
                // This means that it will be doing a write operation, make sure it is safe to
                // do so
                uassert(ErrorCodes::NotMaster,
                        "not master",
                        repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx,
                                                                                     config.nss));
            }

            auto qr = std::make_unique<QueryRequest>(config.nss);
            qr->setFilter(config.filter);
            qr->setSort(config.sort);
            qr->setCollation(config.collation);

            const ExtensionsCallbackReal extensionsCallback(opCtx, &config.nss);

            const boost::intrusive_ptr<ExpressionContext> expCtx;
            auto cq = uassertStatusOKWithContext(
                CanonicalQuery::canonicalize(opCtx,
                                             std::move(qr),
                                             expCtx,
                                             extensionsCallback,
                                             MatchExpressionParser::kAllowAllSpecialFeatures),
                str::stream() << "Can't canonicalize query " << config.filter);

            auto exec = uassertStatusOK(getExecutor(opCtx,
                                                    scopedAutoColl->getCollection(),
                                                    std::move(cq),
                                                    PlanExecutor::YIELD_AUTO,
                                                    0));

            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
            }

            Timer mt;

            BSONObj o;
            PlanExecutor::ExecState execState;
            while (PlanExecutor::ADVANCED == (execState = exec->getNext(&o, nullptr))) {
                o = o.getOwned();  // The object will be accessed outside of collection lock

                // Check to see if this is a new object we don't own yet because of a chunk
                // migration
                if (metadata->isSharded()) {
                    ShardKeyPattern kp(metadata->getKeyPattern());
                    if (!metadata->keyBelongsToMe(kp.extractShardKeyFromDoc(o))) {
                        continue;
                    }
                }

                if (config.verbose)
                    mt.reset();

                config.mapper->map(o);

                if (config.verbose)
                    mapTime += mt.micros();

                // Check if the state accumulated so far needs to be written to a collection.
                // This may yield the DB lock temporarily and then acquire it again.
                numInputs++;
                if (numInputs % 100 == 0) {
                    Timer t;

                    // TODO: As an optimization, we might want to do the save/restore state and
                    // yield inside the reduceAndSpillInMemoryState method, so it only happens
                    // if necessary.
                    exec->saveState();

                    scopedAutoColl.reset();

                    state.reduceAndSpillInMemoryStateIfNeeded();
                    scopedAutoColl.emplace(opCtx, config.nss, MODE_S);

                    exec->restoreState();

                    reduceTime += t.micros();

                    opCtx->checkForInterrupt();
                }

                pm.hit();

                if (config.limit && numInputs >= config.limit)
                    break;
            }

            if (PlanExecutor::FAILURE == execState) {
                uasserted(ErrorCodes::OperationFailed,
                          str::stream() << "Executor error during mapReduce command: "
                                        << WorkingSetCommon::toStatusString(o));
            }

            // Record the indexes used by the PlanExecutor.
            PlanSummaryStats stats;
            Explain::getSummaryStats(*exec, &stats);

            // TODO SERVER-23261: Confirm whether this is the correct place to gather all
            // metrics. There is no harm adding here for the time being.
            curOp->debug().setPlanSummaryMetrics(stats);
            CollectionQueryInfo::get(scopedAutoColl->getCollection()).notifyOfQuery(opCtx, stats);

            if (curOp->shouldDBProfile()) {
                BSONObjBuilder execStatsBob;
                Explain::getWinningPlanStats(exec.get(), &execStatsBob);
                curOp->debug().execStats = execStatsBob.obj();
            }
        }
        pm.finished();

        opCtx->checkForInterrupt();

        // update counters
        countsBuilder.appendNumber("input", numInputs);
        countsBuilder.appendNumber("emit", state.numEmits());
        if (state.numEmits())
            shouldHaveData = true;

        timingBuilder.appendNumber("mapTime", mapTime / 1000);
        timingBuilder.append("emitLoop", t.millis());

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setMessage_inlock("M/R: (2/3) Final In-Memory Reduce");
        }
        Timer rt;
        // do reduce in memory
        // this will be the last reduce needed for inline mode
        state.reduceInMemory();
        // if not inline: dump the in memory map to inc collection, all data is on disk
        state.dumpToInc();
        // final reduce
        state.finalReduce(opCtx, curOp);
        reduceTime += rt.micros();

        // Ensure the profile shows the source namespace. If the output was not inline, the
        // active namespace will be the temporary collection we inserted into.
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setNS_inlock(config.nss.ns());
        }

        countsBuilder.appendNumber("reduce", state.numReduces());
        timingBuilder.appendNumber("reduceTime", reduceTime / 1000);
        timingBuilder.append("mode", state.jsMode() ? "js" : "mixed");

        long long finalCount = state.postProcessCollection(opCtx, curOp);
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
    } catch (StaleConfigException& e) {
        log() << "mr detected stale config, should retry" << redact(e);
        throw;
    }
    // TODO:  The error handling code for queries is v. fragile,
    // *requires* rethrow AssertionExceptions - should probably fix.
    catch (AssertionException& e) {
        log() << "mr failed, removing collection" << redact(e);
        throw;
    } catch (std::exception& e) {
        log() << "mr failed, removing collection" << causedBy(e);
        throw;
    } catch (...) {
        log() << "mr failed for unknown reason, removing collection";
        throw;
    }

    return true;
}


bool runMapReduceShardedFinish(OperationContext* opCtx,
                               const string& dbname,
                               const BSONObj& cmdObj,
                               BSONObjBuilder& result) {
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        uasserted(ErrorCodes::CommandNotSupported,
                  str::stream() << "Can not execute mapReduce with output database " << dbname
                                << " which lives on config servers");
    }

    boost::optional<DisableDocumentValidation> maybeDisableValidation;
    if (shouldBypassDocumentValidationForCommand(cmdObj))
        maybeDisableValidation.emplace(opCtx);

    // legacy name
    const auto shardedOutputCollectionElt = cmdObj["shardedOutputCollection"];
    uassert(ErrorCodes::InvalidNamespace,
            "'shardedOutputCollection' must be of type String",
            shardedOutputCollectionElt.type() == BSONType::String);
    const std::string shardedOutputCollection = shardedOutputCollectionElt.str();
    verify(shardedOutputCollection.size() > 0);

    std::string inputNS;
    if (cmdObj["inputDB"].type() == String) {
        inputNS =
            NamespaceString(cmdObj["inputDB"].valueStringData(), shardedOutputCollection).ns();
    } else {
        inputNS = NamespaceString(dbname, shardedOutputCollection).ns();
    }

    CurOp* curOp = CurOp::get(opCtx);

    Config config(dbname, cmdObj.firstElement().embeddedObjectUserCheck());

    if (cmdObj.hasField("shardedOutputCollUUID")) {
        config.finalOutputCollUUID = uassertStatusOK(UUID::parse(cmdObj["shardedOutputCollUUID"]));
    }

    State state(opCtx, config);
    state.init();

    // no need for incremental collection because records are already sorted
    state._useIncremental = false;
    config.incLong = config.tempNamespace;

    BSONObj shardCounts = cmdObj["shardCounts"].embeddedObjectUserCheck();
    BSONObj counts = cmdObj["counts"].embeddedObjectUserCheck();

    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        curOp->setMessage_inlock("M/R Merge Sort and Reduce");
    }
    set<string> servers;

    {
        // Parse per shard results
        BSONObjIterator i(shardCounts);
        while (i.more()) {
            BSONElement e = i.next();
            std::string server = e.fieldName();
            servers.insert(server);

            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, server));
        }
    }

    state.prepTempCollection();

    std::vector<Chunk> chunks;

    if (config.outputOptions.outType != mr::OutputType::kInMemory) {
        auto outRoutingInfoStatus = Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
            opCtx, config.outputOptions.finalNamespace);
        uassertStatusOK(outRoutingInfoStatus.getStatus());

        if (auto cm = outRoutingInfoStatus.getValue().cm()) {
            // Fetch result from other shards 1 chunk at a time. It would be better to do just
            // one big $or query, but then the sorting would not be efficient.
            const auto shardId = ShardingState::get(opCtx)->shardId();

            for (const auto& chunk : cm->chunks()) {
                if (chunk.getShardId() == shardId) {
                    chunks.push_back(chunk);
                }
            }
        }
    }

    long long inputCount = 0;
    unsigned int index = 0;
    BSONObj query;
    BSONList values;

    while (true) {
        if (chunks.size() > 0) {
            const auto& chunk = chunks[index];
            BSONObjBuilder b;
            b.appendAs(chunk.getMin().firstElement(), "$gte");
            b.appendAs(chunk.getMax().firstElement(), "$lt");
            query = BSON("_id" << b.obj());
        }

        // reduce from each shard for a chunk
        BSONObj sortKey = BSON("_id" << 1);
        ParallelSortClusteredCursor cursor(
            servers, inputNS, Query(query).sort(sortKey), QueryOption_NoCursorTimeout);
        cursor.init(opCtx);

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
            if (state.isOnDisk())
                state.insert(config.tempNamespace, res);
            else
                state.emit(res);

            values.clear();

            if (!t.isEmpty())
                values.push_back(t);
        }

        if (++index >= chunks.size())
            break;
    }

    // Forget temporary input collection, if output is sharded collection
    ShardConnection::forgetNS(inputNS);

    long long outputCount = state.postProcessCollection(opCtx, curOp);
    state.appendResults(result);

    BSONObjBuilder countsB(32);
    countsB.append("input", inputCount);
    countsB.append("reduce", state.numReduces());
    countsB.append("output", outputCount);
    result.append("counts", countsB.obj());

    return true;
}

}  // namespace mr
}  // namespace mongo
