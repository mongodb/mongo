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

#include "mongo/shell/bench.h"

#include <string>

#include "mongo/base/shim.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/scripting/bson_template_evaluator.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/md5.h"
#include "mongo/util/pcre.h"
#include "mongo/util/pcre_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

const std::map<OpType, std::string> kOpTypeNames{{OpType::NONE, "none"},
                                                 {OpType::NOP, "nop"},
                                                 {OpType::FINDONE, "findOne"},
                                                 {OpType::COMMAND, "command"},
                                                 {OpType::FIND, "find"},
                                                 {OpType::UPDATE, "update"},
                                                 {OpType::INSERT, "insert"},
                                                 {OpType::REMOVE, "remove"},
                                                 {OpType::CREATEINDEX, "createIndex"},
                                                 {OpType::DROPINDEX, "dropIndex"},
                                                 {OpType::LET, "let"},
                                                 {OpType::CPULOAD, "cpuload"}};

// When specified to the connection's 'runCommand' call indicates that the command should be
// executed with no query options. This is only meaningful if a command is run via OP_QUERY against
// '$cmd'.
const int kNoOptions = 0;
const int kStartTransactionOption = 1 << 0;
const int kMultiStatementTransactionOption = 1 << 1;

const BSONObj readConcernSnapshot = BSON("level"
                                         << "snapshot");

class BenchRunWorkerStateGuard {
    BenchRunWorkerStateGuard(const BenchRunWorkerStateGuard&) = delete;
    BenchRunWorkerStateGuard& operator=(const BenchRunWorkerStateGuard&) = delete;

public:
    explicit BenchRunWorkerStateGuard(BenchRunState& brState) : _brState(brState) {
        _brState.onWorkerStarted();
    }

    ~BenchRunWorkerStateGuard() {
        _brState.onWorkerFinished();
    }

private:
    BenchRunState& _brState;
};

bool hasSpecial(const BSONObj& obj) {
    BSONObjIterator i(obj);
    while (i.more()) {
        BSONElement e = i.next();
        if (e.fieldName()[0] == '#')
            return true;

        if (!e.isABSONObj())
            continue;

        if (hasSpecial(e.Obj()))
            return true;
    }
    return false;
}

BSONObj fixQuery(const BSONObj& obj, BsonTemplateEvaluator& btl) {
    if (!hasSpecial(obj))
        return obj;

    BSONObjBuilder b(obj.objsize() + 128);
    verify(BsonTemplateEvaluator::StatusSuccess == btl.evaluate(obj, b));
    return b.obj();
}

bool runCommandWithSession(DBClientBase* conn,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           int options,
                           const boost::optional<LogicalSessionIdToClient>& lsid,
                           boost::optional<TxnNumber> txnNumber,
                           BSONObj* result) {
    if (!lsid) {
        invariant(!txnNumber);
        return conn->runCommand(dbname, cmdObj, *result);
    }

    BSONObjBuilder cmdObjWithLsidBuilder;

    for (const auto& cmdArg : cmdObj) {
        uassert(ErrorCodes::IllegalOperation,
                "Command cannot contain session id",
                cmdArg.fieldName() != OperationSessionInfo::kSessionIdFieldName);
        uassert(ErrorCodes::IllegalOperation,
                "Command cannot contain transaction id",
                cmdArg.fieldName() != OperationSessionInfo::kTxnNumberFieldName);

        cmdObjWithLsidBuilder.append(cmdArg);
    }

    {
        BSONObjBuilder lsidBuilder(
            cmdObjWithLsidBuilder.subobjStart(OperationSessionInfo::kSessionIdFieldName));
        lsid->serialize(&lsidBuilder);
        lsidBuilder.doneFast();
    }

    if (txnNumber) {
        cmdObjWithLsidBuilder.append(OperationSessionInfo::kTxnNumberFieldName, *txnNumber);
    }

    if (options & kMultiStatementTransactionOption) {
        cmdObjWithLsidBuilder.append("autocommit", false);
    }

    if (options & kStartTransactionOption) {
        cmdObjWithLsidBuilder.append("startTransaction", true);
    }

    return conn->runCommand(dbname, cmdObjWithLsidBuilder.done(), *result);
}

bool runCommandWithSession(DBClientBase* conn,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           int options,
                           const boost::optional<LogicalSessionIdToClient>& lsid,
                           BSONObj* result) {
    return runCommandWithSession(conn, dbname, cmdObj, options, lsid, boost::none, result);
}

void abortTransaction(DBClientBase* conn,
                      const boost::optional<LogicalSessionIdToClient>& lsid,
                      boost::optional<TxnNumber> txnNumber) {
    BSONObj abortTransactionCmd = BSON("abortTransaction" << 1);
    BSONObj abortCommandResult;
    const bool successful = runCommandWithSession(conn,
                                                  "admin",
                                                  abortTransactionCmd,
                                                  kMultiStatementTransactionOption,
                                                  lsid,
                                                  txnNumber,
                                                  &abortCommandResult);
    // Transaction could be aborted already
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "abort command failed; reply was: " << abortCommandResult,
            successful || abortCommandResult["codeName"].valueStringData() == "NoSuchTransaction");
}

