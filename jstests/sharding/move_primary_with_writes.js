/**
 * @tags: [does_not_support_stepdowns, requires_scripting]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// TODO SERVER-87501 Review all usages of ErrorCodes.MovePrimaryInProgress in this file once v8.0
// branches out

let st = new ShardingTest({
    mongos: 2,
    shards: 2,
});

const dbName = "testdb";
const otherDbName = "otherdb";

const numDocuments = 3;
let collections = [];

function verifyDocuments(mongos, dbName, fromShard, toShard, numDocsOnDataShard) {
    assert.eq(3, collections.length);

    function assertNumDocsOnShard(collName, numDocs, shard) {
        assert.eq(numDocs,
                  shard.getDB(dbName).getCollection(collName).count(),
                  "Collection " + collName + " is expected to have " + numDocs + " docs on shard " +
                      shard.shardName);
    }

    collections.forEach(coll => {
        const collName = coll.collName;
        const originalShard = coll.originalShard;
        if (FixtureHelpers.isTracked(mongos.getDB(dbName).getCollection(collName))) {
            // If the collection is tracked it shouldn't be moved by the movePrimary operation.
            assertNumDocsOnShard(collName, numDocsOnDataShard, originalShard);

            if (fromShard.shardName != originalShard.shardName) {
                assertNumDocsOnShard(collName, 0, fromShard);
            }
            if (toShard.shardName != originalShard.shardName) {
                assertNumDocsOnShard(collName, 0, toShard);
            }

        } else {
            // If the collection is untracked, it has to live on the primary shard.
            assertNumDocsOnShard(collName, 0, fromShard);
            assertNumDocsOnShard(collName, numDocsOnDataShard, toShard);
        }
    });
}

/**
 * Creates the following collections:
 *     - testdb.unshardedColl
 *          collection type:    unsharded
 *          shard:              shard0
 *          count:              3
 *          indexes:            {a: 1}, {c: 1}
 *     - testdb.unshardedTrackedColl
 *          collection type:    unsharded
 *          shard:              shard1
 *          count:              3
 *          indexes:            {a: 1}, {c: 1}
 *     - testdb.shardedColl
 *          collection type:    sharded
 *          shardKey:           {_id: 1}
 *          count:              3
 *          indexes:            {a: 1}, {c: 1}
 *     - otherdb.otherUnshardedColl
 *          collection type:    unsharded
 *          shard:              shard0
 *          count:              0
 *          indexes:            none
 */
function createCollections() {
    collections = [];
    collections.push({collName: 'unshardedColl', originalShard: st.shard0});
    collections.push({collName: 'unshardedTrackedColl', originalShard: st.shard1});
    collections.push({collName: 'shardedColl', originalShard: st.shard0});

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

    assert.commandWorked(db.createCollection('unshardedColl'));
    assert.commandWorked(db.createCollection('unshardedTrackedColl'));
    assert.commandWorked(db.createCollection('shardedColl'));
    assert.commandWorked(otherDb.createCollection('otherUnshardedColl'));

    for (let i = 0; i < numDocuments; i++) {
        assert.commandWorked(db.unshardedColl.insert({_id: i, a: i, b: i, c: i}));
        assert.commandWorked(db.unshardedTrackedColl.insert({_id: i, a: i, b: i, c: i}));
        assert.commandWorked(db.shardedColl.insert({_id: i, a: i, b: i, c: i}));
    }

    assert.commandWorked(
        db.runCommand({createIndexes: 'unshardedColl', indexes: unshardedFooIndexes}));
    assert.commandWorked(
        db.runCommand({createIndexes: 'unshardedTrackedColl', indexes: unshardedFooIndexes}));
    assert.commandWorked(db.runCommand({createIndexes: 'shardedColl', indexes: shardedBarIndexes}));

    assert.commandWorked(db.adminCommand(
        {moveCollection: dbName + '.unshardedTrackedColl', toShard: st.shard1.shardName}));

    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    assert.commandWorked(
        db.adminCommand({shardCollection: dbName + '.shardedColl', key: {_id: 1}}));

    assert.commandWorked(db.createView('unshardedFooView', 'unshardedColl', [{$match: {}}]));
    assert.commandWorked(db.createView('shardedBarView', 'shardedColl', [{$match: {}}]));
}

function mapFunc() {
    emit(this.a, 1);
}
function reduceFunc(key, values) {
    return Array.sum(values);
}

