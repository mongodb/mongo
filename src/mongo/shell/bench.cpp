/** @file bench.cpp */

/*
 *    Copyright (C) 2010 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/shell/bench.h"

#include <pcrecpp.h>
#include <iostream>

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/scripting/bson_template_evaluator.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

// ---------------------------------
// ---- benchmarking system --------
// ---------------------------------

// TODO:  Maybe extract as library to avoid code duplication?
namespace {
inline pcrecpp::RE_Options flags2options(const char* flags) {
    pcrecpp::RE_Options options;
    options.set_utf8(true);
    while (flags && *flags) {
        if (*flags == 'i')
            options.set_caseless(true);
        else if (*flags == 'm')
            options.set_multiline(true);
        else if (*flags == 'x')
            options.set_extended(true);
        flags++;
    }
    return options;
}
}

namespace mongo {

using std::unique_ptr;
using std::cout;
using std::endl;
using std::map;

BenchRunEventCounter::BenchRunEventCounter() {
    reset();
}

BenchRunEventCounter::~BenchRunEventCounter() {}

void BenchRunEventCounter::reset() {
    _numEvents = 0;
    _totalTimeMicros = 0;
}

void BenchRunEventCounter::updateFrom(const BenchRunEventCounter& other) {
    _numEvents += other._numEvents;
    _totalTimeMicros += other._totalTimeMicros;
}

BenchRunStats::BenchRunStats() {
    reset();
}

BenchRunStats::~BenchRunStats() {}

void BenchRunStats::reset() {
    error = false;
    errCount = 0;
    opCount = 0;

    findOneCounter.reset();
    updateCounter.reset();
    insertCounter.reset();
    deleteCounter.reset();
    queryCounter.reset();
    commandCounter.reset();
    trappedErrors.clear();
}

void BenchRunStats::updateFrom(const BenchRunStats& other) {
    if (other.error)
        error = true;
    errCount += other.errCount;
    opCount += other.opCount;

    findOneCounter.updateFrom(other.findOneCounter);
    updateCounter.updateFrom(other.updateCounter);
    insertCounter.updateFrom(other.insertCounter);
    deleteCounter.updateFrom(other.deleteCounter);
    queryCounter.updateFrom(other.queryCounter);
    commandCounter.updateFrom(other.commandCounter);

    for (size_t i = 0; i < other.trappedErrors.size(); ++i)
        trappedErrors.push_back(other.trappedErrors[i]);
}

BenchRunConfig::BenchRunConfig() {
    initializeToDefaults();
}

void BenchRunConfig::initializeToDefaults() {
    host = "localhost";
    db = "test";
    username = "";
    password = "";

    parallel = 1;
    seconds = 1.0;
    hideResults = true;
    handleErrors = false;
    hideErrors = false;

    trapPattern.reset();
    noTrapPattern.reset();
    watchPattern.reset();
    noWatchPattern.reset();

    ops = BSONObj();

    throwGLE = false;
    breakOnTrap = true;
    randomSeed = 1314159265358979323;
}

BenchRunConfig* BenchRunConfig::createFromBson(const BSONObj& args) {
    BenchRunConfig* config = new BenchRunConfig();
    config->initializeFromBson(args);
    return config;
}

void BenchRunConfig::initializeFromBson(const BSONObj& args) {
    initializeToDefaults();

    if (args["host"].type() == String)
        this->host = args["host"].String();
    if (args["db"].type() == String)
        this->db = args["db"].String();
    if (args["username"].type() == String)
        this->username = args["username"].String();
    if (args["password"].type() == String)
        this->password = args["password"].String();

    if (args["parallel"].isNumber())
        this->parallel = args["parallel"].numberInt();
    if (args["randomSeed"].isNumber())
        this->randomSeed = args["randomSeed"].numberInt();
    if (args["seconds"].isNumber())
        this->seconds = args["seconds"].number();
    if (!args["hideResults"].eoo())
        this->hideResults = args["hideResults"].trueValue();
    if (!args["handleErrors"].eoo())
        this->handleErrors = args["handleErrors"].trueValue();
    if (!args["hideErrors"].eoo())
        this->hideErrors = args["hideErrors"].trueValue();
    if (!args["throwGLE"].eoo())
        this->throwGLE = args["throwGLE"].trueValue();
    if (!args["breakOnTrap"].eoo())
        this->breakOnTrap = args["breakOnTrap"].trueValue();

    uassert(16164, "loopCommands config not supported", args["loopCommands"].eoo());

    if (!args["trapPattern"].eoo()) {
        const char* regex = args["trapPattern"].regex();
        const char* flags = args["trapPattern"].regexFlags();
        this->trapPattern =
            std::shared_ptr<pcrecpp::RE>(new pcrecpp::RE(regex, flags2options(flags)));
    }

    if (!args["noTrapPattern"].eoo()) {
        const char* regex = args["noTrapPattern"].regex();
        const char* flags = args["noTrapPattern"].regexFlags();
        this->noTrapPattern =
            std::shared_ptr<pcrecpp::RE>(new pcrecpp::RE(regex, flags2options(flags)));
    }

    if (!args["watchPattern"].eoo()) {
        const char* regex = args["watchPattern"].regex();
        const char* flags = args["watchPattern"].regexFlags();
        this->watchPattern =
            std::shared_ptr<pcrecpp::RE>(new pcrecpp::RE(regex, flags2options(flags)));
    }

    if (!args["noWatchPattern"].eoo()) {
        const char* regex = args["noWatchPattern"].regex();
        const char* flags = args["noWatchPattern"].regexFlags();
        this->noWatchPattern =
            std::shared_ptr<pcrecpp::RE>(new pcrecpp::RE(regex, flags2options(flags)));
    }

    this->ops = args["ops"].Obj().getOwned();
}

DBClientBase* BenchRunConfig::createConnection() const {
    const ConnectionString connectionString = uassertStatusOK(ConnectionString::parse(host));

    std::string errorMessage;
    DBClientBase* connection = connectionString.connect(errorMessage);
    uassert(16158, errorMessage, connection != NULL);

    return connection;
}

BenchRunState::BenchRunState(unsigned numWorkers)
    : _mutex(),
      _numUnstartedWorkers(numWorkers),
      _numActiveWorkers(0),
      _isShuttingDown(0),
      _isCollectingStats(0) {}

BenchRunState::~BenchRunState() {
    wassert(_numActiveWorkers == 0 && _numUnstartedWorkers == 0);
}

void BenchRunState::waitForState(State awaitedState) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    switch (awaitedState) {
        case BRS_RUNNING:
            while (_numUnstartedWorkers > 0) {
                massert(16147, "Already finished.", _numUnstartedWorkers + _numActiveWorkers > 0);
                _stateChangeCondition.wait(lk);
            }
            break;
        case BRS_FINISHED:
            while (_numUnstartedWorkers + _numActiveWorkers > 0) {
                _stateChangeCondition.wait(lk);
            }
            break;
        default:
            msgasserted(16152,
                        mongoutils::str::stream() << "Cannot wait for state " << awaitedState);
    }
}

void BenchRunState::tellWorkersToFinish() {
    _isShuttingDown.store(1);
}

void BenchRunState::tellWorkersToCollectStats() {
    _isCollectingStats.store(1);
}

void BenchRunState::assertFinished() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    verify(0 == _numUnstartedWorkers + _numActiveWorkers);
}

bool BenchRunState::shouldWorkerFinish() {
    return (_isShuttingDown.loadRelaxed() == 1);
}

bool BenchRunState::shouldWorkerCollectStats() {
    return (_isCollectingStats.loadRelaxed() == 1);
}

void BenchRunState::onWorkerStarted() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    verify(_numUnstartedWorkers > 0);
    --_numUnstartedWorkers;
    ++_numActiveWorkers;
    if (_numUnstartedWorkers == 0) {
        _stateChangeCondition.notify_all();
    }
}

void BenchRunState::onWorkerFinished() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    verify(_numActiveWorkers > 0);
    --_numActiveWorkers;
    if (_numActiveWorkers + _numUnstartedWorkers == 0) {
        _stateChangeCondition.notify_all();
    }
}

BSONObj benchStart(const BSONObj&, void*);
BSONObj benchFinish(const BSONObj&, void*);

static bool _hasSpecial(const BSONObj& obj) {
    BSONObjIterator i(obj);
    while (i.more()) {
        BSONElement e = i.next();
        if (e.fieldName()[0] == '#')
            return true;

        if (!e.isABSONObj())
            continue;

        if (_hasSpecial(e.Obj()))
            return true;
    }
    return false;
}

static BSONObj fixQuery(const BSONObj& obj, BsonTemplateEvaluator& btl) {
    if (!_hasSpecial(obj))
        return obj;
    BSONObjBuilder b(obj.objsize() + 128);

    verify(BsonTemplateEvaluator::StatusSuccess == btl.evaluate(obj, b));
    return b.obj();
}

BenchRunWorker::BenchRunWorker(size_t id,
                               const BenchRunConfig* config,
                               BenchRunState* brState,
                               int64_t randomSeed)
    : _id(id), _config(config), _brState(brState), _randomSeed(randomSeed) {}

BenchRunWorker::~BenchRunWorker() {}

void BenchRunWorker::start() {
    stdx::thread(stdx::bind(&BenchRunWorker::run, this)).detach();
}

bool BenchRunWorker::shouldStop() const {
    return _brState->shouldWorkerFinish();
}

bool BenchRunWorker::shouldCollectStats() const {
    return _brState->shouldWorkerCollectStats();
}

void doNothing(const BSONObj&) {}

/**
 * Issues the query 'lpq' against 'conn' using read commands.  Returns the size of the result set
 * returned by the query.
 *
 * If 'lpq' has the 'wantMore' flag set to false and the 'limit' option set to 1LL, then the caller
 * may optionally specify a pointer to an object in 'objOut', which will be filled in with the
 * single object in the query result set (or the empty object, if the result set is empty).
 * If 'lpq' doesn't have these options set, then nullptr must be passed for 'objOut'.
 *
 * On error, throws a UserException.
 */