/**
 * Issues the query 'findCommand' against 'conn' using read commands. Returns the size of the result
 * set returned by the query.
 *
 * If 'findCommand' has the 'wantMore' flag set to false and the 'limit' option set to 1LL, then the
 * caller may optionally specify a pointer to an object in 'objOut', which will be filled in with
 * the single object in the query result set (or the empty object, if the result set is empty). If
 * 'findCommand' doesn't have these options set, then nullptr must be passed for 'objOut'.
 *
 * On error, throws a AssertionException.
 */
int runQueryWithReadCommands(DBClientBase* conn,
                             const boost::optional<LogicalSessionIdToClient>& lsid,
                             boost::optional<TxnNumber> txnNumber,
                             std::unique_ptr<FindCommandRequest> findCommand,
                             Milliseconds delayBeforeGetMore,
                             BSONObj readPrefObj,
                             BSONObj* objOut) {
    const auto dbName =
        findCommand->getNamespaceOrUUID().nss().value_or(NamespaceString()).db().toString();

    BSONObj findCommandResult;
    BSONObj findCommandObj = findCommand->toBSON(readPrefObj);
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "find command failed; reply was: " << findCommandResult,
            runCommandWithSession(
                conn,
                dbName,
                findCommandObj,
                // read command with txnNumber implies performing reads in a
                // multi-statement transaction
                txnNumber ? kStartTransactionOption | kMultiStatementTransactionOption : kNoOptions,
                lsid,
                txnNumber,
                &findCommandResult));

    CursorResponse cursorResponse =
        uassertStatusOK(CursorResponse::parseFromBSON(findCommandResult));
    int count = cursorResponse.getBatch().size();

    if (objOut) {
        invariant(findCommand->getLimit() && *findCommand->getLimit() == 1 &&
                  findCommand->getSingleBatch());
        // Since this is a "single batch" query, we can simply grab the first item in the result set
        // and return here.
        *objOut = (count > 0) ? cursorResponse.getBatch()[0] : BSONObj();
        return count;
    }

    while (cursorResponse.getCursorId() != 0) {
        sleepFor(delayBeforeGetMore);

        GetMoreCommandRequest getMoreRequest(
            cursorResponse.getCursorId(),
            findCommand->getNamespaceOrUUID().nss().value_or(NamespaceString()).coll().toString());
        getMoreRequest.setBatchSize(findCommand->getBatchSize());
        BSONObj getMoreCommandResult;
        uassert(ErrorCodes::CommandFailed,
                str::stream() << "getMore command failed; reply was: " << getMoreCommandResult,
                runCommandWithSession(conn,
                                      dbName,
                                      getMoreRequest.toBSON({}),
                                      // read command with txnNumber implies performing reads in a
                                      // multi-statement transaction
                                      txnNumber ? kMultiStatementTransactionOption : kNoOptions,
                                      lsid,
                                      txnNumber,
                                      &getMoreCommandResult));

        cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(getMoreCommandResult));
        count += cursorResponse.getBatch().size();
    }

    return count;
}

void doNothing(const BSONObj&) {}

/**
 * Queries the oplog for the latest cluster time / timestamp and returns it.
 */
Timestamp getLatestClusterTime(DBClientBase* conn) {
    // Sort by descending 'ts' in the query to the oplog collection. The first entry will have the
    // latest cluster time.
    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceString("local.oplog.rs"));
    findCommand->setSort(BSON("$natural" << -1));
    findCommand->setLimit(1LL);
    findCommand->setSingleBatch(true);
    invariant(query_request_helper::validateFindCommandRequest(*findCommand));
    const auto dbName =
        findCommand->getNamespaceOrUUID().nss().value_or(NamespaceString()).db().toString();

    BSONObj oplogResult;
    int count = runQueryWithReadCommands(conn,
                                         boost::none,
                                         boost::none,
                                         std::move(findCommand),
                                         Milliseconds(0),
                                         BSONObj(),
                                         &oplogResult);
    uassert(ErrorCodes::OperationFailed,
            str::stream() << "Find cmd on the oplog collection failed; reply was: " << oplogResult,
            count == 1);

    const BSONElement tsElem = oplogResult["ts"];
    uassert(ErrorCodes::BadValue,
            str::stream() << "Expects oplog entry to have a valid 'ts' field: " << oplogResult,
            !tsElem.eoo() || tsElem.type() == bsonTimestamp);
    return tsElem.timestamp();
}

/**
 * Gets the latest timestamp/clusterTime T from the oplog collection, then returns a cluster time
 * 'numSecondsInThePast' earlier than that time T.
 *
 * 'numSecondsInThePast' must be greater than or equal to 0.
 */
Timestamp getAClusterTimeSecondsInThePast(DBClientBase* conn, int numSecondsInThePast) {
    invariant(numSecondsInThePast >= 0);
    Timestamp latestTimestamp = getLatestClusterTime(conn);
    return Timestamp(latestTimestamp.getSecs() - numSecondsInThePast, latestTimestamp.getInc());
}

}  // namespace

BenchRunEventCounter::BenchRunEventCounter() = default;

void BenchRunEventCounter::updateFrom(const BenchRunEventCounter& other) {
    _numEvents += other._numEvents;
    _totalTimeMicros += other._totalTimeMicros;
}

