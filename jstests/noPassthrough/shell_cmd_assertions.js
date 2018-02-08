(function() {
    "use strict";
    const conn = MongoRunner.runMongod();
    const db = conn.getDB("commandAssertions");
    const kFakeErrCode = 1234567890;
    const tests = [];

    function setup() {
        db.coll.drop();
        assert.writeOK(db.coll.insert({_id: 1}));
    }

    // Raw command responses.
    tests.push(function rawCommandOk() {
        const res = db.runCommand({"ping": 1});
        assert.doesNotThrow(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.throws(() => assert.commandFailed(res));
        assert.throws(() => assert.commandFailedWithCode(res, 0));
    });

    tests.push(function rawCommandErr() {
        const res = db.runCommand({"IHopeNobodyEverMakesThisACommand": 1});
        assert.throws(() => assert.commandWorked(res));
        assert.throws(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.CommandNotFound));
        // commandFailedWithCode should succeed if any of the passed error codes are matched.
        assert.doesNotThrow(
            () => assert.commandFailedWithCode(res, [ErrorCodes.CommandNotFound, kFakeErrCode]));
    });

    tests.push(function rawCommandWriteOk() {
        const res = db.runCommand({insert: "coll", documents: [{_id: 2}]});
        assert.doesNotThrow(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.throws(() => assert.commandFailed(res));
        assert.throws(() => assert.commandFailedWithCode(res, 0));
    });

    tests.push(function rawCommandWriteErr() {
        const res = db.runCommand({insert: "coll", documents: [{_id: 1}]});
        assert.throws(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
        assert.doesNotThrow(
            () => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
    });

    tests.push(function collInsertWriteOk() {
        const res = db.coll.insert({_id: 2});
        assert(res instanceof WriteResult);
        assert.doesNotThrow(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.throws(() => assert.commandFailed(res));
        assert.throws(() => assert.commandFailedWithCode(res, 0));
    });

    tests.push(function collInsertWriteErr() {
        const res = db.coll.insert({_id: 1});
        assert(res instanceof WriteResult);
        assert.throws(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
        assert.doesNotThrow(
            () => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
    });

    tests.push(function collMultiInsertWriteOk() {
        const res = db.coll.insert([{_id: 3}, {_id: 2}]);
        assert(res instanceof BulkWriteResult);
        assert.doesNotThrow(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.throws(() => assert.commandFailed(res));
        assert.throws(() => assert.commandFailedWithCode(res, 0));
    });

    tests.push(function collMultiInsertWriteErr() {
        const res = db.coll.insert([{_id: 1}, {_id: 2}]);
        assert(res instanceof BulkWriteResult);
        assert.throws(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
        assert.doesNotThrow(
            () => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
    });

    // Test when the insert command fails with ok:0 (i.e. not failing due to write err)
    tests.push(function collInsertCmdErr() {
        const res = db.coll.insert({x: 1}, {writeConcern: {"bad": 1}});
        assert(res instanceof WriteCommandError);
        assert.throws(() => assert.commandWorked(res));
        assert.throws(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.FailedToParse));
        assert.doesNotThrow(
            () => assert.commandFailedWithCode(res, [ErrorCodes.FailedToParse, kFakeErrCode]));
    });

    tests.push(function collMultiInsertCmdErr() {
        const res = db.coll.insert([{x: 1}, {x: 2}], {writeConcern: {"bad": 1}});
        assert(res instanceof WriteCommandError);
        assert.throws(() => assert.commandWorked(res));
        assert.throws(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.FailedToParse));
        assert.doesNotThrow(
            () => assert.commandFailedWithCode(res, [ErrorCodes.FailedToParse, kFakeErrCode]));
    });

    tests.push(function mapReduceOk() {
        const res = db.coll.mapReduce(
            function() {
                emit(this._id, 0);
            },
            function(k, v) {
                return v[0];
            },
            {out: "coll_out"});
        assert(res instanceof MapReduceResult);
        assert.doesNotThrow(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.throws(() => assert.commandFailed(res));
        assert.throws(() => assert.commandFailedWithCode(res, 0));
    });

    tests.push(function mapReduceErr() {
        // db.coll.mapReduce throws if the command response has ok:0
        // Instead manually construct a MapReduceResult with ok:0
        const res = new MapReduceResult(db, {
            "ok": 0,
            "errmsg": "Example Error",
            "code": ErrorCodes.JSInterpreterFailure,
            "codeName": "JSInterpreterFailure"
        });
        assert.throws(() => assert.commandWorked(res));
        assert.throws(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() =>
                                assert.commandFailedWithCode(res, ErrorCodes.JSInterpreterFailure));
        assert.doesNotThrow(() => assert.commandFailedWithCode(
                                res, [ErrorCodes.JSInterpreterFailure, kFakeErrCode]));
    });

    tests.push(function errObject() {
        // Some functions throw an Error with a code property attached.
        let threw = false;
        let res = null;
        try {
            db.eval("this is a syntax error");
        } catch (e) {
            threw = true;
            res = e;
        }
        assert(threw);
        assert(res instanceof Error);
        assert(res.hasOwnProperty("code"));
        assert.throws(() => assert.commandWorked(res));
        assert.throws(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.InternalError));
        assert.doesNotThrow(
            () => assert.commandFailedWithCode(res, [ErrorCodes.InternalError, kFakeErrCode]));
    });

    tests.push(function crudInsertOneOk() {
        const res = db.coll.insertOne({_id: 2});
        assert(res.hasOwnProperty("acknowledged"));
        assert.doesNotThrow(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.throws(() => assert.commandFailed(res));
        assert.throws(() => assert.commandFailedWithCode(res, 0));
    });

    tests.push(function crudInsertOneErr() {
        let threw = false;
        let res = null;
        try {
            db.coll.insertOne({_id: 1});
        } catch (e) {
            threw = true;
            res = e;
        }
        assert(threw);
        assert(res instanceof WriteError);
        assert.throws(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
        assert.doesNotThrow(
            () => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
    });

    tests.push(function crudInsertManyOk() {
        const res = db.coll.insertMany([{_id: 2}, {_id: 3}]);
        assert(res.hasOwnProperty("acknowledged"));
        assert.doesNotThrow(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.throws(() => assert.commandFailed(res));
        assert.throws(() => assert.commandFailedWithCode(res, 0));
    });

    tests.push(function crudInsertManyErr() {
        let threw = false;
        let res = null;
        try {
            db.coll.insertMany([{_id: 1}, {_id: 2}]);
        } catch (e) {
            threw = true;
            res = e;
        }
        assert(threw);
        assert(res instanceof BulkWriteError);
        assert.throws(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
        assert.doesNotThrow(
            () => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
    });

    tests.push(function rawMultiWriteErr() {
        // Do an unordered bulk insert with duplicate keys to produce multiple write errors.
        const res =
            db.runCommand({"insert": "coll", documents: [{_id: 1}, {_id: 1}], ordered: false});
        assert(res.writeErrors.length == 2, "did not get multiple write errors");
        assert.throws(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
        assert.doesNotThrow(
            () => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
    });

    tests.push(function bulkMultiWriteErr() {
        // Do an unordered bulk insert with duplicate keys to produce multiple write errors.
        const res = db.coll.insert([{_id: 1}, {_id: 1}], {ordered: false});
        assert.throws(() => assert.commandWorked(res));
        assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
        assert.doesNotThrow(() => assert.commandFailed(res));
        assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
        assert.doesNotThrow(
            () => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
    });

    tests.forEach((test) => {
        jsTest.log(`Starting test '${test.name}'`);
        setup();
        test();
    });

    MongoRunner.stopMongod(conn);
})();