int runQueryWithReadCommands(DBClientBase* conn,
                             unique_ptr<LiteParsedQuery> lpq,
                             BSONObj* objOut = nullptr) {
    std::string dbName = lpq->nss().db().toString();
    BSONObj findCommandResult;
    bool res = conn->runCommand(dbName, lpq->asFindCommand(), findCommandResult);
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "find command failed; reply was: " << findCommandResult,
            res);

    CursorResponse cursorResponse =
        uassertStatusOK(CursorResponse::parseFromBSON(findCommandResult));
    int count = cursorResponse.getBatch().size();

    if (objOut) {
        invariant(lpq->getLimit() && *lpq->getLimit() == 1 && !lpq->wantMore());
        // Since this is a "single batch" query, we can simply grab the first item in the result set
        // and return here.
        *objOut = (count > 0) ? cursorResponse.getBatch()[0] : BSONObj();
        return count;
    }

    while (cursorResponse.getCursorId() != 0) {
        GetMoreRequest getMoreRequest(lpq->nss(),
                                      cursorResponse.getCursorId(),
                                      lpq->getBatchSize(),
                                      boost::none,   // maxTimeMS
                                      boost::none,   // term
                                      boost::none);  // lastKnownCommittedOpTime
        BSONObj getMoreCommandResult;
        res = conn->runCommand(dbName, getMoreRequest.toBSON(), getMoreCommandResult);
        uassert(ErrorCodes::CommandFailed,
                str::stream() << "getMore command failed; reply was: " << getMoreCommandResult,
                res);
        cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(getMoreCommandResult));
        count += cursorResponse.getBatch().size();
    }

    return count;
}