void BenchRunStats::updateFrom(const BenchRunStats& other) {
    error = other.error;

    errCount += other.errCount;
    opCount += other.opCount;

    findOneCounter.updateFrom(other.findOneCounter);
    updateCounter.updateFrom(other.updateCounter);
    insertCounter.updateFrom(other.insertCounter);
    deleteCounter.updateFrom(other.deleteCounter);
    queryCounter.updateFrom(other.queryCounter);
    commandCounter.updateFrom(other.commandCounter);

    for (const auto& trappedError : other.trappedErrors) {
        trappedErrors.push_back(trappedError);
    }
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
    auto config = std::make_unique<BenchRunConfig>();
    config->initializeFromBson(args);
    return config.release();
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
        } else if (name == "command") {
            // type needs to be command
            uassert(34398,
                    str::stream() << "Field 'command' only valid for command op type. Type is "
                                  << opType,
                    opType == "command");
            myOp.command = arg.Obj();
        } else if (name == "context") {
            myOp.context = arg.Obj();
        } else if (name == "cpuFactor") {
            uassert(40436,
                    str::stream() << "Field 'cpuFactor' should be a number, instead it's type: "
                                  << typeName(arg.type()),
                    arg.isNumber());
            myOp.cpuFactor = arg.numberDouble();
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
            } else if (type == "cpuload") {
                myOp.op = OpType::CPULOAD;
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
        } else if (name == "sort") {
            uassert(ErrorCodes::BadValue,
                    str::stream()
                        << "Field 'sort' is only valid for query, fineOne and find. Op type is "
                        << opType,
                    (opType == "findOne") || (opType == "query") || (opType == "find"));
            uassert(ErrorCodes::BadValue,
                    "Expected sort to be an object",
                    arg.type() == BSONType::Object);
            myOp.sort = arg.Obj();
        } else if (name == "showError") {
            myOp.showError = arg.trueValue();
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
            myOp.update = write_ops::UpdateModification::parseFromBSON(arg);
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
        } else if (name == "useAClusterTimeWithinPastSeconds") {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Field 'useAClusterTimeWithinPastSeconds' should be a number, "
                                     "instead it's type: "
                                  << typeName(arg.type()),
                    arg.isNumber());
            uassert(ErrorCodes::BadValue,
                    str::stream() << "field 'useAClusterTimeWithinPastSeconds' cannot be a "
                                     "negative value. Value is: "
                                  << arg.numberInt(),
                    arg.numberInt() >= 0);
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "Field 'useAClusterTimeWithinPastSeconds' is only valid for "
                                     "find op types. Type is "
                                  << opType,
                    (opType == "find") || (opType == "query"));
            myOp.useAClusterTimeWithinPastSeconds = arg.numberInt();
        } else if (name == "maxRandomMillisecondDelayBeforeGetMore") {
            uassert(ErrorCodes::BadValue,
                    str::stream()
                        << "Field 'maxRandomMillisecondDelayBeforeGetMore' should be a number, "
                           "instead it's type: "
                        << typeName(arg.type()),
                    arg.isNumber());
            uassert(ErrorCodes::BadValue,
                    str::stream() << "field 'maxRandomMillisecondDelayBeforeGetMore' cannot be a "
                                     "negative value. Value is: "
                                  << arg.numberInt(),
                    arg.numberInt() >= 0);
            uassert(
                ErrorCodes::InvalidOptions,
                str::stream() << "Field 'maxRandomMillisecondDelayBeforeGetMore' is only valid for "
                                 "find op types. Type is "
                              << opType,
                (opType == "find") || (opType == "query"));
            myOp.maxRandomMillisecondDelayBeforeGetMore = arg.numberInt();
        } else if (name == "readPrefMode") {
            uassert(
                ErrorCodes::InvalidOptions,
                str::stream() << "Field 'readPrefMode' is only valid for find op types. Type is "
                              << opType,
                (opType == "find") || (opType == "query") || (opType == "findOne"));
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Field 'readPrefMode' should be a string, instead it's type: "
                                  << typeName(arg.type()),
                    arg.type() == BSONType::String);

            ReadPreference mode;
            try {
                mode = ReadPreference_parse(IDLParserContext("mode"), arg.str());
            } catch (DBException& e) {
                e.addContext("benchRun(): Could not parse readPrefMode argument");
                throw;
            }

            myOp.readPrefObj = ReadPreferenceSetting(mode).toContainingBSON();
        } else {
            uassert(34394, str::stream() << "Benchrun op has unsupported field: " << name, false);
        }
    }

    uassert(5751400,
            "readCmd: true must be specified for read ops",
            !isReadOp(myOp.op) || myOp.useReadCmd);
    uassert(5751401,
            "writeCmd: true must be specified for write ops",
            !isWriteOp(myOp.op) || myOp.useWriteCmd);
    uassert(5751402, "Exhaust queries are not supported", !(myOp.options & QueryOption_Exhaust));

    uassert(34395, "Benchrun op has an zero length ns", !myOp.ns.empty());
    uassert(34396, "Benchrun op doesn't have an optype set", myOp.op != OpType::NONE);

    return myOp;
}

