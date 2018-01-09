/**
 * Tests that write operations executed through the mongo shell's CRUD API are assigned a
 * transaction number so that they can be retried.
 */
(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const db = primary.startSession({retryWrites: true}).getDatabase("test");
    const coll = db.shell_can_retry_writes;

    function testCommandCanBeRetried(func, expected = true) {
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

        assert(cmdObjSeen.hasOwnProperty("lsid"),
               "Expected operation " + tojson(cmdObjSeen) + " to have a logical session id: " +
                   func.toString());

        if (expected) {
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
        coll.insertOne({_id: 0});
    });

    testCommandCanBeRetried(function() {
        coll.updateOne({_id: 0}, {$set: {a: 1}});
    });

    testCommandCanBeRetried(function() {
        coll.updateOne({_id: 1}, {$set: {a: 2}}, {upsert: true});
    });

    testCommandCanBeRetried(function() {
        coll.deleteOne({_id: 1});
    });

    testCommandCanBeRetried(function() {
        coll.insertMany([{_id: 2, b: 3}, {_id: 3, b: 4}], {ordered: true});
    });

    testCommandCanBeRetried(function() {
        coll.insertMany([{_id: 4}, {_id: 5}], {ordered: false});
    });

    testCommandCanBeRetried(function() {
        coll.updateMany({a: {$gt: 0}}, {$set: {c: 7}});
    }, false);

    testCommandCanBeRetried(function() {
        coll.deleteMany({b: {$lt: 5}});
    }, false);

    //
    // Tests for writeConcern.
    //

    testCommandCanBeRetried(function() {
        coll.insertOne({_id: 1}, {w: 1});
    });

    testCommandCanBeRetried(function() {
        coll.insertOne({_id: "majority"}, {w: "majority"});
    });

    testCommandCanBeRetried(function() {
        coll.insertOne({_id: 0}, {w: 0});
    }, false);

    //
    // Tests for bulkWrite().
    //

    testCommandCanBeRetried(function() {
        coll.bulkWrite([{insertOne: {document: {_id: 10}}}]);
    });

    testCommandCanBeRetried(function() {
        coll.bulkWrite([{updateOne: {filter: {_id: 10}, update: {$set: {a: 1}}}}]);
    });

    testCommandCanBeRetried(function() {
        coll.bulkWrite([{updateOne: {filter: {_id: 10}, update: {$set: {a: 2}}, upsert: true}}]);
    });

    testCommandCanBeRetried(function() {
        coll.bulkWrite([{deleteOne: {filter: {_id: 10}}}]);
    });

    testCommandCanBeRetried(function() {
        coll.bulkWrite(
            [{insertOne: {document: {_id: 20, b: 3}}}, {insertOne: {document: {_id: 30, b: 4}}}],
            {ordered: true});
    });

    testCommandCanBeRetried(function() {
        coll.bulkWrite([{insertOne: {document: {_id: 40}}}, {insertOne: {document: {_id: 50}}}],
                       {ordered: false});
    });

    testCommandCanBeRetried(function() {
        coll.bulkWrite([{updateMany: {filter: {a: {$gt: 0}}, update: {$set: {c: 7}}}}]);
    }, false);

    testCommandCanBeRetried(function() {
        coll.bulkWrite([{deleteMany: {filter: {b: {$lt: 5}}}}]);
    }, false);

    //
    // Tests for wrappers around "findAndModify" command.
    //

    testCommandCanBeRetried(function() {
        coll.findOneAndUpdate({_id: 100}, {$set: {d: 9}}, {upsert: true});
    });

    testCommandCanBeRetried(function() {
        coll.findOneAndReplace({_id: 100}, {e: 11});
    });

    testCommandCanBeRetried(function() {
        coll.findOneAndDelete({e: {$exists: true}});
    });

    db.getSession().endSession();
    rst.stopSet();
})();