void BenchRunWorker::generateLoadOnConnection(DBClientBase* conn) {
    verify(conn);
    long long count = 0;
    mongo::Timer timer;

    BsonTemplateEvaluator bsonTemplateEvaluator(_randomSeed);
    invariant(bsonTemplateEvaluator.setId(_id) == BsonTemplateEvaluator::StatusSuccess);

    if (_config->username != "") {
        string errmsg;
        if (!conn->auth("admin", _config->username, _config->password, errmsg)) {
            uasserted(15931, "Authenticating to connection for _benchThread failed: " + errmsg);
        }
    }

    while (!shouldStop()) {
        BSONObjIterator i(_config->ops);
        while (i.more()) {
            if (shouldStop())
                break;
            auto& stats = shouldCollectStats() ? _stats : _statsBlackHole;
            BSONElement e = i.next();

            string ns = e["ns"].String();
            string op = e["op"].String();

            int delay = e["delay"].eoo() ? 0 : e["delay"].Int();

            bool useWriteCmd = false;  // By default, don't use write commands.
            if (e["writeCmd"]) {
                useWriteCmd = e["writeCmd"].Bool();
            }
            bool useReadCmd = false;  // By default, don't use read commands.
            if (e["readCmd"]) {
                useReadCmd = e["readCmd"].Bool();
            }

            BSONObj context = e["context"].eoo() ? BSONObj() : e["context"].Obj();

            unique_ptr<Scope> scope;
            ScriptingFunction scopeFunc = 0;
            BSONObj scopeObj;

            bool check = !e["check"].eoo();
            if (check) {
                if (e["check"].type() == CodeWScope || e["check"].type() == Code ||
                    e["check"].type() == String) {
                    scope = globalScriptEngine->getPooledScope(NULL, ns, "benchrun");
                    verify(scope.get());

                    if (e.type() == CodeWScope) {
                        scopeFunc = scope->createFunction(e["check"].codeWScopeCode());
                        scopeObj = BSONObj(e.codeWScopeScopeDataUnsafe());
                    } else {
                        scopeFunc = scope->createFunction(e["check"].valuestr());
                    }

                    scope->init(&scopeObj);
                    invariant(scopeFunc);
                } else {
                    warning() << "Invalid check type detected in benchRun op : " << e << endl;
                    check = false;
                }
            }

            try {
                if (op == "nop") {
                    // do nothing
                } else if (op == "findOne") {
                    BSONObj fixedQuery = fixQuery(e["query"].Obj(), bsonTemplateEvaluator);
                    BSONObj result;
                    if (useReadCmd) {
                        unique_ptr<LiteParsedQuery> lpq =
                            LiteParsedQuery::makeAsFindCmd(NamespaceString(ns),
                                                           fixedQuery,
                                                           BSONObj(),    // projection
                                                           BSONObj(),    // sort
                                                           BSONObj(),    // hint
                                                           BSONObj(),    // readConcern
                                                           boost::none,  // skip
                                                           1LL,          // limit
                                                           boost::none,  // batchSize
                                                           boost::none,  // ntoreturn
                                                           false);       // wantMore
                        BenchRunEventTrace _bret(&stats.findOneCounter);
                        runQueryWithReadCommands(conn, std::move(lpq), &result);
                    } else {
                        BenchRunEventTrace _bret(&stats.findOneCounter);
                        result = conn->findOne(ns, fixedQuery);
                    }

                    if (check) {
                        int err = scope->invoke(scopeFunc, 0, &result, 1000 * 60, false);
                        if (err) {
                            log() << "Error checking in benchRun thread [findOne]"
                                  << causedBy(scope->getError()) << endl;

                            stats.errCount++;

                            return;
                        }
                    }

                    if (!_config->hideResults || e["showResult"].trueValue())
                        log() << "Result from benchRun thread [findOne] : " << result << endl;

                } else if (op == "command") {
                    bool ok;
                    BSONObj result;
                    {
                        BenchRunEventTrace _bret(&stats.commandCounter);
                        ok = conn->runCommand(ns,
                                              fixQuery(e["command"].Obj(), bsonTemplateEvaluator),
                                              result,
                                              e["options"].numberInt());
                    }
                    if (!ok) {
                        stats.errCount++;
                    } else if (check) {
                        int err = scope->invoke(scopeFunc, 0, &result, 1000 * 60, false);
                        if (err) {
                            log() << "Error checking in benchRun thread [command]"
                                  << causedBy(scope->getError()) << endl;

                            stats.errCount++;

                            return;
                        }
                    }

                    if (!_config->hideResults || e["showResult"].trueValue())
                        log() << "Result from benchRun thread [command] : " << result << endl;

                } else if (op == "find" || op == "query") {
                    int limit = e["limit"].eoo() ? 0 : e["limit"].numberInt();
                    int skip = e["skip"].eoo() ? 0 : e["skip"].Int();
                    int options = e["options"].eoo() ? 0 : e["options"].Int();
                    int batchSize = e["batchSize"].eoo() ? 0 : e["batchSize"].Int();
                    int expected = e["expected"].eoo() ? -1 : e["expected"].Int();

                    // TODO: The following option should be named "projection".  The work to rename
                    // it is being tracked at SERVER-21013.
                    BSONObj projection = e["filter"].eoo() ? BSONObj() : e["filter"].Obj();

                    int count;

                    BSONObj fixedQuery = fixQuery(e["query"].Obj(), bsonTemplateEvaluator);

                    if (useReadCmd) {
                        uassert(28824,
                                "cannot use 'options' in combination with read commands",
                                !options);
                        unique_ptr<LiteParsedQuery> lpq = LiteParsedQuery::makeAsFindCmd(
                            NamespaceString(ns),
                            fixedQuery,
                            projection,
                            BSONObj(),  // sort
                            BSONObj(),  // hint
                            BSONObj(),  // readConcern
                            skip ? boost::optional<long long>(skip) : boost::none,
                            limit ? boost::optional<long long>(limit) : boost::none,
                            batchSize ? boost::optional<long long>(batchSize) : boost::none);
                        BenchRunEventTrace _bret(&stats.queryCounter);
                        count = runQueryWithReadCommands(conn, std::move(lpq));
                    } else {
                        // Use special query function for exhaust query option.
                        if (options & QueryOption_Exhaust) {
                            BenchRunEventTrace _bret(&stats.queryCounter);
                            stdx::function<void(const BSONObj&)> castedDoNothing(doNothing);
                            count =
                                conn->query(castedDoNothing, ns, fixedQuery, &projection, options);
                        } else {
                            BenchRunEventTrace _bret(&stats.queryCounter);
                            unique_ptr<DBClientCursor> cursor;
                            cursor = conn->query(
                                ns, fixedQuery, limit, skip, &projection, options, batchSize);
                            count = cursor->itcount();
                        }
                    }

                    if (expected >= 0 && count != expected) {
                        cout << "bench query on: " << ns << " expected: " << expected
                             << " got: " << count << endl;
                        verify(false);
                    }

                    if (check) {
                        BSONObj thisValue = BSON("count" << count << "context" << context);
                        int err = scope->invoke(scopeFunc, 0, &thisValue, 1000 * 60, false);
                        if (err) {
                            log() << "Error checking in benchRun thread [find]"
                                  << causedBy(scope->getError()) << endl;

                            stats.errCount++;

                            return;
                        }
                    }

                    if (!_config->hideResults || e["showResult"].trueValue())
                        log() << "Result from benchRun thread [query] : " << count << endl;

                } else if (op == "update") {
                    bool multi = e["multi"].trueValue();
                    bool upsert = e["upsert"].trueValue();
                    BSONObj queryOrginal = e["query"].eoo() ? BSONObj() : e["query"].Obj();
                    BSONObj updateOriginal = e["update"].Obj();
                    BSONObj result;
                    bool safe = e["safe"].trueValue();

                    {
                        BenchRunEventTrace _bret(&stats.updateCounter);
                        BSONObj query = fixQuery(queryOrginal, bsonTemplateEvaluator);
                        BSONObj update = fixQuery(updateOriginal, bsonTemplateEvaluator);

                        if (useWriteCmd) {
                            // TODO: Replace after SERVER-11774.
                            BSONObjBuilder builder;
                            builder.append("update", nsToCollectionSubstring(ns));
                            BSONArrayBuilder docBuilder(builder.subarrayStart("updates"));
                            docBuilder.append(BSON("q" << query << "u" << update << "multi" << multi
                                                       << "upsert" << upsert));
                            docBuilder.done();
                            conn->runCommand(
                                nsToDatabaseSubstring(ns).toString(), builder.done(), result);
                        } else {
                            conn->update(ns, query, update, upsert, multi);
                            if (safe)
                                result = conn->getLastErrorDetailed();
                        }
                    }

                    if (safe) {
                        if (check) {
                            int err = scope->invoke(scopeFunc, 0, &result, 1000 * 60, false);
                            if (err) {
                                log() << "Error checking in benchRun thread [update]"
                                      << causedBy(scope->getError()) << endl;

                                stats.errCount++;

                                return;
                            }
                        }

                        if (!_config->hideResults || e["showResult"].trueValue())
                            log() << "Result from benchRun thread [safe update] : " << result
                                  << endl;

                        if (!result["err"].eoo() && result["err"].type() == String &&
                            (_config->throwGLE || e["throwGLE"].trueValue()))
                            throw DBException((string) "From benchRun GLE" +
                                                  causedBy(result["err"].String()),
                                              result["code"].eoo() ? 0 : result["code"].Int());
                    }
                } else if (op == "insert") {
                    bool safe = e["safe"].trueValue();
                    BSONObj result;

                    {
                        BenchRunEventTrace _bret(&stats.insertCounter);

                        BSONObj insertDoc;
                        if (useWriteCmd) {
                            BSONObjBuilder builder;
                            builder.append("insert", nsToCollectionSubstring(ns));
                            BSONArrayBuilder docBuilder(builder.subarrayStart("documents"));
                            if (e["doc"].type() == Array) {
                                for (auto& element : e["doc"].Array()) {
                                    insertDoc = fixQuery(element.Obj(), bsonTemplateEvaluator);
                                    docBuilder.append(insertDoc);
                                }
                            } else {
                                insertDoc = fixQuery(e["doc"].Obj(), bsonTemplateEvaluator);
                                docBuilder.append(insertDoc);
                            }
                            docBuilder.done();
                            // TODO: Replace after SERVER-11774.
                            conn->runCommand(
                                nsToDatabaseSubstring(ns).toString(), builder.done(), result);
                        } else {
                            if (e["doc"].type() == Array) {
                                std::vector<BSONObj> insertArray;
                                for (auto& element : e["doc"].Array()) {
                                    BSONObj e = fixQuery(element.Obj(), bsonTemplateEvaluator);
                                    insertArray.push_back(e);
                                }
                                conn->insert(ns, insertArray);
                            } else {
                                insertDoc = fixQuery(e["doc"].Obj(), bsonTemplateEvaluator);
                                conn->insert(ns, insertDoc);
                            }
                            if (safe)
                                result = conn->getLastErrorDetailed();
                        }
                    }

                    if (safe) {
                        if (check) {
                            int err = scope->invoke(scopeFunc, 0, &result, 1000 * 60, false);
                            if (err) {
                                log() << "Error checking in benchRun thread [insert]"
                                      << causedBy(scope->getError()) << endl;

                                stats.errCount++;

                                return;
                            }
                        }

                        if (!_config->hideResults || e["showResult"].trueValue())
                            log() << "Result from benchRun thread [safe insert] : " << result
                                  << endl;

                        if (!result["err"].eoo() && result["err"].type() == String &&
                            (_config->throwGLE || e["throwGLE"].trueValue()))
                            throw DBException((string) "From benchRun GLE" +
                                                  causedBy(result["err"].String()),
                                              result["code"].eoo() ? 0 : result["code"].Int());
                    }
                } else if (op == "delete" || op == "remove") {
                    bool multi = e["multi"].eoo() ? true : e["multi"].trueValue();
                    BSONObj query = e["query"].eoo() ? BSONObj() : e["query"].Obj();
                    bool safe = e["safe"].trueValue();
                    BSONObj result;
                    {
                        BenchRunEventTrace _bret(&stats.deleteCounter);
                        BSONObj predicate = fixQuery(query, bsonTemplateEvaluator);
                        if (useWriteCmd) {
                            // TODO: Replace after SERVER-11774.
                            BSONObjBuilder builder;
                            builder.append("delete", nsToCollectionSubstring(ns));
                            BSONArrayBuilder docBuilder(builder.subarrayStart("deletes"));
                            int limit = (multi == true) ? 0 : 1;
                            docBuilder.append(BSON("q" << predicate << "limit" << limit));
                            docBuilder.done();
                            conn->runCommand(
                                nsToDatabaseSubstring(ns).toString(), builder.done(), result);
                        } else {
                            conn->remove(ns, predicate, !multi);
                            if (safe)
                                result = conn->getLastErrorDetailed();
                        }
                    }

                    if (safe) {
                        if (check) {
                            int err = scope->invoke(scopeFunc, 0, &result, 1000 * 60, false);
                            if (err) {
                                log() << "Error checking in benchRun thread [delete]"
                                      << causedBy(scope->getError()) << endl;

                                stats.errCount++;

                                return;
                            }
                        }

                        if (!_config->hideResults || e["showResult"].trueValue())
                            log() << "Result from benchRun thread [safe remove] : " << result
                                  << endl;

                        if (!result["err"].eoo() && result["err"].type() == String &&
                            (_config->throwGLE || e["throwGLE"].trueValue()))
                            throw DBException((string) "From benchRun GLE " +
                                                  causedBy(result["err"].String()),
                                              result["code"].eoo() ? 0 : result["code"].Int());
                    }
                } else if (op == "createIndex") {
                    conn->ensureIndex(ns, e["key"].Obj(), false, "", false);
                } else if (op == "dropIndex") {
                    conn->dropIndex(ns, e["key"].Obj());
                } else if (op == "let") {
                    string target = e["target"].eoo() ? string() : e["target"].String();
                    BSONElement value = e["value"].eoo() ? BSONElement() : e["value"];
                    BSONObjBuilder valBuilder;
                    BSONObjBuilder templateBuilder;
                    valBuilder.append(value);
                    bsonTemplateEvaluator.evaluate(valBuilder.done(), templateBuilder);
                    bsonTemplateEvaluator.setVariable(target,
                                                      templateBuilder.done().firstElement());
                } else {
                    log() << "don't understand op: " << op << endl;
                    stats.error = true;
                    return;
                }
                // Count 1 for total ops. Successfully got through the try phrase
                stats.opCount++;
            } catch (DBException& ex) {
                if (!_config->hideErrors || e["showError"].trueValue()) {
                    bool yesWatch =
                        (_config->watchPattern && _config->watchPattern->FullMatch(ex.what()));
                    bool noWatch =
                        (_config->noWatchPattern && _config->noWatchPattern->FullMatch(ex.what()));

                    if ((!_config->watchPattern && _config->noWatchPattern &&
                         !noWatch) ||  // If we're just ignoring things
                        (!_config->noWatchPattern && _config->watchPattern &&
                         yesWatch) ||  // If we're just watching things
                        (_config->watchPattern && _config->noWatchPattern && yesWatch && !noWatch))
                        log() << "Error in benchRun thread for op " << e << causedBy(ex) << endl;
                }

                bool yesTrap = (_config->trapPattern && _config->trapPattern->FullMatch(ex.what()));
                bool noTrap =
                    (_config->noTrapPattern && _config->noTrapPattern->FullMatch(ex.what()));

                if ((!_config->trapPattern && _config->noTrapPattern && !noTrap) ||
                    (!_config->noTrapPattern && _config->trapPattern && yesTrap) ||
                    (_config->trapPattern && _config->noTrapPattern && yesTrap && !noTrap)) {
                    {
                        stats.trappedErrors.push_back(
                            BSON("error" << ex.what() << "op" << e << "count" << count));
                    }
                    if (_config->breakOnTrap)
                        return;
                }
                if (!_config->handleErrors && !e["handleError"].trueValue())
                    return;

                stats.errCount++;
            } catch (...) {
                if (!_config->hideErrors || e["showError"].trueValue())
                    log() << "Error in benchRun thread caused by unknown error for op " << e
                          << endl;
                if (!_config->handleErrors && !e["handleError"].trueValue())
                    return;

                stats.errCount++;
            }

            if (++count % 100 == 0 && !useWriteCmd) {
                conn->getLastError();
            }

            if (delay > 0)
                sleepmillis(delay);
        }
    }

    conn->getLastError();
}