void BenchRunConfig::initializeFromBson(const BSONObj& args) {
    initializeToDefaults();

    auto argToRegex = [](auto&& arg) {
        return std::make_shared<pcre::Regex>(arg.regex(),
                                             pcre_util::flagsToOptions(arg.regexFlags()));
    };

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
        } else if (name == "useSessions") {
            uassert(40641,
                    str::stream() << "Field '" << name << "' should be a boolean. . Type is "
                                  << typeName(arg.type()),
                    arg.isBoolean());
            useSessions = arg.boolean();
        } else if (name == "useIdempotentWrites") {
            uassert(40642,
                    str::stream() << "Field '" << name << "' should be a boolean. . Type is "
                                  << typeName(arg.type()),
                    arg.isBoolean());
            useIdempotentWrites = arg.boolean();
        } else if (name == "useSnapshotReads") {
            uassert(50707,
                    str::stream() << "Field '" << name << "' should be a boolean. Type is "
                                  << typeName(arg.type()),
                    arg.isBoolean());
            useSnapshotReads = arg.boolean();
        } else if (name == "delayMillisOnFailedOperation") {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Field '" << name << "' should be a number. Type is "
                                  << typeName(arg.type()),
                    arg.isNumber());
            delayMillisOnFailedOperation = Milliseconds(arg.numberInt());
        } else if (name == "handleErrors") {
            handleErrors = arg.trueValue();
        } else if (name == "hideErrors") {
            hideErrors = arg.trueValue();
        } else if (name == "throwGLE") {
            throwGLE = arg.trueValue();
        } else if (name == "breakOnTrap") {
            breakOnTrap = arg.trueValue();
        } else if (name == "trapPattern") {
            trapPattern = argToRegex(arg);
        } else if (name == "noTrapPattern") {
            noTrapPattern = argToRegex(arg);
        } else if (name == "watchPattern") {
            watchPattern = argToRegex(arg);
        } else if (name == "noWatchPattern") {
            noWatchPattern = argToRegex(arg);
        } else if (name == "ops") {
            // iterate through the objects in ops
            // create an BenchRunOp per
            // put in ops vector.
            BSONObjIterator i(arg.Obj());
            while (i.more()) {
                ops.push_back(opFromBson(i.next().Obj()));
            }
        } else {
            LOGV2_INFO(22793, "benchRun passed an unsupported field", "name"_attr = name);
            uassert(34376, "benchRun passed an unsupported configuration field", false);
        }
    }
}

std::unique_ptr<DBClientBase> BenchRunConfig::createConnectionImpl(
    const BenchRunConfig& benchRunConfig) {
    static auto w = MONGO_WEAK_FUNCTION_DEFINITION(BenchRunConfig::createConnectionImpl);
    return w(benchRunConfig);
}

std::unique_ptr<DBClientBase> BenchRunConfig::createConnection() const {
    return BenchRunConfig::createConnectionImpl(*this);
}

BenchRunState::BenchRunState(unsigned numWorkers)
    : _mutex(),
      _numUnstartedWorkers(numWorkers),
      _numActiveWorkers(0),
      _isShuttingDown(0),
      _isCollectingStats(0) {}

BenchRunState::~BenchRunState() {
    if (_numActiveWorkers != 0)
        LOGV2_WARNING(22802, "Destroying BenchRunState with active workers");
    if (_numUnstartedWorkers != 0)
        LOGV2_WARNING(22803, "Destroying BenchRunState with unstarted workers");
}

void BenchRunState::waitForState(State awaitedState) {
    stdx::unique_lock<Latch> lk(_mutex);

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
            msgasserted(16152, str::stream() << "Cannot wait for state " << awaitedState);
    }
}

void BenchRunState::tellWorkersToFinish() {
    _isShuttingDown.store(1);
}

void BenchRunState::tellWorkersToCollectStats() {
    _isCollectingStats.store(1);
}

void BenchRunState::assertFinished() const {
    stdx::lock_guard<Latch> lk(_mutex);
    verify(0 == _numUnstartedWorkers + _numActiveWorkers);
}

bool BenchRunState::shouldWorkerFinish() const {
    return (_isShuttingDown.loadRelaxed() == 1);
}

bool BenchRunState::shouldWorkerCollectStats() const {
    return (_isCollectingStats.loadRelaxed() == 1);
}

void BenchRunState::onWorkerStarted() {
    stdx::lock_guard<Latch> lk(_mutex);
    verify(_numUnstartedWorkers > 0);
    --_numUnstartedWorkers;
    ++_numActiveWorkers;
    if (_numUnstartedWorkers == 0) {
        _stateChangeCondition.notify_all();
    }
}

void BenchRunState::onWorkerFinished() {
    stdx::lock_guard<Latch> lk(_mutex);
    verify(_numActiveWorkers > 0);
    --_numActiveWorkers;
    if (_numActiveWorkers + _numUnstartedWorkers == 0) {
        _stateChangeCondition.notify_all();
    }
}

BenchRunWorker::BenchRunWorker(size_t id,
                               const BenchRunConfig* config,
                               BenchRunState& brState,
                               int64_t randomSeed)
    : _id(id), _config(config), _brState(brState), _rng(randomSeed) {}

BenchRunWorker::~BenchRunWorker() {
    try {
        // We explicitly call join() on the started thread to ensure
        // that any thread-local variables have been destructed
        // before returning from BenchRunWorker's destructor.
        _thread.join();
    } catch (...) {
        LOGV2_FATAL_CONTINUE(
            22807, "Caught exception in destructor", "error"_attr = exceptionToStatus());
        std::terminate();
    }
}

void BenchRunWorker::start() {
    _thread = stdx::thread([this] { run(); });
}

bool BenchRunWorker::shouldStop() const {
    return _brState.shouldWorkerFinish();
}

bool BenchRunWorker::shouldCollectStats() const {
    return _brState.shouldWorkerCollectStats();
}

