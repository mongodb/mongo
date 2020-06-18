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

const expectedDocs = 1000;
const dbName = 'test';
const collName = 'foo';
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
    // TODO SERVER-48128: once the mapreduce with output is fixed, uncomment this command
    /*
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
    aggregateWithWrite: {
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
    }
    */
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

var removeShardWithName = function(st, shardName) {
    var res = st.s.adminCommand({removeShard: shardName});
    assert.commandWorked(res);
    assert.eq('started', res.state);
    assert.soon(function() {
        res = st.s.adminCommand({removeShard: shardName});
        assert.commandWorked(res);
        return ('completed' === res.state);
    }, "removeShard never completed for shard " + shardName);
};

let checkCRUDCommands = function(testDB) {
    for (let command in CRUDCommands) {
        jsTestLog('Testing CRUD command: ' + command);
        CRUDCommands[command].assertFunc(testDB.runCommand(CRUDCommands[command].command), testDB);
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

jsTest.log("Runing control tests.");
checkCRUDCommands(rst0.getPrimary().getDB(dbName));
checkDDLCommands(rst0.getPrimary().getDB(DDLDbName));

let st = new ShardingTest({
    shards: 0,
    mongos: 1,
});

rst0.restart(0, {shardsvr: ''});
rst0.restart(1, {shardsvr: ''});
rst0.awaitReplication();

let addShardRes = st.s.adminCommand({addShard: rst0.getURL(), name: rst0.name});
assertAddShardSucceeded(addShardRes, rst0.name);

jsTest.log("First test, using the rs connection directly.");
checkCRUDCommands(rst0.getPrimary().getDB(dbName));
checkDDLCommands(rst0.getPrimary().getDB(DDLDbName));

jsTest.log("Second test, using the router.");
checkCRUDCommands(st.s0.getDB(dbName));
checkDDLCommands(st.s0.getDB(DDLDbName));

// Cleaning up.
rst0.stopSet();

st.stop();
})();