namespace {
class BenchRunWorkerStateGuard {
    MONGO_DISALLOW_COPYING(BenchRunWorkerStateGuard);

public:
    explicit BenchRunWorkerStateGuard(BenchRunState* brState) : _brState(brState) {
        _brState->onWorkerStarted();
    }

    ~BenchRunWorkerStateGuard() {
        _brState->onWorkerFinished();
    }

private:
    BenchRunState* _brState;
};
}  // namespace

void BenchRunWorker::run() {
    try {
        std::unique_ptr<DBClientBase> conn(_config->createConnection());
        if (!_config->username.empty()) {
            string errmsg;
            if (!conn->auth("admin", _config->username, _config->password, errmsg)) {
                uasserted(15932, "Authenticating to connection for benchThread failed: " + errmsg);
            }
        }
        BenchRunWorkerStateGuard _workerStateGuard(_brState);
        generateLoadOnConnection(conn.get());
    } catch (DBException& e) {
        error() << "DBException not handled in benchRun thread" << causedBy(e) << endl;
    } catch (std::exception& e) {
        error() << "std::exception not handled in benchRun thread" << causedBy(e) << endl;
    } catch (...) {
        error() << "Unknown exception not handled in benchRun thread." << endl;
    }
}

BenchRunner::BenchRunner(BenchRunConfig* config) : _brState(config->parallel), _config(config) {
    _oid.init();
    stdx::lock_guard<stdx::mutex> lk(_staticMutex);
    _activeRuns[_oid] = this;
}