void BenchRunWorker::generateLoadOnConnection(DBClientBase* conn) {
    verify(conn);
    long long count = 0;
    Timer timer;

    BsonTemplateEvaluator bsonTemplateEvaluator(_rng.nextInt32());
    invariant(bsonTemplateEvaluator.setId(_id) == BsonTemplateEvaluator::StatusSuccess);

    if (_config->username != "") {
        std::string errmsg;
        uassert(15931,
                str::stream() << "Authenticating to connection for _benchThread failed: " << errmsg,
                conn->auth("admin", _config->username, _config->password, errmsg));
    }

    boost::optional<LogicalSessionIdToClient> lsid;
    if (_config->useSessions) {
        BSONObj result;
        uassert(40640,
                str::stream() << "Unable to create session due to error " << result,
                conn->runCommand("admin", BSON("startSession" << 1), result));

        lsid.emplace(LogicalSessionIdToClient::parse(IDLParserContext("lsid"), result["id"].Obj()));
    }

    BenchRunOp::State opState(&_rng, &bsonTemplateEvaluator, &_statsBlackHole);

    ON_BLOCK_EXIT([&] {
        // Executing the transaction with a new txnNumber would end the previous transaction
        // automatically, but we have to end the last transaction manually with an abort command.
        if (opState.inProgressMultiStatementTxn) {
            abortTransaction(conn, lsid, opState.txnNumber);
        }
    });

    while (!shouldStop()) {
        for (const auto& op : _config->ops) {
            if (shouldStop())
                break;

            opState.stats = shouldCollectStats() ? &_stats : &_statsBlackHole;

            try {
                op.executeOnce(conn, lsid, *_config, &opState);
            } catch (const DBException& ex) {
                if (!_config->hideErrors || op.showError) {
                    bool yesWatch = (_config->watchPattern &&
                                     _config->watchPattern->matchView(
                                         ex.what(), pcre::ANCHORED | pcre::ENDANCHORED));
                    bool noWatch = (_config->noWatchPattern &&
                                    _config->noWatchPattern->matchView(
                                        ex.what(), pcre::ANCHORED | pcre::ENDANCHORED));

                    if ((!_config->watchPattern && _config->noWatchPattern &&
                         !noWatch) ||  // If we're just ignoring things
                        (!_config->noWatchPattern && _config->watchPattern &&
                         yesWatch) ||  // If we're just watching things
                        (_config->watchPattern && _config->noWatchPattern && yesWatch && !noWatch))
                        LOGV2_INFO(22794,
                                   "Error in benchRun thread for op",
                                   "op"_attr = kOpTypeNames.find(op.op)->second,
                                   "error"_attr = causedBy(ex));
                }

                bool yesTrap = (_config->trapPattern &&
                                _config->trapPattern->matchView(
                                    ex.what(), pcre::ANCHORED | pcre::ENDANCHORED));
                bool noTrap = (_config->noTrapPattern &&
                               _config->noTrapPattern->matchView(
                                   ex.what(), pcre::ANCHORED | pcre::ENDANCHORED));

                if ((!_config->trapPattern && _config->noTrapPattern && !noTrap) ||
                    (!_config->noTrapPattern && _config->trapPattern && yesTrap) ||
                    (_config->trapPattern && _config->noTrapPattern && yesTrap && !noTrap)) {
                    {
                        opState.stats->trappedErrors.push_back(
                            BSON("error" << ex.what() << "op" << kOpTypeNames.find(op.op)->second
                                         << "count" << count));
                    }
                    if (_config->breakOnTrap)
                        return;
                }
                if (!_config->handleErrors && !op.handleError)
                    throw;

                sleepFor(_config->delayMillisOnFailedOperation);

                ++opState.stats->errCount;
            } catch (...) {
                if (!_config->hideErrors || op.showError)
                    LOGV2_INFO(22795,
                               "Error in benchRun thread caused by unknown error for op",
                               "op"_attr = kOpTypeNames.find(op.op)->second);
                if (!_config->handleErrors && !op.handleError)
                    return;

                ++opState.stats->errCount;
            }

            if (op.delay > 0)
                sleepmillis(op.delay);
        }
    }
}