function buildCommands(collName, isCollTracked) {
    const commands = [
        {
            command: {insert: collName, documents: [{a: 10}]},
            shouldFail: !isCollTracked,
            isAdminCommand: false
        },
        {
            command: {update: collName, updates: [{q: {a: 1}, u: {$set: {a: 11}}}]},
            shouldFail: !isCollTracked,
            isAdminCommand: false
        },
        {
            command: {findAndModify: collName, query: {_id: 2}, update: {$set: {a: 11}}},
            shouldFail: !isCollTracked,
            isAdminCommand: false
        },
        {
            command: {delete: collName, deletes: [{q: {_id: 0}, limit: 1}]},
            shouldFail: !isCollTracked,
            isAdminCommand: false
        },
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
            shouldFail: !isCollTracked,
            isAdminCommand: false
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
            shouldFail: false,
            isAdminCommand: false
        },
        {
            command:
                {aggregate: collName, cursor: {}, pipeline: [{$match: {}}, {$out: "testOutColl"}]},
            shouldFail: true,
            isAdminCommand: false,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress]
        },
        {
            command: {
                mapReduce: collName,
                map: mapFunc,
                reduce: reduceFunc,
                out: {merge: "testOutMR", db: dbName}
            },
            shouldFail: true,
            isAdminCommand: false
        },
        {
            command: {
                mapReduce: collName,
                map: mapFunc,
                reduce: reduceFunc,
                out: {merge: "testOutMR", db: otherDbName}
            },
            shouldFail: false,
            isAdminCommand: false
        },
        {
            command: {create: "testCollection"},
            shouldFail: true,
            isAdminCommand: false,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress]
        },
        {
            command: {create: "testView", viewOn: collName, pipeline: [{$match: {}}]},
            shouldFail: true,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress],
            isAdminCommand: false
        },
        {
            command: {createIndexes: collName, indexes: [{key: {b: 1}, name: collName + "Idx_b"}]},
            shouldFail: !isCollTracked,
            isAdminCommand: false
        },
        {
            command: {collMod: collName, index: {keyPattern: {c: 1}, expireAfterSeconds: 3600}},
            shouldFail: true,
            isAdminCommand: false,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress]
        },
        {
            command: {collMod: collName + "View", viewOn: collName, pipeline: [{$match: {_id: 1}}]},
            shouldFail: true,
            isAdminCommand: false,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress]
        },
        {
            command: {convertToCapped: "unshardedFoo", size: 1000000},
            shouldFail: true,
            isAdminCommand: false,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.MovePrimaryInProgress]
        },
        {
            command: {dropIndexes: collName, index: collName + "Index"},
            shouldFail: true,
            isAdminCommand: false,
            errorCodes: [
                ErrorCodes.LockBusy,
                ErrorCodes.MovePrimaryInProgress,
                ErrorCodes.InterruptedDueToReplStateChange
            ]
        },
        {
            command: {drop: collName},
            shouldFail: true,
            isAdminCommand: false,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.InterruptedDueToReplStateChange]
        },
        {
            command: {dropDatabase: 1},
            shouldFail: true,
            isAdminCommand: false,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.InterruptedDueToReplStateChange]
        },
        {
            command: {renameCollection: dbName + "." + collName, to: dbName + ".testCollection"},
            shouldFail: true,
            isAdminCommand: true,
            errorCodes: [ErrorCodes.LockBusy, ErrorCodes.InterruptedDueToReplStateChange]
        }
    ];
    return commands;
}

function testMovePrimary(failpoint, fromShard, toShard, mongoS, dbName) {
    let db = mongoS.getDB(dbName);

    let codeToRunInParallelShell = '{ db.getSiblingDB("admin").runCommand({movePrimary: "' +
        dbName + '", to: "' + toShard.name + '"}); }';

    let fp = configureFailPoint(fromShard, failpoint);

    let awaitShell = startParallelShell(codeToRunInParallelShell, st.s.port);

    jsTestLog("Waiting for failpoint " + failpoint);
    fp.wait();
    clearRawMongoProgramOutput();

    assert.eq(3, collections.length);
    collections.forEach(coll => {
        const collName = coll.collName;
        const isCollTracked =
            FixtureHelpers.isTracked(mongoS.getDB(dbName).getCollection(collName));

        jsTestLog("Testing move primary with FP: " + failpoint + ", collection: " + collName +
                  ", isTracked: " + isCollTracked);

        buildCommands(collName, isCollTracked).forEach(commandObj => {
            jsTestLog("Running command: " + tojson(commandObj.command) +
                      ", shoudFail: " + commandObj.shouldFail);

            let dbTarget = db;
            if (commandObj.isAdminCommand) {
                dbTarget = db.getSiblingDB('admin');
            }

            if (commandObj.shouldFail) {
                if (commandObj.hasOwnProperty("errorCodes")) {
                    assert.commandFailedWithCode(dbTarget.runCommand(commandObj.command),
                                                 commandObj.errorCodes);
                } else {
                    assert.commandFailedWithCode(dbTarget.runCommand(commandObj.command),
                                                 ErrorCodes.MovePrimaryInProgress);
                }
            } else if (!commandObj.shouldFail) {
                assert.commandWorked(dbTarget.runCommand(commandObj.command));
            }
        });
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

let hangBeforeCloningDataFPName = "hangBeforeCloningData";

createCollections();
let fromShard = st.getPrimaryShard(dbName);
let toShard = st.getOther(fromShard);

testMovePrimary(hangBeforeCloningDataFPName, fromShard, toShard, st.s, dbName);
verifyDocuments(st.s, dbName, fromShard, toShard, numDocuments);

overrideDDLLockTimeoutFPs.forEach(fp => fp.off());

st.stop();