BenchRunner::~BenchRunner() {
    for (size_t i = 0; i < _workers.size(); ++i)
        delete _workers[i];
}

void BenchRunner::start() {
    {
        std::unique_ptr<DBClientBase> conn(_config->createConnection());
        // Must authenticate to admin db in order to run serverStatus command
        if (_config->username != "") {
            string errmsg;
            if (!conn->auth("admin", _config->username, _config->password, errmsg)) {
                uasserted(16704,
                          str::stream()
                              << "User " << _config->username
                              << " could not authenticate to admin db; admin db access is "
                                 "required to use benchRun with auth enabled");
            }
        }

        // Start threads
        for (int64_t i = 0; i < _config->parallel; i++) {
            // Make a unique random seed for the worker.
            int64_t seed = _config->randomSeed + i;
            BenchRunWorker* worker = new BenchRunWorker(i, _config.get(), &_brState, seed);
            worker->start();
            _workers.push_back(worker);
        }

        _brState.waitForState(BenchRunState::BRS_RUNNING);

        // initial stats
        _brState.tellWorkersToCollectStats();
        _brTimer = new mongo::Timer();
    }
}

void BenchRunner::stop() {
    _brState.tellWorkersToFinish();
    _brState.waitForState(BenchRunState::BRS_FINISHED);
    _microsElapsed = _brTimer->micros();
    delete _brTimer;

    {
        std::unique_ptr<DBClientBase> conn(_config->createConnection());
        if (_config->username != "") {
            string errmsg;
            // this can only fail if admin access was revoked since start of run
            if (!conn->auth("admin", _config->username, _config->password, errmsg)) {
                uasserted(16705,
                          str::stream()
                              << "User " << _config->username
                              << " could not authenticate to admin db; admin db access is "
                                 "still required to use benchRun with auth enabled");
            }
        }
    }

    {
        stdx::lock_guard<stdx::mutex> lk(_staticMutex);
        _activeRuns.erase(_oid);
    }
}

