/**
 * Tests for the write assertion functions in mongo/shell/assert.js.
 */
(() => {
    "use strict";
    const conn = MongoRunner.runMongod();
    const db = conn.getDB("writeAssertions");
    assert.neq(null, conn, "mongodb was unable to start up");
    const tests = [];

    function setup() {
        db.coll.drop();
    }

    function _doFailedWrite(collection) {
        const duplicateId = 42;

        const res = collection.insert({_id: duplicateId});
        assert.writeOK(res, "write to collection should have been successful");
        const failedRes = collection.insert({_id: duplicateId});
        assert.writeError(failedRes, "duplicate key write should have failed");
        return failedRes;
    }

    /* writeOK tests */
    tests.push(function writeOKSuccessfulWriteDoesNotCallMsgFunction() {
        var msgFunctionCalled = false;

        const result = db.coll.insert({data: "hello world"});
        assert.doesNotThrow(() => {
            assert.writeOK(result, () => {
                msgFunctionCalled = true;
            });
        });

        assert.eq(false, msgFunctionCalled, "message function should not have been called");
    });

    tests.push(function writeOKUnsuccessfulWriteDoesCallMsgFunction() {
        var msgFunctionCalled = false;

        const failedResult = _doFailedWrite(db.coll);
        assert.throws(() => {
            assert.writeOK(failedResult, () => {
                msgFunctionCalled = true;
            });
        });

        assert.eq(true, msgFunctionCalled, "message function should have been called");
    });

    /* writeError tests */
    tests.push(function writeErrorSuccessfulWriteDoesCallMsgFunction() {
        var msgFunctionCalled = false;

        const result = db.coll.insert({data: "hello world"});
        assert.throws(() => {
            assert.writeError(result, () => {
                msgFunctionCalled = true;
            });
        });

        assert.eq(true, msgFunctionCalled, "message function should have been called");
    });

    tests.push(function writeErrorUnsuccessfulWriteDoesNotCallMsgFunction() {
        var msgFunctionCalled = false;

        const failedResult = _doFailedWrite(db.coll);
        assert.doesNotThrow(() => {
            assert.writeError(failedResult, () => {
                msgFunctionCalled = true;
            });
        });

        assert.eq(false, msgFunctionCalled, "message function should not have been called");
    });

    /* main */

    tests.forEach((test) => {
        jsTest.log(`Starting tests '${test.name}'`);
        setup();
        test();
    });

    /* cleanup */
    MongoRunner.stopMongod(conn);
})();
