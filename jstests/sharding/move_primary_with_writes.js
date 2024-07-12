/**
 * @tags: [does_not_support_stepdowns]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// TODO SERVER-87501 Review all usages of ErrorCodes.MovePrimaryInProgress in this file once v8.0
// branches out

let st = new ShardingTest({
    mongos: 2,
    shards: 2,
});

const dbName = "testdb";
const otherDbName = "otherdb";

function verifyDocuments(mongos, dbName, fromShard, toShard, count) {
    if (FixtureHelpers.isTracked(mongos.getDB(dbName).getCollection('unshardedFoo'))) {
        assert.eq(count, fromShard.unshardedFoo.count());
        assert.eq(0, toShard.unshardedFoo.count());
    } else {
        assert.eq(0, fromShard.unshardedFoo.count());
        assert.eq(count, toShard.unshardedFoo.count());
    }
}

function createCollections() {
    assert.commandWorked(st.getDB(dbName).runCommand({dropDatabase: 1}));
    let db = st.getDB(dbName);
    let otherDb = st.getDB(otherDbName);

    const unshardedFooIndexes = [
        {key: {a: 1}, name: 'unshardedFooIndex'},
        {key: {c: 1}, name: 'fooTTL_c', expireAfterSeconds: 1800}
    ];
    const shardedBarIndexes = [
        {key: {a: 1}, name: 'shardedBarIndex'},
        {key: {c: 1}, name: 'barTTL_c', expireAfterSeconds: 1800}
    ];

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

    assert.commandWorked(db.createCollection('unshardedFoo'));
    assert.commandWorked(db.createCollection('shardedBar'));
    assert.commandWorked(otherDb.createCollection('unshardedOtherFoo'));

    for (let i = 0; i < 3; i++) {
        assert.commandWorked(db.unshardedFoo.insert({_id: i, a: i, b: i, c: i}));
        assert.commandWorked(db.shardedBar.insert({_id: i, a: i, b: i, c: i}));
    }

    assert.commandWorked(
        db.runCommand({createIndexes: 'unshardedFoo', indexes: unshardedFooIndexes}));
    assert.commandWorked(db.runCommand({createIndexes: 'shardedBar', indexes: shardedBarIndexes}));

    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    assert.commandWorked(db.adminCommand({shardCollection: dbName + '.shardedBar', key: {_id: 1}}));

    assert.commandWorked(db.createView('unshardedFooView', 'unshardedFoo', [{$match: {}}]));
    assert.commandWorked(db.createView('shardedBarView', 'shardedBar', [{$match: {}}]));
}

function mapFunc() {
    emit(this.a, 1);
}
function reduceFunc(key, values) {
    return Array.sum(values);
}

function buildCommands(collName, shouldFail) {
    const commands = [
        {command: {insert: collName, documents: [{a: 10}]}, shouldFail: shouldFail},
        {
            command: {update: collName, updates: [{q: {a: 1}, u: {$set: {a: 11}}}]},
            shouldFail: shouldFail
        },
        {
            command: {findAndModify: collName, query: {_id: 2}, update: {$set: {a: 11}}},
            shouldFail: shouldFail
        },
        {command: {delete: collName, deletes: [{q: {_id: 0}, limit: 1}]}, shouldFail: shouldFail},
        {
            command: {
                aggregate: collName,
                cursor: {},
                pipeline: [
                    {$match: {_id: 0}},
                    {
                        $merge: {
                            into: {db: dbName, coll: "testMergeColl"},
                            on: "_id",
                            whenMatched: "replace",
                            whenNotMatched: "insert"
                        }
                    }
                ]
            },
            shouldFail: true
        },
        {
            command: {
                aggregate: collName,
                cursor: {},
                pipeline: [
                    {$match: {_id: 0}},
                    {
                        $merge: {
                            into: {db: otherDbName, coll: "testMergeColl"},
                            on: "_id",
                            whenMatched: "replace",
                            whenNotMatched: "insert"
                        }
                    }
                ]
            },
            shouldFail: false
        },
        {
            command:
                {aggregate: collName, cursor: {}, pipeline: [{$match: {}}, {$out: "testOutColl"}]},
            shouldFail: true,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress]
        },
        {
            command: {
                mapReduce: collName,
                map: mapFunc,
                reduce: reduceFunc,
                out: {merge: "testOutMR", db: dbName}
            },
            shouldFail: true
        },
        {
            command: {
                mapReduce: collName,
                map: mapFunc,
                reduce: reduceFunc,
                out: {merge: "testOutMR", db: otherDbName}
            },
            shouldFail: false
        },
        {
            command: {create: "testCollection"},
            shouldFail: true,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress]
        },
        {
            command: {create: "testView", viewOn: collName, pipeline: [{$match: {}}]},
            shouldFail: true,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress]
        },
        {
            command: {createIndexes: collName, indexes: [{key: {b: 1}, name: collName + "Idx_b"}]},
            shouldFail: shouldFail
        },
        {
            command: {collMod: collName, index: {keyPattern: {c: 1}, expireAfterSeconds: 3600}},
            shouldFail: true,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress]
        },
        {
            command: {collMod: collName + "View", viewOn: collName, pipeline: [{$match: {_id: 1}}]},
            shouldFail: true,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress]
        },
        {
            command: {convertToCapped: "unshardedFoo", size: 1000000},
            shouldFail: true,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress]
        },
        {
            command: {dropIndexes: collName, index: collName + "Index"},
            shouldFail: true,
            errorCodes: [
                ErrorCodes.LockBusy,
                ErrorCodes.MovePrimaryInProgress,
                ErrorCodes.InterruptedDueToReplStateChange
            ]
        },
        {
            command: {drop: collName},
            shouldFail: true,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.InterruptedDueToReplStateChange]
        },
        {
            command: {dropDatabase: 1},
            shouldFail: true,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.InterruptedDueToReplStateChange]
        },
    ];
    return commands;
}

function buildDDLCommands(collName) {
    const commands = [{
        command: {renameCollection: dbName + "." + collName, to: dbName + ".testCollection"},
        alwaysFail: true
    }];
    return commands;
}

function testMovePrimary(failpoint, fromShard, toShard, mongoS, dbName, shouldFail, sharded) {
    jsTestLog("Testing move primary with FP: " + failpoint + " shouldFail: " + shouldFail +
              " sharded: " + sharded);
    let db = mongoS.getDB(dbName);

    let codeToRunInParallelShell = '{ db.getSiblingDB("admin").runCommand({movePrimary: "' +
        dbName + '", to: "' + toShard.name + '"}); }';

    let fp = configureFailPoint(fromShard, failpoint);

    let awaitShell = startParallelShell(codeToRunInParallelShell, st.s.port);

    jsTestLog("Waiting for failpoint " + failpoint);
    fp.wait();
    clearRawMongoProgramOutput();

    // Test DML

    let collName;
    if (sharded) {
        collName = "shardedBar";
    } else {
        collName = "unshardedFoo";
    }
    let cmdShouldFail = !FixtureHelpers.isTracked(mongoS.getDB(dbName).getCollection(collName));

    buildCommands(collName, cmdShouldFail).forEach(commandObj => {
        if (shouldFail && commandObj.shouldFail) {
            jsTestLog("running command: " + tojson(commandObj.command) +
                      ",\nshoudFail: " + shouldFail);
            if (commandObj.hasOwnProperty("errorCodes")) {
                assert.commandFailedWithCode(db.runCommand(commandObj.command),
                                             commandObj.errorCodes);
            } else {
                assert.commandFailedWithCode(db.runCommand(commandObj.command),
                                             ErrorCodes.MovePrimaryInProgress);
            }
        } else if (!shouldFail && !commandObj.shouldFail) {
            jsTestLog("running command: " + tojson(commandObj.command) +
                      ",\nshoudFail: " + shouldFail);
            assert.commandWorked(db.runCommand(commandObj.command));
        }
    });

    fp.off();

    awaitShell();
}

function testMovePrimaryDDL(failpoint, fromShard, toShard, db, shouldFail, sharded) {
    jsTest.log("Testing move primary DDL with FP: " + failpoint + " shouldFail: " + shouldFail +
               " sharded: " + sharded);

    let codeToRunInParallelShell = '{ db.getSiblingDB("admin").runCommand({movePrimary: "' +
        dbName + '", to: "' + toShard.name + '"}); }';

    let fp = configureFailPoint(fromShard, failpoint);

    let awaitShell = startParallelShell(codeToRunInParallelShell, st.s.port);

    jsTestLog("Waiting for failpoint " + failpoint);
    fp.wait();
    clearRawMongoProgramOutput();

    let collName;
    if (sharded) {
        collName = "shardedBar";
    } else {
        collName = "unshardedFoo";
    }

    buildDDLCommands(collName).forEach(commandObj => {
        if (shouldFail) {
            jsTestLog("running command: " + tojson(commandObj.command) +
                      ",\nshoudFail: " + shouldFail);
            assert.commandFailedWithCode(db.runCommand(commandObj.command),
                                         ErrorCodes.MovePrimaryInProgress);
        } else if (!commandObj.alwaysFail) {
            jsTestLog("running command: " + tojson(commandObj.command) +
                      ",\nshoudFail: " + shouldFail);
            assert.commandWorked(db.runCommand(commandObj.command));
        }
    });

    fp.off();

    awaitShell();
}

// Reduce DDL lock timeout to half a second to speedup testing command that are expected to fail
// with lockbusy error
let overrideDDLLockTimeoutFPs = [];
st.forEachConnection(shard => {
    try {
        overrideDDLLockTimeoutFPs.push(
            configureFailPoint(shard, "overrideDDLLockTimeout", {'timeoutMillisecs': 500}));
    } catch (e) {
        // The failpoint has been added in 5.3 so multiversion suite will fail to set this failpoint
        jsTestLog("Failed to override DDL lock timeout: " + e);
    }
});

let cloningDataFPName = "hangBeforeCloningData";
let unshardedNss = dbName + '.unshardedFoo';

createCollections();
let fromShard = st.getPrimaryShard(dbName);
let toShard = st.getOther(fromShard);

testMovePrimary(cloningDataFPName, fromShard, toShard, st.s, dbName, true, false);
verifyDocuments(st.s, dbName, fromShard.getDB(dbName), toShard.getDB(dbName), 3);

createCollections();
fromShard = st.getPrimaryShard(dbName);
toShard = st.getOther(fromShard);

testMovePrimary(cloningDataFPName, fromShard, toShard, st.s, dbName, false, true);
verifyDocuments(st.s, dbName, fromShard.getDB(dbName), toShard.getDB(dbName), 3);

createCollections();
fromShard = st.getPrimaryShard(dbName);
toShard = st.getOther(fromShard);
testMovePrimaryDDL(cloningDataFPName, fromShard, toShard, st.s.getDB("admin"), false, true);

overrideDDLLockTimeoutFPs.forEach(fp => fp.off());

st.stop();