BenchRunner* BenchRunner::createWithConfig(const BSONObj& configArgs) {
    BenchRunConfig* config = BenchRunConfig::createFromBson(configArgs);
    return new BenchRunner(config);
}

BenchRunner* BenchRunner::get(OID oid) {
    stdx::lock_guard<stdx::mutex> lk(_staticMutex);
    return _activeRuns[oid];
}

void BenchRunner::populateStats(BenchRunStats* stats) {
    _brState.assertFinished();
    stats->reset();
    for (size_t i = 0; i < _workers.size(); ++i)
        stats->updateFrom(_workers[i]->stats());
}

static void appendAverageMicrosIfAvailable(BSONObjBuilder& buf,
                                           const std::string& name,
                                           const BenchRunEventCounter& counter) {
    if (counter.getNumEvents() > 0)
        buf.append(name,
                   static_cast<double>(counter.getTotalTimeMicros()) / counter.getNumEvents());
}

BSONObj BenchRunner::finish(BenchRunner* runner) {
    runner->stop();

    BenchRunStats stats;
    runner->populateStats(&stats);

    // vector<BSONOBj> errors = runner->config.errors;
    bool error = stats.error;

    if (error)
        return BSON("err" << 1);

    BSONObjBuilder buf;
    buf.append("note", "values per second");
    buf.append("errCount", static_cast<long long>(stats.errCount));
    buf.append("trapped", "error: not implemented");
    appendAverageMicrosIfAvailable(buf, "findOneLatencyAverageMicros", stats.findOneCounter);
    appendAverageMicrosIfAvailable(buf, "insertLatencyAverageMicros", stats.insertCounter);
    appendAverageMicrosIfAvailable(buf, "deleteLatencyAverageMicros", stats.deleteCounter);
    appendAverageMicrosIfAvailable(buf, "updateLatencyAverageMicros", stats.updateCounter);
    appendAverageMicrosIfAvailable(buf, "queryLatencyAverageMicros", stats.queryCounter);
    appendAverageMicrosIfAvailable(buf, "commandsLatencyAverageMicros", stats.commandCounter);

    buf.append("totalOps", static_cast<long long>(stats.opCount));

    auto appendPerSec = [&buf, runner](StringData name, double total) {
        buf.append(name, total / (runner->_microsElapsed / 1000000.0));
    };

    appendPerSec("totalOps/s", stats.opCount);
    appendPerSec("findOne", stats.findOneCounter.getNumEvents());
    appendPerSec("insert", stats.insertCounter.getNumEvents());
    appendPerSec("delete", stats.deleteCounter.getNumEvents());
    appendPerSec("update", stats.updateCounter.getNumEvents());
    appendPerSec("query", stats.queryCounter.getNumEvents());
    appendPerSec("command", stats.commandCounter.getNumEvents());

    BSONObj zoo = buf.obj();

    delete runner;
    return zoo;
}

