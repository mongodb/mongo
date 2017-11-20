/**
 * Tests that the mongo shell doesn't attempt to retry its write operations after downgrading from
 * 3.6 to 3.4.
 */
(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    load("jstests/replsets/rslib.js");

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();

    const db = primary.startSession({retryWrites: true}).getDatabase("test");
    const coll = db.shell_retryable_writes_downgrade;

    function testCommandCanBeRetried(func, {
        expectedLogicalSessionId: expectedLogicalSessionId = true,
        expectedTransactionNumber: expectedTransactionNumber = true
    } = {}) {
        const mongoRunCommandOriginal = Mongo.prototype.runCommand;

        const sentinel = {};
        let cmdObjSeen = sentinel;

        Mongo.prototype.runCommand = function runCommandSpy(dbName, cmdObj, options) {
            cmdObjSeen = cmdObj;
            return mongoRunCommandOriginal.apply(this, arguments);
        };

        try {
            assert.doesNotThrow(func);
        } finally {
            Mongo.prototype.runCommand = mongoRunCommandOriginal;
        }

        if (cmdObjSeen === sentinel) {
            throw new Error("Mongo.prototype.runCommand() was never called: " + func.toString());
        }

        let cmdName = Object.keys(cmdObjSeen)[0];

        // If the command is in a wrapped form, then we look for the actual command object inside
        // the query/$query object.
        if (cmdName === "query" || cmdName === "$query") {
            cmdObjSeen = cmdObjSeen[cmdName];
            cmdName = Object.keys(cmdObjSeen)[0];
        }

        if (expectedLogicalSessionId) {
            assert(cmdObjSeen.hasOwnProperty("lsid"),
                   "Expected operation " + tojson(cmdObjSeen) + " to have a logical session id: " +
                       func.toString());
        } else {
            assert(!cmdObjSeen.hasOwnProperty("lsid"),
                   "Expected operation " + tojson(cmdObjSeen) +
                       " to not have a logical session id: " + func.toString());
        }

        if (expectedTransactionNumber) {
            assert(cmdObjSeen.hasOwnProperty("txnNumber"),
                   "Expected operation " + tojson(cmdObjSeen) +
                       " to be assigned a transaction number since it can be retried: " +
                       func.toString());
        } else {
            assert(!cmdObjSeen.hasOwnProperty("txnNumber"),
                   "Expected operation " + tojson(cmdObjSeen) +
                       " to not be assigned a transaction number since it cannot be retried: " +
                       func.toString());
        }
    }

    testCommandCanBeRetried(function() {
        assert.writeOK(coll.insert({_id: "while fCV=3.6"}));
    });

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    // The server errors on lsid and txnNumber while in featureCompatibilityVersion=3.4.
    testCommandCanBeRetried(function() {
        assert.writeError(coll.insert({_id: "while fCV=3.4"}));
    });

    rst.restart(primary, {binVersion: "3.4"});
    rst.waitForMaster();

    assert(db.getSession().getOptions().shouldRetryWrites(),
           "Re-establishing the connection shouldn't change the state of the SessionOptions");

    // After downgrading to MongoDB 3.4, the mongo shell shouldn't attempt to automatically retry
    // write operations.
    assert.throws(function() {
        coll.insert({_id: "while in binVersion=3.4 and disconnected"});
    });

    // After downgrading to MongoDB 3.4, the mongo shell shouldn't inject an lsid or assign a
    // transaction number to its write requests.
    testCommandCanBeRetried(function() {
        assert.writeOK(coll.insert({_id: "while binVersion=3.4 and reconnected"}));
    }, {expectedLogicalSessionId: false, expectedTransactionNumber: false});

    rst.restart(primary, {binVersion: "latest", noReplSet: true});
    rst.waitForMaster();
    reconnect(primary);

    // When upgrading to MongoDB 3.6 but running as a stand-alone server, the mongo shell should
    // still assign a transaction number to its write requests (per the Driver's specification).
    testCommandCanBeRetried(function() {
        assert.writeError(
            coll.insert({_id: "while binVersion=3.6 as stand-alone and reconnected"}));
    });

    db.getSession().endSession();
    rst.stopSet();
})();