void BenchRunOp::executeOnce(DBClientBase* conn,
                             const boost::optional<LogicalSessionIdToClient>& lsid,
                             const BenchRunConfig& config,
                             State* state) const {
    switch (this->op) {
        case OpType::NOP:
            break;
        case OpType::CPULOAD: {
            // perform a tight multiplication loop. The
            // performance of this loop should be
            // predictable, and this operation can be used
            // to test underlying system variability.
            long long limit = 10000 * this->cpuFactor;
            // volatile used to ensure that loop is not optimized away
            volatile uint64_t result [[maybe_unused]] = 0;  // NOLINT
            uint64_t x = 100;
            for (long long i = 0; i < limit; i++) {
                x *= 13;
            }
            result = x;
        } break;
        case OpType::FINDONE: {
            BSONObj fixedQuery = fixQuery(this->query, *state->bsonTemplateEvaluator);
            BSONObj result;
            auto findCommand = std::make_unique<FindCommandRequest>(NamespaceString(this->ns));
            findCommand->setFilter(fixedQuery);
            findCommand->setProjection(this->projection);
            findCommand->setLimit(1LL);
            findCommand->setSingleBatch(true);
            findCommand->setSort(this->sort);
            if (config.useSnapshotReads) {
                findCommand->setReadConcern(readConcernSnapshot);
            }
            invariant(query_request_helper::validateFindCommandRequest(*findCommand));

            BenchRunEventTrace _bret(&state->stats->findOneCounter);
            boost::optional<TxnNumber> txnNumberForOp;
            if (config.useSnapshotReads) {
                ++state->txnNumber;
                txnNumberForOp = state->txnNumber;
                state->inProgressMultiStatementTxn = true;
            }
            runQueryWithReadCommands(conn,
                                     lsid,
                                     txnNumberForOp,
                                     std::move(findCommand),
                                     Milliseconds(0),
                                     readPrefObj,
                                     &result);
            LOGV2_DEBUG(22796, 5, "Result from benchRun thread [findOne]", "result"_attr = result);
        } break;
        case OpType::COMMAND: {
            bool ok;
            BSONObj result;
            {
                BenchRunEventTrace _bret(&state->stats->commandCounter);
                ok = runCommandWithSession(conn,
                                           this->ns,
                                           fixQuery(this->command, *state->bsonTemplateEvaluator),
                                           this->options,
                                           lsid,
                                           &result);
            }
            if (!ok) {
                ++state->stats->errCount;
            }

            if (!result["cursor"].eoo()) {
                // The command returned a cursor, so iterate all results.
                auto cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(result));
                int count = cursorResponse.getBatch().size();
                while (cursorResponse.getCursorId() != 0) {
                    GetMoreCommandRequest getMoreRequest(cursorResponse.getCursorId(),
                                                         cursorResponse.getNSS().coll().toString());
                    BSONObj getMoreCommandResult;
                    uassert(ErrorCodes::CommandFailed,
                            str::stream()
                                << "getMore command failed; reply was: " << getMoreCommandResult,
                            runCommandWithSession(conn,
                                                  this->ns,
                                                  getMoreRequest.toBSON({}),
                                                  kNoOptions,
                                                  lsid,
                                                  &getMoreCommandResult));
                    cursorResponse =
                        uassertStatusOK(CursorResponse::parseFromBSON(getMoreCommandResult));
                    count += cursorResponse.getBatch().size();
                }
                // Just give the count to the check function.
                result = BSON("count" << count << "context" << this->context);
            }
        } break;
        case OpType::FIND: {
            int count;

            BSONObj fixedQuery = fixQuery(this->query, *state->bsonTemplateEvaluator);

            uassert(
                28824, "cannot use 'options' in combination with read commands", !this->options);

            auto findCommand = std::make_unique<FindCommandRequest>(NamespaceString(this->ns));
            findCommand->setFilter(fixedQuery);
            findCommand->setProjection(this->projection);
            if (this->skip) {
                findCommand->setSkip(this->skip);
            }
            if (this->limit) {
                findCommand->setLimit(this->limit);
            }
            if (this->batchSize) {
                findCommand->setBatchSize(this->batchSize);
            }
            if (!this->sort.isEmpty()) {
                findCommand->setSort(this->sort);
            }
            BSONObjBuilder readConcernBuilder;
            if (config.useSnapshotReads) {
                readConcernBuilder.append("level", "snapshot");
            }
            if (this->useAClusterTimeWithinPastSeconds > 0) {
                invariant(config.useSnapshotReads);
                // Get a random cluster time between the latest time and
                // 'useAClusterTimeWithinPastSeconds' in the past.
                Timestamp atClusterTime = getAClusterTimeSecondsInThePast(
                    conn, state->rng->nextInt32(this->useAClusterTimeWithinPastSeconds));
                readConcernBuilder.append("atClusterTime", atClusterTime);
            }
            findCommand->setReadConcern(readConcernBuilder.obj());

            invariant(query_request_helper::validateFindCommandRequest(*findCommand));

            BenchRunEventTrace _bret(&state->stats->queryCounter);
            boost::optional<TxnNumber> txnNumberForOp;
            if (config.useSnapshotReads) {
                ++state->txnNumber;
                txnNumberForOp = state->txnNumber;
                state->inProgressMultiStatementTxn = true;
            }

            int delayBeforeGetMore = this->maxRandomMillisecondDelayBeforeGetMore
                ? state->rng->nextInt32(this->maxRandomMillisecondDelayBeforeGetMore)
                : 0;

            count = runQueryWithReadCommands(conn,
                                             lsid,
                                             txnNumberForOp,
                                             std::move(findCommand),
                                             Milliseconds(delayBeforeGetMore),
                                             readPrefObj,
                                             nullptr);

            if (this->expected >= 0 && count != this->expected) {
                LOGV2_INFO(22797,
                           "Bench query on: {namespace} expected: {expected} got: {got}",
                           "Bench query on namespace got diffrent results then expected",
                           "namespace"_attr = this->ns,
                           "expected"_attr = this->expected,
                           "got"_attr = count);
                verify(false);
            }
            LOGV2_DEBUG(22798, 5, "Result from benchRun thread [query]", "count"_attr = count);
        } break;
        case OpType::UPDATE: {
            BSONObj result;
            {
                BenchRunEventTrace _bret(&state->stats->updateCounter);
                BSONObj query = fixQuery(this->query, *state->bsonTemplateEvaluator);

                BSONObjBuilder builder;
                builder.append("update", nsToCollectionSubstring(this->ns));
                BSONArrayBuilder updateArray(builder.subarrayStart("updates"));
                {
                    BSONObjBuilder singleUpdate;
                    singleUpdate.append("q", query);
                    switch (this->update.type()) {
                        case write_ops::UpdateModification::Type::kReplacement: {
                            singleUpdate.append("u",
                                                fixQuery(this->update.getUpdateReplacement(),
                                                         *state->bsonTemplateEvaluator));
                            break;
                        }
                        case write_ops::UpdateModification::Type::kModifier: {
                            singleUpdate.append("u",
                                                fixQuery(this->update.getUpdateModifier(),
                                                         *state->bsonTemplateEvaluator));
                            break;
                        }
                        case write_ops::UpdateModification::Type::kPipeline: {
                            BSONArrayBuilder pipelineBuilder(singleUpdate.subarrayStart("u"));
                            for (auto&& stage : this->update.getUpdatePipeline()) {
                                pipelineBuilder.append(
                                    fixQuery(stage, *state->bsonTemplateEvaluator));
                            }
                            pipelineBuilder.doneFast();
                            break;
                        }
                        case write_ops::UpdateModification::Type::kDelta:
                            // It is not possible to run a delta update directly from a client.
                            // Delta updates are only executed on secondaries as part of oplog
                            // application.
                            MONGO_UNREACHABLE;
                        case write_ops::UpdateModification::Type::kTransform:
                            // It is not possible to run a transform update directly from a client.
                            MONGO_UNREACHABLE;
                    }
                    singleUpdate.append("multi", this->multi);
                    singleUpdate.append("upsert", this->upsert);
                    updateArray.append(singleUpdate.done());
                }
                updateArray.doneFast();
                builder.append("writeConcern", this->writeConcern);

                boost::optional<TxnNumber> txnNumberForOp;
                if (config.useIdempotentWrites) {
                    ++state->txnNumber;
                    txnNumberForOp = state->txnNumber;
                }
                runCommandWithSession(conn,
                                      nsToDatabaseSubstring(this->ns).toString(),
                                      builder.done(),
                                      kNoOptions,
                                      lsid,
                                      txnNumberForOp,
                                      &result);
            }
            LOGV2_DEBUG(
                22799, 5, "Result from benchRun thread [safe update]", "result"_attr = result);
        } break;
        case OpType::INSERT: {
            BSONObj result;
            {
                BenchRunEventTrace _bret(&state->stats->insertCounter);

                BSONObj insertDoc;
                BSONObjBuilder builder;
                builder.append("insert", nsToCollectionSubstring(this->ns));
                BSONArrayBuilder docBuilder(builder.subarrayStart("documents"));
                if (this->isDocAnArray) {
                    for (const auto& element : this->doc) {
                        insertDoc = fixQuery(element.Obj(), *state->bsonTemplateEvaluator);
                        docBuilder.append(insertDoc);
                    }
                } else {
                    insertDoc = fixQuery(this->doc, *state->bsonTemplateEvaluator);
                    docBuilder.append(insertDoc);
                }
                docBuilder.done();
                builder.append("writeConcern", this->writeConcern);

                boost::optional<TxnNumber> txnNumberForOp;
                if (config.useIdempotentWrites) {
                    ++state->txnNumber;
                    txnNumberForOp = state->txnNumber;
                }
                runCommandWithSession(conn,
                                      nsToDatabaseSubstring(this->ns).toString(),
                                      builder.done(),
                                      kNoOptions,
                                      lsid,
                                      txnNumberForOp,
                                      &result);
            }
            LOGV2_DEBUG(
                22800, 5, "Result from benchRun thread [safe insert]", "result"_attr = result);
        } break;
        case OpType::REMOVE: {
            BSONObj result;
            {
                BenchRunEventTrace _bret(&state->stats->deleteCounter);
                BSONObj predicate = fixQuery(this->query, *state->bsonTemplateEvaluator);
                BSONObjBuilder builder;
                builder.append("delete", nsToCollectionSubstring(this->ns));
                BSONArrayBuilder docBuilder(builder.subarrayStart("deletes"));
                int limit = (this->multi == true) ? 0 : 1;
                docBuilder.append(BSON("q" << predicate << "limit" << limit));
                docBuilder.done();
                builder.append("writeConcern", this->writeConcern);

                boost::optional<TxnNumber> txnNumberForOp;
                if (config.useIdempotentWrites) {
                    ++state->txnNumber;
                    txnNumberForOp = state->txnNumber;
                }
                runCommandWithSession(conn,
                                      nsToDatabaseSubstring(this->ns).toString(),
                                      builder.done(),
                                      kNoOptions,
                                      lsid,
                                      txnNumberForOp,
                                      &result);
            }
            LOGV2_DEBUG(
                22801, 5, "Result from benchRun thread [safe remove]", "result"_attr = result);
        } break;
        case OpType::CREATEINDEX:
            conn->createIndex(this->ns, this->key);
            break;
        case OpType::DROPINDEX:
            conn->dropIndex(this->ns, this->key);
            break;
        case OpType::LET: {
            BSONObjBuilder templateBuilder;
            state->bsonTemplateEvaluator->evaluate(this->value, templateBuilder);
            state->bsonTemplateEvaluator->setVariable(this->target,
                                                      templateBuilder.done().firstElement());
        } break;
        default:
            uassert(34397, "In benchRun loop and got unknown op type", false);
    }

    // Count 1 for total ops. Successfully got through the try phrase
    ++state->stats->opCount;
}

