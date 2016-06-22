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

#include <iostream>
#include <pcrecpp.h>

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request.h"
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

const std::map<OpType, std::string> opTypeName{{OpType::NONE, "none"},
                                               {OpType::NOP, "nop"},
                                               {OpType::FINDONE, "findOne"},
                                               {OpType::COMMAND, "command"},
                                               {OpType::FIND, "find"},
                                               {OpType::UPDATE, "update"},
                                               {OpType::INSERT, "insert"},
                                               {OpType::REMOVE, "remove"},
                                               {OpType::CREATEINDEX, "createIndex"},
                                               {OpType::DROPINDEX, "dropIndex"},
                                               {OpType::LET, "let"}};

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

    throwGLE = false;
    breakOnTrap = true;
    randomSeed = 1314159265358979323;
}

BenchRunConfig* BenchRunConfig::createFromBson(const BSONObj& args) {
    BenchRunConfig* config = new BenchRunConfig();
    config->initializeFromBson(args);
    return config;
}

BenchRunOp opFromBson(const BSONObj& op) {
    BenchRunOp myOp;
    myOp.myBsonOp = op.getOwned();  // save an owned copy of the BSON obj
    auto opType = myOp.myBsonOp["op"].valueStringData();
    for (auto arg : myOp.myBsonOp) {
        auto name = arg.fieldNameStringData();
        if (name == "batchSize") {
            uassert(34377,
                    str::stream() << "Field 'batchSize' should be a number, instead it's type: "
                                  << typeName(arg.type()),
                    arg.isNumber());
            uassert(34378,
                    str::stream() << "Field 'batchSize' only valid for find op types. Type is "
                                  << opType,
                    (opType == "find") || (opType == "query"));
            myOp.batchSize = arg.numberInt();
        } else if (name == "check") {
            // check function gets thrown into a scoped function. Leaving that parsing in main loop.
            myOp.useCheck = true;
            myOp.check = arg;
            uassert(
                34420,
                str::stream()
                    << "Check field requires type CodeWScoe, Code, or String, instead its type is: "
                    << typeName(myOp.check.type()),
                (myOp.check.type() == CodeWScope || myOp.check.type() == Code ||
                 myOp.check.type() == String));

        } else if (name == "command") {
            // type needs to be command
            uassert(34398,
                    str::stream() << "Field 'command' only valid for command op type. Type is "
                                  << opType,
                    opType == "command");
            myOp.command = arg.Obj();
        } else if (name == "context") {
            myOp.context = arg.Obj();
        } else if (name == "delay") {
            uassert(34379,
                    str::stream() << "Field 'delay' should be a number, instead it's type: "
                                  << typeName(arg.type()),
                    arg.isNumber());
            myOp.delay = arg.numberInt();
        } else if (name == "doc") {
            uassert(34399,
                    str::stream() << "Field 'doc' only valid for insert op type. Type is "
                                  << opType,
                    (opType == "insert"));
            myOp.isDocAnArray = arg.type() == Array;
            myOp.doc = arg.Obj();
        } else if (name == "expected") {
            uassert(34380,
                    str::stream() << "Field 'Expected' should be a number, instead it's type: "
                                  << typeName(arg.type()),
                    arg.isNumber());
            uassert(34400,
                    str::stream() << "Field 'Expected' only valid for find op type. Type is "
                                  << opType,
                    (opType == "find") || (opType == "query"));
            myOp.expected = arg.numberInt();
        } else if (name == "filter") {
            uassert(
                34401,
                str::stream()
                    << "Field 'Filter' (projection) only valid for find/findOne op type. Type is "
                    << opType,
                (opType == "find") || (opType == "query") || (opType == "findOne"));
            myOp.projection = arg.Obj();  // the name should be switched to projection
                                          // also, but that will break things
        } else if (name == "handleError") {
            myOp.handleError = arg.trueValue();
        } else if (name == "key") {
            uassert(34402,
                    str::stream()
                        << "Field 'key' only valid for create or drop index op types. Type is "
                        << opType,
                    (opType == "createIndex") || (opType == "dropIndex"));
            myOp.key = arg.Obj();
        } else if (name == "limit") {
            uassert(34381,
                    str::stream() << "Field 'limit' is only valid for find op types. Type is "
                                  << opType,
                    (opType == "find") || (opType == "query"));
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Field 'limit' should be a number, instead it's type: "
                                  << typeName(arg.type()),
                    arg.isNumber());
            myOp.limit = arg.numberInt();
        } else if (name == "multi") {
            uassert(34383,
                    str::stream()
                        << "Field 'multi' is only valid for update/remove/delete types. Type is "
                        << opType,
                    (opType == "update") || (opType == "remove") || (opType == "delete"));
            myOp.multi = arg.trueValue();
        } else if (name == "ns") {
            uassert(34385,
                    str::stream() << "Field 'ns' should be a string, instead it's type: "
                                  << typeName(arg.type()),
                    arg.type() == String);
            myOp.ns = arg.String();
        } else if (name == "op") {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Field 'op' is not a string, instead it's type: "
                                  << typeName(arg.type()),
                    arg.type() == String);
            auto type = arg.valueStringData();
            if (type == "nop") {
                myOp.op = OpType::NOP;
            } else if (type == "findOne") {
                myOp.op = OpType::FINDONE;
            } else if (type == "command") {
                myOp.op = OpType::COMMAND;
            } else if (type == "find" || type == "query") {
                myOp.op = OpType::FIND;
            } else if (type == "update") {
                myOp.op = OpType::UPDATE;
            } else if (type == "insert") {
                myOp.op = OpType::INSERT;
            } else if (type == "delete" || type == "remove") {
                myOp.op = OpType::REMOVE;
            } else if (type == "createIndex") {
                myOp.op = OpType::CREATEINDEX;
            } else if (type == "dropIndex") {
                myOp.op = OpType::DROPINDEX;
            } else if (type == "let") {
                myOp.op = OpType::LET;
            } else {
                uassert(34387,
                        str::stream() << "benchRun passed an unsupported op type: " << type,
                        false);
            }
        } else if (name == "options") {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Field 'options' should be a number, instead it's type: "
                                  << typeName(arg.type()),
                    arg.isNumber());
            uassert(34388,
                    str::stream() << "Field 'options' but not a command or find type. Type is "
                                  << opType,
                    (opType == "command") || (opType == "query") || (opType == "find"));
            myOp.options = arg.numberInt();
        } else if (name == "query") {
            uassert(34389,
                    str::stream() << "Field 'query' is only valid for findOne, find, update, and "
                                     "remove types. Type is "
                                  << opType,
                    (opType == "findOne") || (opType == "query") ||
                        (opType == "find" || (opType == "update") || (opType == "delete") ||
                         (opType == "remove")));
            myOp.query = arg.Obj();
        } else if (name == "safe") {
            myOp.safe = arg.trueValue();
        } else if (name == "skip") {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Field 'skip' should be a number, instead it's type: "
                                  << typeName(arg.type()),
                    arg.isNumber());
            uassert(34390,
                    str::stream() << "Field 'skip' is only valid for find/query op types. Type is "
                                  << opType,
                    (opType == "find") || (opType == "query"));
            myOp.skip = arg.numberInt();
        } else if (name == "showError") {
            myOp.showError = arg.trueValue();
        } else if (name == "showResult") {
            myOp.showResult = arg.trueValue();
        } else if (name == "target") {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Field 'target' should be a string. It's type: "
                                  << typeName(arg.type()),
                    arg.type() == String);
            myOp.target = arg.String();
        } else if (name == "throwGLE") {
            myOp.throwGLE = arg.trueValue();
        } else if (name == "update") {
            uassert(34391,
                    str::stream() << "Field 'update' is only valid for update op type. Op type is "
                                  << opType,
                    (opType == "update"));
            myOp.update = arg.Obj();
        } else if (name == "upsert") {
            uassert(34392,
                    str::stream() << "Field 'upsert' is only valid for update op type. Op type is "
                                  << opType,
                    (opType == "update"));
            myOp.upsert = arg.trueValue();
        } else if (name == "readCmd") {
            myOp.useReadCmd = arg.trueValue();
        } else if (name == "writeCmd") {
            myOp.useWriteCmd = arg.trueValue();
        } else if (name == "writeConcern") {
            // Mongo-perf wants to pass the write concern into all calls. It is only used for
            // update, insert, delete
            myOp.writeConcern = arg.Obj();
        } else if (name == "value") {
            uassert(34403,
                    str::stream() << "Field 'value' is only valid for let op type. Op type is "
                                  << opType,
                    opType == "let");
            BSONObjBuilder valBuilder;
            valBuilder.append(arg);
            myOp.value = valBuilder.obj();
        } else {
            uassert(34394, str::stream() << "Benchrun op has unsupported field: " << name, false);
        }
    }

    uassert(34395, "Benchrun op has an zero length ns", !myOp.ns.empty());
    uassert(34396, "Benchrun op doesn't have an optype set", myOp.op != OpType::NONE);
    return myOp;
}

