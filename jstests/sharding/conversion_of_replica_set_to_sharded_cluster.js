/*
 * Tests that during an upgrade from a replica set to a sharded cluster the CRUD and DDL command
 * works. This implies testing those commands on a replica set directly when it is in a sharded
 * cluster.
 * @tags: [
 *   requires_persistence,
 *   multiversion_incompatible
 * ]
 */

(function() {
'use strict';

load('jstests/replsets/rslib.js');
load('jstests/sharding/libs/remove_shard_util.js');

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

const expectedDocs = 1000;
const dbName = 'test';
const collName = 'foo';
const otherCollName = "bar";
const DDLDbName = 'DDLTest';
const DDLCollName = 'DDLFoo';
let str = 'a';
while (str.length < 8000) {
    str += str;
}

let map = function() {
    emit(this.i, this.j);
};

let reduce = function(key, values) {
    let jCount = 0;
    values.forEach(function(j) {
        jCount += j;
    });
    return jCount;
};

const CRUDCommands = {
    find: {
        command: {find: collName},
        assertFunc: (res, testDB) => {
            assert.commandWorked(res);
            assert.eq(expectedDocs, new DBCommandCursor(testDB, res).itcount());
        }
    },
    count: {
        command: {count: collName},
        assertFunc: (res) => {
            assert.commandWorked(res);
            assert.eq(expectedDocs, res.n);
        }
    },
    dbstats: {
        command: {dbstats: 1},
        assertFunc: (res) => {
            assert.commandWorked(res);
        }
    },
    collstats: {
        command: {collstats: collName},
        assertFunc: (res) => {
            assert.commandWorked(res);
        }
    },
    mapreduce: {
        command: {mapreduce: collName, map: map, reduce: reduce, out: {inline: 1}},
        assertFunc: (res) => {
            assert.commandWorked(res);
            assert.eq(100, res.results.length);
            assert.eq(45, res.results[0].value);
        }
    },
    aggregate: {
        command: {
            aggregate: collName,
            pipeline: [{$project: {j: 1}}, {$group: {_id: 'j', sum: {$sum: '$j'}}}],
            cursor: {}
        },
        assertFunc: (res) => {
            assert.commandWorked(res);
            assert.eq(4500, res.cursor.firstBatch[0].sum);
        }
    },
    aggregateWithLookup: {
        command: {
            aggregate: collName,
            pipeline: [
                {$sort: {j: 1}},
                {$lookup: {from: otherCollName, localField: "j", foreignField: "i", as: "lookedUp"}}
            ],
            cursor: {}
        },
        assertFunc: (res) => {
            assert.commandWorked(res);
            assert.eq(res.cursor.firstBatch[0].lookedUp.length, 10);
        }
    },
    aggregateWithGraphLookup: {
        command: {
            aggregate: collName,
            pipeline: [
                {$sort: {j: 1}},
                {
                    $graphLookup: {
                        from: otherCollName,
                        startWith: "$j",
                        connectFromField: "j",
                        connectToField: "i",
                        as: "graphLookedUp",
                        maxDepth: 10
                    }
                }
            ],
            cursor: {}
        },
        assertFunc: (res) => {
            assert.commandWorked(res);
            assert.eq(res.cursor.firstBatch[0].graphLookedUp.length, 100);
        }
    },
    insert: {
        command: {insert: collName, documents: [{a: 1, i: 1, j: 1}]},
        assertFunc: (res) => {
            assert.commandWorked(res);
            assert.eq(1, res.n);
        }
    },
    update: {
        command: {update: collName, updates: [{q: {a: 1, i: 1, j: 1}, u: {$set: {u: 1}}}]},
        assertFunc: (res, testDB) => {
            assert.commandWorked(res);
            assert.eq(1, res.n);
            assert.eq(1, testDB.foo.findOne({a: 1}).u);
        }
    },
    findAndModify: {
        command: {findAndModify: collName, query: {a: 1, i: 1, j: 1}, update: {$set: {b: 1}}},
        assertFunc: (res, testDB) => {
            assert.commandWorked(res);
            assert.eq(1, res.value.a);
            assert.eq(null, res.value.b);
            assert.eq(1, testDB.foo.findOne({a: 1}).b);
        }
    },
    remove: {
        command: {delete: collName, deletes: [{q: {a: 1}, limit: 1}]},
        assertFunc: (res) => {
            assert.commandWorked(res);
            assert.eq(1, res.n);
        }
    },
    mapreduceWithWrite: {
        command: {mapreduce: collName, map: map, reduce: reduce, out: 'mrOutput'},
        assertFunc: (res, testDB) => {
            assert.commandWorked(res);
            assert.eq(100, testDB.mrOutput.count());
            assert.eq(45, testDB.mrOutput.findOne().value);
        },
        post: (testDB) => {
            testDB.mrOutput.remove({});
        }
    },
    aggregateWithOut: {
        command: {
            aggregate: collName,
            pipeline:
                [{$project: {j: 1}}, {$group: {_id: 'j', sum: {$sum: '$j'}}}, {$out: 'aggOutput'}],
            cursor: {}
        },
        assertFunc: (res, testDB) => {
            assert.commandWorked(res);
            assert.eq(4500, testDB.aggOutput.findOne().sum);
        },
        post: (testDB) => {
            testDB.aggOutput.remove({});
        }
    },
    aggregateWithMerge: {
        command: {
            aggregate: collName,
            pipeline: [{
                $merge: {
                    into: otherCollName,
                    whenMatched: [{$set: {merged: true}}],
                    whenNotMatched: "fail"
                }
            }],
            cursor: {}
        },
        assertFunc: (res, testDB) => {
            assert.commandWorked(res);
            assert.eq(testDB[otherCollName].findOne().merged, true);
        }
    }
};

const DDLCommands = {
    createCollection: {
        command: {create: DDLCollName},
        assertFunc: (res, testDB) => {
            assert.commandWorked(res);
            assert.eq(0, testDB.runCommand({count: DDLCollName}).n);
        }
    },
    createIndex: {
        pre: (testDB) => {
            let res = testDB.runCommand({insert: DDLCollName, documents: [{a: 1}]});
            assert.commandWorked(res);
            assert.eq(1, res.n);
        },
        command: {createIndexes: DDLCollName, indexes: [{key: {a: 1}, name: 'a_1'}]},
        assertFunc: (res) => {
            assert.commandWorked(res);
        }
    },
    dropIndex: {
        command: {dropIndexes: DDLCollName, index: ['a_1']},
        assertFunc: (res) => {
            assert.commandWorked(res);
        }
    },
    dropCollection: {
        command: {drop: DDLCollName},
        assertFunc: (res, testDB) => {
            assert.commandWorked(res);
            assert.eq(0, testDB.runCommand({count: DDLCollName}).n);
        }
    },
    dropDatabase: {
        command: {dropDatabase: 1},
        assertFunc: (res, testDB) => {
            assert.commandWorked(res);
            assert.eq(0, testDB.runCommand({count: DDLCollName}).n);
        }
    }
};

let assertAddShardSucceeded = function(res, shardName) {
    assert.commandWorked(res);

    // If a shard name was specified, make sure that the name the addShard command reports the
    // shard was added with matches the specified name.
    if (shardName) {
        assert.eq(shardName,
                  res.shardAdded,
                  "name returned by addShard does not match name specified in addShard");
    }

    // Make sure the shard shows up in config.shards with the shardName reported by the
    // addShard command.
    assert.neq(null,
               st.s.getDB('config').shards.findOne({_id: res.shardAdded}),
               "newly added shard " + res.shardAdded + " not found in config.shards");
};

let removeShardWithName = removeShard;

let checkCRUDCommands = function(testDB) {
    for (let command in CRUDCommands) {
        jsTestLog('Testing CRUD command: ' + command);
        assert.soonNoExcept(() => {
            CRUDCommands[command].assertFunc(testDB.runCommand(CRUDCommands[command].command),
                                             testDB);
            return true;
        });
    }
};

let checkDDLCommands = function(testDB) {
    for (let command in DDLCommands) {
        jsTestLog('Testing DDL command: ' + command);
        if (DDLCommands[command].pre) {
            DDLCommands[command].pre(testDB);
        }
        DDLCommands[command].assertFunc(testDB.runCommand(DDLCommands[command].command), testDB);
    }
};

jsTest.log("Creating replica set.");
let rst0 = new ReplSetTest({name: 'rs0', nodes: 2});
rst0.startSet();
rst0.initiate();
waitForAllMembers(rst0.getPrimary().getDB(dbName));

let coll = rst0.getPrimary().getDB(dbName).getCollection(collName);
// Initial set up.
for (var i = 0; i < 100; i++) {
    var bulk = coll.initializeUnorderedBulkOp();
    for (var j = 0; j < 10; j++) {
        bulk.insert({i: i, j: j, str: str});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));
}
// Create collection 'otherCollName' as a duplicate of the original collection. This is just an easy
// way of providing a second collection for $lookup, $graphLookup and $merge aggregations.
assert.commandWorked(coll.runCommand("aggregate", {pipeline: [{$out: otherCollName}], cursor: {}}));

jsTest.log("First test: run all test-cases on the replica set as a non-shard server.");
checkCRUDCommands(rst0.getPrimary().getDB(dbName));
checkDDLCommands(rst0.getPrimary().getDB(DDLDbName));

let st = new ShardingTest({
    shards: TestData.configShard ? 1 : 0,
    mongos: 1,
});

jsTest.log("Second test: restart the replica set as a shardsvr but don't add it to a cluster.");
rst0.restart(0, {shardsvr: ''});
rst0.restart(1, {shardsvr: ''});
rst0.awaitReplication();

checkCRUDCommands(rst0.getPrimary().getDB(dbName));
checkDDLCommands(rst0.getPrimary().getDB(DDLDbName));

jsTest.log("Third test, using the rs connection directly.");
let addShardRes = st.s.adminCommand({addShard: rst0.getURL(), name: rst0.name});
assertAddShardSucceeded(addShardRes, rst0.name);

checkCRUDCommands(rst0.getPrimary().getDB(dbName));
checkDDLCommands(rst0.getPrimary().getDB(DDLDbName));

jsTest.log("Fourth test, using the router.");
checkCRUDCommands(st.s0.getDB(dbName));
checkDDLCommands(st.s0.getDB(DDLDbName));

// Cleaning up.
rst0.stopSet();

st.stop();
})();