void BenchRunWorker::run() {
    try {
        auto conn(_config->createConnection());

        if (!_config->username.empty()) {
            std::string errmsg;
            if (!conn->auth("admin", _config->username, _config->password, errmsg)) {
                uasserted(15932, "Authenticating to connection for benchThread failed: " + errmsg);
            }
        }

        BenchRunWorkerStateGuard workerStateGuard(_brState);
        generateLoadOnConnection(conn.get());
    } catch (const DBException& e) {
        LOGV2_ERROR(
            22804, "DBException not handled in benchRun thread", "error"_attr = causedBy(e));
    } catch (const std::exception& e) {
        LOGV2_ERROR(
            22805, "std::exception not handled in benchRun thread", "error"_attr = causedBy(e));
    } catch (...) {
        LOGV2_ERROR(22806, "Unknown exception not handled in benchRun thread.");
    }
}

BenchRunner::BenchRunner(BenchRunConfig* config) : _brState(config->parallel), _config(config) {
    _oid.init();

    stdx::lock_guard<Latch> lk(_staticMutex);
    _activeRuns[_oid] = this;
}

BenchRunner::~BenchRunner() = default;

void BenchRunner::start() {
    {
        std::unique_ptr<DBClientBase> conn(_config->createConnection());
        // Must authenticate to admin db in order to run serverStatus command
        if (_config->username != "") {
            std::string errmsg;
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
            const int64_t seed = _config->randomSeed + i;

            auto worker = std::make_unique<BenchRunWorker>(i, _config.get(), _brState, seed);
            worker->start();

            _workers.push_back(std::move(worker));
        }

        _brState.waitForState(BenchRunState::BRS_RUNNING);

        // initial stats
        _brState.tellWorkersToCollectStats();
        _brTimer.emplace();
    }
}