void BenchRunConfig::initializeFromBson(const BSONObj& args) {
    initializeToDefaults();

    for (auto arg : args) {
        auto name = arg.fieldNameStringData();
        if (name == "host") {
            uassert(34404,
                    str::stream() << "Field '" << name << "' should be a string. . Type is "
                                  << typeName(arg.type()),
                    arg.type() == String);
            host = arg.String();
        } else if (name == "db") {
            uassert(34405,
                    str::stream() << "Field '" << name << "' should be a string. . Type is "
                                  << typeName(arg.type()),
                    arg.type() == String);
            db = arg.String();
        } else if (name == "username") {
            uassert(34406,
                    str::stream() << "Field '" << name << "' should be a string. . Type is "
                                  << typeName(arg.type()),
                    arg.type() == String);
            username = arg.String();
        } else if (name == "password") {
            uassert(34407,
                    str::stream() << "Field '" << name << "' should be a string. . Type is "
                                  << typeName(arg.type()),
                    arg.type() == String);
            password = arg.String();
        } else if (name == "parallel") {
            uassert(34409,
                    str::stream() << "Field '" << name << "' should be a number. . Type is "
                                  << typeName(arg.type()),
                    arg.isNumber());
            parallel = arg.numberInt();
        } else if (name == "randomSeed") {
            uassert(34365,
                    str::stream() << "Field '" << name << "' should be a number. . Type is "
                                  << typeName(arg.type()),
                    arg.isNumber());
            randomSeed = arg.numberInt();
        } else if (name == "seconds") {
            uassert(34408,
                    str::stream() << "Field '" << name << "' should be a number. . Type is "
                                  << typeName(arg.type()),
                    arg.isNumber());
            seconds = arg.number();
        } else if (name == "hideResults") {
            hideResults = arg.trueValue();
        } else if (name == "handleErrors") {
            handleErrors = arg.trueValue();
        } else if (name == "hideErrors") {
            hideErrors = arg.trueValue();
        } else if (name == "throwGLE") {
            throwGLE = arg.trueValue();
        } else if (name == "breakOnTrap") {
            breakOnTrap = arg.trueValue();
        } else if (name == "trapPattern") {
            const char* regex = arg.regex();
            const char* flags = arg.regexFlags();
            trapPattern =
                std::shared_ptr<pcrecpp::RE>(new pcrecpp::RE(regex, flags2options(flags)));
        } else if (name == "noTrapPattern") {
            const char* regex = arg.regex();
            const char* flags = arg.regexFlags();
            noTrapPattern =
                std::shared_ptr<pcrecpp::RE>(new pcrecpp::RE(regex, flags2options(flags)));
        } else if (name == "watchPattern") {
            const char* regex = arg.regex();
            const char* flags = arg.regexFlags();
            watchPattern =
                std::shared_ptr<pcrecpp::RE>(new pcrecpp::RE(regex, flags2options(flags)));
        } else if (name == "noWatchPattern") {
            const char* regex = arg.regex();
            const char* flags = arg.regexFlags();
            noWatchPattern =
                std::shared_ptr<pcrecpp::RE>(new pcrecpp::RE(regex, flags2options(flags)));
        } else if (name == "ops") {
            // iterate through the objects in ops
            // create an BenchRunOp per
            // put in ops vector.
            BSONObjIterator i(arg.Obj());
            while (i.more()) {
                ops.push_back(opFromBson(i.next().Obj()));
            }
        } else {
            log() << "benchRun passed an unsupported field: " << name;
            uassert(34376, "benchRun passed an unsupported configuration field", false);
        }
    }
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
 * Issues the query 'qr' against 'conn' using read commands. Returns the size of the result set
 * returned by the query.
 *
 * If 'qr' has the 'wantMore' flag set to false and the 'limit' option set to 1LL, then the caller
 * may optionally specify a pointer to an object in 'objOut', which will be filled in with the
 * single object in the query result set (or the empty object, if the result set is empty).
 * If 'qr' doesn't have these options set, then nullptr must be passed for 'objOut'.
 *
 * On error, throws a UserException.
 */
int runQueryWithReadCommands(DBClientBase* conn,
                             unique_ptr<QueryRequest> qr,
                             BSONObj* objOut = nullptr) {
    std::string dbName = qr->nss().db().toString();
    BSONObj findCommandResult;
    bool res = conn->runCommand(dbName, qr->asFindCommand(), findCommandResult);
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "find command failed; reply was: " << findCommandResult,
            res);

    CursorResponse cursorResponse =
        uassertStatusOK(CursorResponse::parseFromBSON(findCommandResult));
    int count = cursorResponse.getBatch().size();

    if (objOut) {
        invariant(qr->getLimit() && *qr->getLimit() == 1 && !qr->wantMore());
        // Since this is a "single batch" query, we can simply grab the first item in the result set
        // and return here.
        *objOut = (count > 0) ? cursorResponse.getBatch()[0] : BSONObj();
        return count;
    }

    while (cursorResponse.getCursorId() != 0) {
        GetMoreRequest getMoreRequest(qr->nss(),
                                      cursorResponse.getCursorId(),
                                      qr->getBatchSize(),
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

    unique_ptr<Scope> scope{globalScriptEngine->newScopeForCurrentThread()};
    verify(scope.get());

    while (!shouldStop()) {
        for (const auto& op : _config->ops) {
            if (shouldStop())
                break;
            auto& stats = shouldCollectStats() ? _stats : _statsBlackHole;

            ScriptingFunction scopeFunc = 0;
            BSONObj scopeObj;
            if (op.useCheck) {
                auto check = op.check;

                if (check.type() == CodeWScope) {
                    scopeFunc = scope->createFunction(check.codeWScopeCode());
                    scopeObj = BSONObj(check.codeWScopeScopeDataUnsafe());
                } else {
                    scopeFunc = scope->createFunction(check.valuestr());
                }

                scope->init(&scopeObj);
                invariant(scopeFunc);
            }

            try {
                switch (op.op) {
                    case OpType::NOP:
                        break;
                    case OpType::FINDONE: {
                        BSONObj fixedQuery = fixQuery(op.query, bsonTemplateEvaluator);
                        BSONObj result;
                        if (op.useReadCmd) {
                            auto qr = stdx::make_unique<QueryRequest>(NamespaceString(op.ns));
                            qr->setFilter(fixedQuery);
                            qr->setProj(op.projection);
                            qr->setLimit(1LL);
                            qr->setWantMore(false);
                            invariantOK(qr->validate());

                            BenchRunEventTrace _bret(&stats.findOneCounter);
                            runQueryWithReadCommands(conn, std::move(qr), &result);
                        } else {
                            BenchRunEventTrace _bret(&stats.findOneCounter);
                            result = conn->findOne(op.ns, fixedQuery);
                        }

                        if (op.useCheck) {
                            int err = scope->invoke(scopeFunc, 0, &result, 1000 * 60, false);
                            if (err) {
                                log() << "Error checking in benchRun thread [findOne]"
                                      << causedBy(scope->getError()) << endl;

                                stats.errCount++;

                                return;
                            }
                        }

                        if (!_config->hideResults || op.showResult)
                            log() << "Result from benchRun thread [findOne] : " << result << endl;
                    } break;
                    case OpType::COMMAND: {
                        bool ok;
                        BSONObj result;
                        {
                            BenchRunEventTrace _bret(&stats.commandCounter);
                            ok = conn->runCommand(op.ns,
                                                  fixQuery(op.command, bsonTemplateEvaluator),
                                                  result,
                                                  op.options);
                        }
                        if (!ok) {
                            stats.errCount++;
                        }

                        if (!result["cursor"].eoo()) {
                            // The command returned a cursor, so iterate all results.
                            auto cursorResponse =
                                uassertStatusOK(CursorResponse::parseFromBSON(result));
                            int count = cursorResponse.getBatch().size();
                            while (cursorResponse.getCursorId() != 0) {
                                GetMoreRequest getMoreRequest(
                                    cursorResponse.getNSS(),
                                    cursorResponse.getCursorId(),
                                    boost::none,   // batchSize
                                    boost::none,   // maxTimeMS
                                    boost::none,   // term
                                    boost::none);  // lastKnownCommittedOpTime
                                BSONObj getMoreCommandResult;
                                ok = conn->runCommand(
                                    op.ns, getMoreRequest.toBSON(), getMoreCommandResult);
                                uassert(ErrorCodes::CommandFailed,
                                        str::stream() << "getMore command failed; reply was: "
                                                      << getMoreCommandResult,
                                        ok);
                                cursorResponse = uassertStatusOK(
                                    CursorResponse::parseFromBSON(getMoreCommandResult));
                                count += cursorResponse.getBatch().size();
                            }
                            // Just give the count to the check function.
                            result = BSON("count" << count << "context" << op.context);
                        }

                        if (op.useCheck) {
                            int err = scope->invoke(scopeFunc, 0, &result, 1000 * 60, false);
                            if (err) {
                                log() << "Error checking in benchRun thread [command]"
                                      << causedBy(scope->getError()) << endl;
                                int err = scope->invoke(scopeFunc, 0, &result, 1000 * 60, false);
                                if (err) {
                                    log() << "Error checking in benchRun thread [command]"
                                          << causedBy(scope->getError()) << endl;

                                    stats.errCount++;

                                    return;
                                }
                            }

                            if (!_config->hideResults || op.showResult)
                                log() << "Result from benchRun thread [command] : " << result
                                      << endl;
                        }
                    } break;
                    case OpType::FIND: {
                        int count;

                        BSONObj fixedQuery = fixQuery(op.query, bsonTemplateEvaluator);

                        if (op.useReadCmd) {
                            uassert(28824,
                                    "cannot use 'options' in combination with read commands",
                                    !op.options);

                            auto qr = stdx::make_unique<QueryRequest>(NamespaceString(op.ns));
                            qr->setFilter(fixedQuery);
                            qr->setProj(op.projection);
                            if (op.skip) {
                                qr->setSkip(op.skip);
                            }
                            if (op.limit) {
                                qr->setLimit(op.limit);
                            }
                            if (op.batchSize) {
                                qr->setBatchSize(op.batchSize);
                            }
                            invariantOK(qr->validate());

                            BenchRunEventTrace _bret(&stats.queryCounter);
                            count = runQueryWithReadCommands(conn, std::move(qr));
                        } else {
                            // Use special query function for exhaust query option.
                            if (op.options & QueryOption_Exhaust) {
                                BenchRunEventTrace _bret(&stats.queryCounter);
                                stdx::function<void(const BSONObj&)> castedDoNothing(doNothing);
                                count = conn->query(
                                    castedDoNothing, op.ns, fixedQuery, &op.projection, op.options);
                            } else {
                                BenchRunEventTrace _bret(&stats.queryCounter);
                                unique_ptr<DBClientCursor> cursor;
                                cursor = conn->query(op.ns,
                                                     fixedQuery,
                                                     op.limit,
                                                     op.skip,
                                                     &op.projection,
                                                     op.options,
                                                     op.batchSize);
                                count = cursor->itcount();
                            }
                        }

                        if (op.expected >= 0 && count != op.expected) {
                            cout << "bench query on: " << op.ns << " expected: " << op.expected
                                 << " got: " << count << endl;
                            verify(false);
                        }

                        if (op.useCheck) {
                            BSONObj thisValue = BSON("count" << count << "context" << op.context);
                            int err = scope->invoke(scopeFunc, 0, &thisValue, 1000 * 60, false);
                            if (err) {
                                log() << "Error checking in benchRun thread [find]"
                                      << causedBy(scope->getError()) << endl;

                                stats.errCount++;

                                return;
                            }
                        }

                        if (!_config->hideResults || op.showResult)
                            log() << "Result from benchRun thread [query] : " << count << endl;
                    } break;
                    case OpType::UPDATE: {
                        BSONObj result;
                        {
                            BenchRunEventTrace _bret(&stats.updateCounter);
                            BSONObj query = fixQuery(op.query, bsonTemplateEvaluator);
                            BSONObj update = fixQuery(op.update, bsonTemplateEvaluator);

                            if (op.useWriteCmd) {
                                // TODO: Replace after SERVER-11774.
                                BSONObjBuilder builder;
                                builder.append("update", nsToCollectionSubstring(op.ns));
                                BSONArrayBuilder docBuilder(builder.subarrayStart("updates"));
                                docBuilder.append(BSON(
                                    "q" << query << "u" << update << "multi" << op.multi << "upsert"
                                        << op.upsert));
                                docBuilder.done();
                                builder.append("writeConcern", op.writeConcern);
                                conn->runCommand(nsToDatabaseSubstring(op.ns).toString(),
                                                 builder.done(),
                                                 result);
                            } else {
                                conn->update(op.ns, query, update, op.upsert, op.multi);
                                if (op.safe)
                                    result = conn->getLastErrorDetailed();
                            }
                        }

                        if (op.safe) {
                            if (op.useCheck) {
                                int err = scope->invoke(scopeFunc, 0, &result, 1000 * 60, false);
                                if (err) {
                                    log() << "Error checking in benchRun thread [update]"
                                          << causedBy(scope->getError()) << endl;

                                    stats.errCount++;

                                    return;
                                }
                            }

                            if (!_config->hideResults || op.showResult)
                                log() << "Result from benchRun thread [safe update] : " << result
                                      << endl;

                            if (!result["err"].eoo() && result["err"].type() == String &&
                                (_config->throwGLE || op.throwGLE))
                                throw DBException((string) "From benchRun GLE" +
                                                      causedBy(result["err"].String()),
                                                  result["code"].eoo() ? 0 : result["code"].Int());
                        }
                    } break;
                    case OpType::INSERT: {
                        BSONObj result;

                        {
                            BenchRunEventTrace _bret(&stats.insertCounter);

                            BSONObj insertDoc;
                            if (op.useWriteCmd) {
                                // TODO: Replace after SERVER-11774.
                                BSONObjBuilder builder;
                                builder.append("insert", nsToCollectionSubstring(op.ns));
                                BSONArrayBuilder docBuilder(builder.subarrayStart("documents"));
                                if (op.isDocAnArray) {
                                    for (const auto& element : op.doc) {
                                        insertDoc = fixQuery(element.Obj(), bsonTemplateEvaluator);
                                        docBuilder.append(insertDoc);
                                    }
                                } else {
                                    insertDoc = fixQuery(op.doc, bsonTemplateEvaluator);
                                    docBuilder.append(insertDoc);
                                }
                                docBuilder.done();
                                builder.append("writeConcern", op.writeConcern);
                                conn->runCommand(nsToDatabaseSubstring(op.ns).toString(),
                                                 builder.done(),
                                                 result);
                            } else {
                                if (op.isDocAnArray) {
                                    std::vector<BSONObj> insertArray;
                                    for (const auto& element : op.doc) {
                                        BSONObj e = fixQuery(element.Obj(), bsonTemplateEvaluator);
                                        insertArray.push_back(e);
                                    }
                                    conn->insert(op.ns, insertArray);
                                } else {
                                    insertDoc = fixQuery(op.doc, bsonTemplateEvaluator);
                                    conn->insert(op.ns, insertDoc);
                                }
                                if (op.safe)
                                    result = conn->getLastErrorDetailed();
                            }
                        }

                        if (op.safe) {
                            if (op.useCheck) {
                                int err = scope->invoke(scopeFunc, 0, &result, 1000 * 60, false);
                                if (err) {
                                    log() << "Error checking in benchRun thread [insert]"
                                          << causedBy(scope->getError()) << endl;

                                    stats.errCount++;

                                    return;
                                }
                            }

                            if (!_config->hideResults || op.showResult)
                                log() << "Result from benchRun thread [safe insert] : " << result
                                      << endl;

                            if (!result["err"].eoo() && result["err"].type() == String &&
                                (_config->throwGLE || op.throwGLE))
                                throw DBException((string) "From benchRun GLE" +
                                                      causedBy(result["err"].String()),
                                                  result["code"].eoo() ? 0 : result["code"].Int());
                        }
                    } break;
                    case OpType::REMOVE: {
                        BSONObj result;
                        {
                            BenchRunEventTrace _bret(&stats.deleteCounter);
                            BSONObj predicate = fixQuery(op.query, bsonTemplateEvaluator);
                            if (op.useWriteCmd) {
                                // TODO: Replace after SERVER-11774.
                                BSONObjBuilder builder;
                                builder.append("delete", nsToCollectionSubstring(op.ns));
                                BSONArrayBuilder docBuilder(builder.subarrayStart("deletes"));
                                int limit = (op.multi == true) ? 0 : 1;
                                docBuilder.append(BSON("q" << predicate << "limit" << limit));
                                docBuilder.done();
                                builder.append("writeConcern", op.writeConcern);
                                conn->runCommand(nsToDatabaseSubstring(op.ns).toString(),
                                                 builder.done(),
                                                 result);
                            } else {
                                conn->remove(op.ns, predicate, !op.multi);
                                if (op.safe)
                                    result = conn->getLastErrorDetailed();
                            }
                        }

                        if (op.safe) {
                            if (op.useCheck) {
                                int err = scope->invoke(scopeFunc, 0, &result, 1000 * 60, false);
                                if (err) {
                                    log() << "Error checking in benchRun thread [delete]"
                                          << causedBy(scope->getError()) << endl;

                                    stats.errCount++;

                                    return;
                                }
                            }

                            if (!_config->hideResults || op.showResult)
                                log() << "Result from benchRun thread [safe remove] : " << result
                                      << endl;

                            if (!result["err"].eoo() && result["err"].type() == String &&
                                (_config->throwGLE || op.throwGLE))
                                throw DBException((string) "From benchRun GLE " +
                                                      causedBy(result["err"].String()),
                                                  result["code"].eoo() ? 0 : result["code"].Int());
                        }
                    } break;
                    case OpType::CREATEINDEX:
                        conn->createIndex(op.ns, op.key);
                        break;
                    case OpType::DROPINDEX:
                        conn->dropIndex(op.ns, op.key);
                        break;
                    case OpType::LET: {
                        BSONObjBuilder templateBuilder;
                        bsonTemplateEvaluator.evaluate(op.value, templateBuilder);
                        bsonTemplateEvaluator.setVariable(op.target,
                                                          templateBuilder.done().firstElement());
                    } break;
                    default:
                        uassert(34397, "In benchRun loop and got unknown op type", false);
                }
                // Count 1 for total ops. Successfully got through the try phrase
                stats.opCount++;
            } catch (DBException& ex) {
                if (!_config->hideErrors || op.showError) {
                    bool yesWatch =
                        (_config->watchPattern && _config->watchPattern->FullMatch(ex.what()));
                    bool noWatch =
                        (_config->noWatchPattern && _config->noWatchPattern->FullMatch(ex.what()));

                    if ((!_config->watchPattern && _config->noWatchPattern &&
                         !noWatch) ||  // If we're just ignoring things
                        (!_config->noWatchPattern && _config->watchPattern &&
                         yesWatch) ||  // If we're just watching things
                        (_config->watchPattern && _config->noWatchPattern && yesWatch && !noWatch))
                        log() << "Error in benchRun thread for op "
                              << opTypeName.find(op.op)->second << causedBy(ex) << endl;
                }

                bool yesTrap = (_config->trapPattern && _config->trapPattern->FullMatch(ex.what()));
                bool noTrap =
                    (_config->noTrapPattern && _config->noTrapPattern->FullMatch(ex.what()));

                if ((!_config->trapPattern && _config->noTrapPattern && !noTrap) ||
                    (!_config->noTrapPattern && _config->trapPattern && yesTrap) ||
                    (_config->trapPattern && _config->noTrapPattern && yesTrap && !noTrap)) {
                    {
                        stats.trappedErrors.push_back(BSON("error" << ex.what() << "op"
                                                                   << opTypeName.find(op.op)->second
                                                                   << "count"
                                                                   << count));
                    }
                    if (_config->breakOnTrap)
                        return;
                }
                if (!_config->handleErrors && !op.handleError)
                    return;

                stats.errCount++;
            } catch (...) {
                if (!_config->hideErrors || op.showError)
                    log() << "Error in benchRun thread caused by unknown error for op "
                          << opTypeName.find(op.op)->second << endl;
                if (!_config->handleErrors && !op.handleError)
                    return;

                stats.errCount++;
            }

            if (++count % 100 == 0 && !op.useWriteCmd) {
                conn->getLastError();
            }

            if (op.delay > 0)
                sleepmillis(op.delay);
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
                uasserted(
                    16704,
                    str::stream() << "User " << _config->username
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
                uasserted(
                    16705,
                    str::stream() << "User " << _config->username
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