stdx::mutex BenchRunner::_staticMutex;
map<OID, BenchRunner*> BenchRunner::_activeRuns;

/**
 * benchRun( { ops : [] , host : XXX , db : XXXX , parallel : 5 , seconds : 5 }
 */
BSONObj BenchRunner::benchRunSync(const BSONObj& argsFake, void* data) {
    BSONObj start = benchStart(argsFake, data);

    OID oid = OID(start.firstElement().String());
    BenchRunner* runner = BenchRunner::get(oid);

    sleepmillis((int)(1000.0 * runner->config().seconds));

    return benchFinish(start, data);
}

/**
 * benchRun( { ops : [] , host : XXX , db : XXXX , parallel : 5 , seconds : 5 }
 */
BSONObj BenchRunner::benchStart(const BSONObj& argsFake, void* data) {
    verify(argsFake.firstElement().isABSONObj());
    BSONObj args = argsFake.firstElement().Obj();

    // Get new BenchRunner object
    BenchRunner* runner = BenchRunner::createWithConfig(args);

    runner->start();
    return BSON("" << runner->oid().toString());
}

/**
 * benchRun( { ops : [] , host : XXX , db : XXXX , parallel : 5 , seconds : 5 }
 */
BSONObj BenchRunner::benchFinish(const BSONObj& argsFake, void* data) {
    OID oid = OID(argsFake.firstElement().String());

    // Get old BenchRunner object
    BenchRunner* runner = BenchRunner::get(oid);

    BSONObj finalObj = BenchRunner::finish(runner);

    return BSON("" << finalObj);
}

}  // namespace mongo