void BenchRunner::stop() {
    _brState.tellWorkersToFinish();
    _brState.waitForState(BenchRunState::BRS_FINISHED);
    _microsElapsed = _brTimer->micros();
    _brTimer.reset();

    {
        std::unique_ptr<DBClientBase> conn(_config->createConnection());
        if (_config->username != "") {
            std::string errmsg;
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
        stdx::lock_guard<Latch> lk(_staticMutex);
        _activeRuns.erase(_oid);
    }
}

BenchRunner* BenchRunner::createWithConfig(const BSONObj& configArgs) {
    BenchRunConfig* config = BenchRunConfig::createFromBson(configArgs);
    return new BenchRunner(config);
}

BenchRunner* BenchRunner::get(OID oid) {
    stdx::lock_guard<Latch> lk(_staticMutex);
    return _activeRuns[oid];
}

BenchRunStats BenchRunner::gatherStats() const {
    _brState.assertFinished();

    BenchRunStats stats;

    for (size_t i = 0; i < _workers.size(); ++i) {
        stats.updateFrom(_workers[i]->stats());
    }

    return stats;
}

BSONObj BenchRunner::finish(BenchRunner* runner) {
    runner->stop();

    const auto stats(runner->gatherStats());

    const bool error = stats.error;
    if (error) {
        return BSON("err" << 1);
    }

    BSONObjBuilder buf;
    buf.append("note", "values per second");
    buf.append("errCount", static_cast<long long>(stats.errCount));
    buf.append("trapped", "error: not implemented");

    const auto appendAverageMicrosIfAvailable = [&buf](StringData name,
                                                       const BenchRunEventCounter& counter) {
        if (counter.getNumEvents() > 0) {
            buf.append(name,
                       static_cast<double>(counter.getTotalTimeMicros()) / counter.getNumEvents());
        }
    };

    appendAverageMicrosIfAvailable("findOneLatencyAverageMicros", stats.findOneCounter);
    appendAverageMicrosIfAvailable("insertLatencyAverageMicros", stats.insertCounter);
    appendAverageMicrosIfAvailable("deleteLatencyAverageMicros", stats.deleteCounter);
    appendAverageMicrosIfAvailable("updateLatencyAverageMicros", stats.updateCounter);
    appendAverageMicrosIfAvailable("queryLatencyAverageMicros", stats.queryCounter);
    appendAverageMicrosIfAvailable("commandsLatencyAverageMicros", stats.commandCounter);

    buf.append("totalOps", static_cast<long long>(stats.opCount));

    const auto appendPerSec = [&buf, runner](StringData name, double total) {
        buf.append(name, total / (runner->_microsElapsed / 1000000.0));
    };

    appendPerSec("totalOps/s", stats.opCount);
    appendPerSec("findOne", stats.findOneCounter.getNumEvents());
    appendPerSec("insert", stats.insertCounter.getNumEvents());
    appendPerSec("delete", stats.deleteCounter.getNumEvents());
    appendPerSec("update", stats.updateCounter.getNumEvents());
    appendPerSec("query", stats.queryCounter.getNumEvents());
    appendPerSec("command", stats.commandCounter.getNumEvents());

    buf.append("findOnes", stats.findOneCounter.getNumEvents());
    buf.append("inserts", stats.insertCounter.getNumEvents());
    buf.append("deletes", stats.deleteCounter.getNumEvents());
    buf.append("updates", stats.updateCounter.getNumEvents());
    buf.append("queries", stats.queryCounter.getNumEvents());
    buf.append("commands", stats.commandCounter.getNumEvents());

    BSONObj zoo = buf.obj();

    delete runner;
    return zoo;
}

Mutex BenchRunner::_staticMutex = MONGO_MAKE_LATCH("BenchRunner");
std::map<OID, BenchRunner*> BenchRunner::_activeRuns;

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
