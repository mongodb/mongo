/*
Test that authorization works properly for all
database commands.

Tests are defined declaratively in the "tests" array.

Example:

This tests that when run on the "roles_commands_1" database,
the given command will return an authorization error unless
the user has the role with key "readWrite" or the role with
key "readWriteAnyDatabase". The command will be run as a
user with each of the roles in the "roles" array below.

Similarly, this tests that when run on the "roles_commands_2"
database, only a user with role "readWriteAnyDatabase" should
be authorized.

    {
        testname: "aggregate_write",
        command: {aggregate: "foo", pipeline: [ {$out: "foo_out"} ] },
        testcases: [
            { runOnDb: "roles_commands_1", rolesAllowed: {readWrite: 1, readWriteAnyDatabase: 1} },
            { runOnDb: "roles_commands_2", rolesAllowed: {readWriteAnyDatabase: 1} }
        ]
    },

Additional options:

1) onSuccess

A test can provide an onSuccess callback, which is called when the command
is authorized and succeeds. The callback is passed a single parameter, which
is the document returned by the command.

2) expectFail

You can add "expectFail: true" to an individual element of the testcases
array. This means that if the command is authorized, then you still expect
it to fail with a non-auth related error. As always, for roles other than
those in rolesAllowed, an auth error is expected.

3) skipSharded

Add "skipSharded: true" if you want to run the test only on a standalone.

4) skipStandalone

Add "skipStandalone: true" if you want to run the test only in sharded
configuration.

5) setup

The setup function, if present, is called before testing whether a
particular role authorizes a command for a particular database.

6) teardown

The teardown function, if present, is called immediately after
testint whether a particular role authorizes a command for a
particular database.

*/


// constants
var firstDbName = "roles_commands_1";
var secondDbName = "roles_commands_2";
var adminDbName = "admin";
var authErrCode = 13;
var shard0name = "shard0000";

var roles = [
    {key: "read", role: "read", dbname: firstDbName},
    {key: "readAnyDatabase", role: "readAnyDatabase", dbname: adminDbName},
    {key: "readWrite", role: "readWrite", dbname: firstDbName},
    {key: "readWriteAnyDatabase", role: "readWriteAnyDatabase", dbname: adminDbName},
    {key: "userAdmin", role: "userAdmin", dbname: firstDbName},
    {key: "userAdminAnyDatabase", role: "userAdminAnyDatabase", dbname: adminDbName},
    {key: "dbAdmin", role: "dbAdmin", dbname: firstDbName},
    {key: "dbAdminAnyDatabase", role: "dbAdminAnyDatabase", dbname: adminDbName},
    {key: "clusterAdmin", role: "clusterAdmin", dbname: adminDbName},
    {key: "__system", role: "__system", dbname: adminDbName}
];

// useful shorthand when defining the tests below
var roles_write = {
    readWrite: 1,
    readWriteAnyDatabase: 1,
    dbOwner: 1,
    root: 1,
    __system: 1
};
var roles_readWrite = {
    read: 1,
    readAnyDatabase: 1,
    readWrite: 1,
    readWriteAnyDatabase: 1,
    dbOwner: 1,
    root: 1,
    __system: 1
};
var roles_readWriteAny = {
    readAnyDatabase: 1,
    readWriteAnyDatabase: 1,
    root: 1,
    __system: 1
};
var roles_writeDbAdmin = {
    readWrite: 1,
    readWriteAnyDatabase: 1,
    dbAdmin: 1,
    dbAdminAnyDatabase: 1,
    dbOwner: 1,
    root: 1,
    __system: 1 
};
var roles_writeDbAdminAny = {
    readWriteAnyDatabase: 1,
    dbAdminAnyDatabase: 1,
    root: 1,
    __system: 1
};
var roles_readWriteDbAdmin = {
    read: 1,
    readAnyDatabase: 1,
    readWrite: 1,
    readWriteAnyDatabase: 1,
    dbAdmin: 1,
    dbAdminAnyDatabase: 1,
    dbOwner: 1,
    root: 1,
    __system: 1
};
var roles_readWriteDbAdminAny = {
    readAnyDatabase: 1,
    readWriteAnyDatabase: 1,
    dbAdminAnyDatabase: 1,
    root: 1,
    __system: 1
};
var roles_monitoring = {
    clusterMonitor: 1,
    clusterAdmin: 1,
    root: 1,
    __system: 1
};
var roles_hostManager = {
    hostManager: 1,
    clusterAdmin: 1,
    root: 1,
    __system: 1
};
var roles_clusterManager = {
    clusterManager: 1,
    clusterAdmin: 1,
    root: 1,
    __system: 1
};
var roles_all = {
    read: 1,
    readAnyDatabase: 1,
    readWrite: 1,
    readWriteAnyDatabase: 1,
    userAdmin: 1,
    userAdminAnyDatabase: 1,
    dbAdmin: 1,
    dbAdminAnyDatabase: 1,
    dbOwner: 1,
    clusterMonitor: 1,
    hostManager: 1,
    clusterManager: 1,
    clusterAdmin: 1,
    root: 1,
    __system: 1
}

/************* TEST CASES ****************/

var tests = [
    {
        testname: "addShard",
        command: {addShard: "x"},
        skipStandalone: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "applyOps",
        command: {applyOps: "x"},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: {__system: 1}, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {__system: 1}, expectFail: true }
        ]
    },
    {
        testname: "aggregate_readonly",
        command: {aggregate: "foo", pipeline: []},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite },
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "aggregate_explain",
        command: {aggregate: "foo", explain: true, pipeline: [ {$match: {bar: 1}} ] },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite },
            { runOnDb: secondDbName,  rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "aggregate_write",
        command: {aggregate: "foo", pipeline: [ {$out: "foo_out"} ] },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_write },
            { runOnDb: secondDbName, rolesAllowed: {readWriteAnyDatabase: 1, root: 1, __system: 1} }
        ]
    },
    {
        testname: "buildInfo",
        command: {buildInfo: 1},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    },
    {
        testname: "checkShardingIndexFirstDB",
        command: {checkShardingIndex: firstDbName + ".x", keyPattern: {_id: 1} },
        skipSharded: true,
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite }
        ]
    },
    {
        testname: "checkShardingSecondDB",
        command: {checkShardingIndex: secondDbName + ".x", keyPattern: {_id: 1} },
        skipSharded: true,
        testcases: [
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "cloneCollection_1",
        command: {cloneCollection: firstDbName + ".x"},
        skipSharded: true,
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_write, expectFail: true }
        ]
    },
    {
        testname: "cloneCollection_2",
        command: {cloneCollection: secondDbName + ".x"},
        skipSharded: true,
        testcases: [
            {
                runOnDb: secondDbName,
                rolesAllowed: {readWriteAnyDatabase: 1, root: 1, __system: 1},
                expectFail: true
            }
        ]
    },
    {
        testname: "cloneCollectionAsCapped",
        command: {cloneCollectionAsCapped: "x", toCollection: "y", size: 1000},
        skipSharded: true,
        setup: function (db) { db.x.save( {} ); },
        teardown: function (db) {
            db.x.drop();
            db.y.drop();
        },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_write },
            { runOnDb: secondDbName, rolesAllowed: {readWriteAnyDatabase: 1, root: 1, __system: 1} }
        ]
    },
    {
        testname: "closeAllDatabases",
        command: {closeAllDatabases: "x"},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_hostManager },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "collMod",
        command: {collMod: "foo", usePowerOf2Sizes: true},
        setup: function (db) { db.foo.save( {} ); },
        teardown: function (db) { db.dropDatabase(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {dbAdmin: 1, dbAdminAnyDatabase: 1, root: 1, __system: 1} },
            { runOnDb: secondDbName, rolesAllowed: {dbAdminAnyDatabase: 1, root: 1, __system: 1} }
        ]
    },
    {
        testname: "collStats",
        command: {collStats: "bar", scale: 1},
        setup: function (db) { db.bar.save( {} ); },
        teardown: function (db) { db.dropDatabase(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {read: 1,
                                                   readAnyDatabase: 1,
                                                   readWrite: 1,
                                                   readWriteAnyDatabase: 1,
                                                   dbAdmin: 1,
                                                   dbAdminAnyDatabase: 1,
                                                   dbOwner: 1,
                                                   monitor: 1,
                                                   root: 1,
                                                   __system: 1
                                                  } },
            { runOnDb: secondDbName, rolesAllowed: {readAnyDatabase: 1,
                                                    readWriteAnyDatabase: 1,
                                                    dbAdminAnyDatabase: 1,
                                                    monitor: 1,
                                                    root: 1,
                                                    __system: 1
                                                   } }
            ]
    },
    {
        testname: "compact",
        command: {compact: "foo"},
        skipSharded: true,
        setup: function (db) { db.foo.save( {} ); },
        teardown: function (db) { db.dropDatabase(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {dbAdmin: 1, dbAdminAnyDatabase: 1, root: 1, __system: 1} },
            { runOnDb: secondDbName, rolesAllowed: {dbAdminAnyDatabase: 1, root: 1, __system: 1} }
        ]
    },
    {
        testname: "connectionStatus",
        command: {connectionStatus: 1},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    },
    {
        testname: "connPoolStats",
        command: {connPoolStats: 1},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_monitoring },
            { runOnDb: secondDbName, rolesAllowed: roles_monitoring }
        ]
    },
    {
        testname: "connPoolSync",
        command: {connPoolSync: 1},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_hostManager },
            { runOnDb: secondDbName, rolesAllowed: roles_hostManager }
        ]
    },
    /* SERVER-11098
    {
        testname: "convertToCapped",
        command: {convertToCapped: "toCapped", size: 1000},
        setup: function (db) { db.toCapped.save( {} ); },
        teardown: function (db) { db.toCapped.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_write },
            { runOnDb: secondDbName, rolesAllowed: {readWriteAnyDatabase: 1} }
        ]
    }, */
    {
        testname: "count",
        command: {count: "x"},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite },
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "create",
        command: {create: "x"},
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_writeDbAdmin },
            { runOnDb: secondDbName, rolesAllowed: roles_writeDbAdminAny }
        ]
    },
    {
        testname: "cursorInfo",
        command: {cursorInfo: 1},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_monitoring },
            { runOnDb: secondDbName, rolesAllowed: roles_monitoring }
        ]
    },
    {
        testname: "dataSize_1",
        command: {dataSize: firstDbName + ".x"},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite }
        ]
    },
    {
        testname: "dataSize_2",
        command: {dataSize: secondDbName + ".x"},
        testcases: [
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "dbHash",
        command: {dbHash: 1},
        skipSharded: true,
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite },
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "dbStats",
        command: {dbStats: 1, scale: 1024},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: { read: 1,
                                                    readAnyDatabase: 1,
                                                    readWrite: 1,
                                                    readWriteAnyDatabase: 1,
                                                    dbAdmin: 1,
                                                    dbAdminAnyDatabase: 1,
                                                    dbOwner: 1,
                                                    monitor: 1,
                                                    root: 1,
                                                    __system: 1
                                                  } },
            { runOnDb: secondDbName, rolesAllowed: { readAnyDatabase: 1,
                                                     readWriteAnyDatabase: 1,
                                                     dbAdminAnyDatabase: 1,
                                                     monitor: 1,
                                                     root: 1,
                                                     __system: 1
                                                   } }
        ]
    },
    {
        testname: "deleteIndexes",
        command: {deleteIndexes: "x", index: "*"},
        setup: function (db) {
            db.x.save({a: 3});
            db.x.ensureIndex({a: 1});
        },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_writeDbAdmin },
            { runOnDb: secondDbName, rolesAllowed: roles_writeDbAdminAny }
        ]
    },
    {
        testname: "diagLogging",
        command: {diagLogging: 1},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_hostManager },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "distinct",
        command: {distinct: "coll", key: "a", query: {}},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite },
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "drop",
        command: {drop: "x"},
        setup: function (db) { db.x.save({}); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_writeDbAdmin },
            { runOnDb: secondDbName, rolesAllowed: roles_writeDbAdminAny }
        ]
    },
    {
        testname: "dropDatabase",
        command: {dropDatabase: 1},
        setup: function (db) { db.x.save({}); },
        teardown: function (db) { db.x.save({}); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {dbAdmin: 1,
                                                   dbAdminAnyDatabase: 1,
                                                   clusterAdmin: 1,
                                                   root: 1,
                                                   __system: 1} },
            { runOnDb: secondDbName, rolesAllowed: {dbAdminAnyDatabase:1,
                                                    clusterAdmin: 1,
                                                    root: 1,
                                                    __system: 1} }
        ]
    },
    {
        testname: "dropIndexes",
        command: {dropIndexes: "x", index: "*"},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_writeDbAdmin },
            { runOnDb: secondDbName, rolesAllowed: roles_writeDbAdminAny }
        ]
    },
    {
        testname: "enableSharding",
        command: {enableSharding: "x"},
        skipStandalone: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "eval",
        command: {$eval: function () { print("noop"); } },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {__system: 1} },
            { runOnDb: secondDbName, rolesAllowed: {__system: 1} }
        ]
    },
    {
        testname: "features",
        command: {features: 1},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    },
    {
        testname: "filemd5",
        command: {filemd5: 1, root: "fs"},
        setup: function (db) {
            db.fs.chunks.drop();
            db.fs.chunks.insert({files_id: 1, n: 0, data: new BinData(0, "test")});
            db.fs.chunks.ensureIndex({files_id: 1, n: 1});
        },
        teardown: function (db) {
            db.fs.chunks.drop();
        },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite },
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "findAndModify",
        command: {findAndModify: "x", query: {_id: "abc"}, update: {$inc: {n: 1}}},
        setup: function (db) {
            db.x.drop();
            db.x.save( {_id: "abc", n: 0} );
        },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_write },
            { runOnDb: secondDbName, rolesAllowed: {readWriteAnyDatabase: 1, root: 1, __system: 1} }
        ]
    },
    {
        testname: "flushRouterConfig",
        command: {flushRouterConfig: 1},
        skipStandalone: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_hostManager },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "fsync",
        command: {fsync: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_hostManager },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "geoNear",
        command: {geoNear: "x", near: [50, 50], num: 1},
        setup: function (db) {
            db.x.drop();
            db.x.save({loc: [50, 50]});
            db.x.ensureIndex({loc: "2d"});
        },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite },
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "geoSearch",
        command: {geoSearch: "x", near: [50, 50], maxDistance: 6, limit: 1, search: {} },
        skipSharded: true,
        setup: function (db) {
            db.x.drop();
            db.x.save({loc: {long: 50, lat: 50}});
            db.x.ensureIndex({loc: "geoHaystack", type: 1}, {bucketSize: 1});
        },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite },
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "getCmdLineOpts",
        command: {getCmdLineOpts: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_monitoring },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "getLastError",
        command: {getLastError: 1},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    },
    {
        testname: "getLog",
        command: {getLog: "*"},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_monitoring },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "getnonce",
        command: {getnonce: 1},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    },
    {
        testname: "getParameter",
        command: {getParameter: 1, quiet: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_monitoring },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "getPrevError",
        command: {getPrevError: 1},
        skipSharded: true,
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    },
    {
        testname: "getoptime",
        command: {getoptime: 1},
        skipSharded: true,
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    },
    {
        testname: "getShardMap",
        command: {getShardMap: "x"},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_monitoring },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "getShardVersion",
        command: {getShardVersion: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_monitoring, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "group",
        command: {
            group: {
                ns: "x",
                key: {groupby: 1},
                initial: {total: 0},
                $reduce: function (curr, result) {
                    result.total += curr.n;
                }
            }
        },
        setup: function (db) {
            db.x.insert({groupby: 1, n: 5});
            db.x.insert({groupby: 1, n: 6});
        },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite },
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "hostInfo",
        command: {hostInfo: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_monitoring, },
            { runOnDb: firstDbName, rolesAllowed: roles_monitoring },
            { runOnDb: secondDbName, rolesAllowed: roles_monitoring }
        ]
    },
    {
        testname: "indexStats",
        command: {indexStats: "x", index: "a_1"},
        skipSharded: true, 
        setup: function (db) {
            db.x.save({a: 10});
            db.x.ensureIndex({a: 1});
        },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {dbAdmin: 1, dbAdminAnyDatabase: 1, root: 1,  __system: 1} },
            { runOnDb: secondDbName, rolesAllowed: {dbAdminAnyDatabase: 1, root: 1, __system: 1} }
        ]
    },
    {
        testname: "isMaster",
        command: {isMaster: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_all },
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    },
    {
        testname: "listCommands",
        command: {listCommands: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_all },
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    },
    {
        testname: "listDatabases",
        command: {listDatabases: 1},
        testcases: [
            {
                runOnDb: adminDbName,
                rolesAllowed: {
                    readAnyDatabase: 1,
                    readWriteAnyDatabase: 1,
                    monitor: 1,
                    clusterAdmin: 1,
                    root: 1,
                    __system: 1
                }
            },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "listShards",
        command: {listShards: 1},
        skipStandalone: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_monitoring },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "logRotate",
        command: {logRotate: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_hostManager, },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "mapReduce_readonly",
        command: {
            mapreduce: "x",
            map: function () { emit(this.groupby, this.n); },
            reduce: function (id,emits) { return Array.sum(emits); },
            out: {inline: 1}
        },
        setup: function (db) {
            db.x.insert({groupby: 1, n: 5});
            db.x.insert({groupby: 1, n: 6});
        },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite },
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny }
        ]
    },
    {
        testname: "mapReduce_write",
        command: {
            mapreduce: "x",
            map: function () { emit(this.groupby, this.n); },
            reduce: function (id,emits) { return Array.sum(emits); },
            out: "mr_out"
        },
        setup: function (db) {
            db.x.insert({groupby: 1, n: 5});
            db.x.insert({groupby: 1, n: 6});
        },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_write },
            { runOnDb: secondDbName, rolesAllowed: {readWriteAnyDatabase: 1, root: 1, __system: 1} }
        ]
    },
    {
        testname: "mergeChunks",
        command: {mergeChunks: "x", bounds: [{i : 0}, {i : 5}]},
        skipStandalone: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "moveChunk",
        command: {moveChunk: "x"},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "movePrimary",
        command: {movePrimary: "x"},
        skipStandalone: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "netstat",
        command: {netstat: "x"},
        skipStandalone: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_monitoring, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "ping",
        command: {ping: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_all },
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    },
    {
        testname: "profile",  
        command: {profile: 0},
        skipSharded: true,
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {dbAdmin: 1,
                                                   dbAdminAnyDatabase: 1,
                                                   root: 1,
                                                   __system: 1} },
            { runOnDb: secondDbName, rolesAllowed: {dbAdminAnyDatabase: 1, root: 1, __system: 1} }
        ]
    },
    {
        testname: "renameCollection_sameDb",
        command: {renameCollection: firstDbName + ".x", to: firstDbName + ".y"},
        setup: function (db) { db.getSisterDB(firstDbName).x.save( {} ); },
        teardown: function (db) {
            db.getSisterDB(firstDbName).x.drop();
            db.getSisterDB(firstDbName).y.drop();
        },
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_writeDbAdmin },
            /* SERVER-11085
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
            */
        ]
    },
    {
        testname: "renameCollection_twoDbs",
        command: {renameCollection: firstDbName + ".x", to: secondDbName + ".y"},
        setup: function (db) {
            db.getSisterDB(firstDbName).x.save( {} );
            db.getSisterDB(firstDbName).getLastError();
            db.getSisterDB(adminDbName).runCommand({movePrimary: firstDbName, to: shard0name});
            db.getSisterDB(adminDbName).runCommand({movePrimary: secondDbName, to: shard0name});
        },
        teardown: function (db) {
            db.getSisterDB(firstDbName).x.drop();
            db.getSisterDB(secondDbName).y.drop();
        },
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: {readWriteAnyDatabase: 1, root: 1, __system: 1} },
            /* SERVER-11085
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
            */
        ]
    },
    {
        testname: "reIndex",
        command: {reIndex: "x"},
        setup: function (db) { db.x.save( {} ); },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {dbAdmin: 1, dbAdminAnyDatabase: 1, root: 1, __system: 1} },
            { runOnDb: secondDbName, rolesAllowed: {dbAdminAnyDatabase: 1, root: 1, __system: 1} }
        ]
    },
    {
        testname: "removeShard",
        command: {removeShard: "x"},
        skipStandalone: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "repairDatabase",
        command: {repairDatabase: 1},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {dbAdmin: 1,
                                                   dbAdminAnyDatabase: 1,
                                                   hostManager: 1,
                                                   clusterAdmin: 1,
                                                   root: 1,
                                                   __system: 1} },
            { runOnDb: secondDbName, rolesAllowed: {dbAdminAnyDatabase: 1,
                                                    hostManager: 1,
                                                    clusterAdmin: 1,
                                                    root: 1,
                                                    __system: 1} },
            { runOnDb: adminDbName, rolesAllowed: {dbAdminAnyDatabase: 1,
                                                   hostManager: 1,
                                                   clusterAdmin: 1,
                                                   root: 1,
                                                   __system: 1} }
        ]
    },
    {
        testname: "replSetElect",
        command: {replSetElect: 1},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: {__system: 1}, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "replSetFreeze",
        command: {replSetFreeze: "x"},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "replSetFresh",
        command: {replSetFresh: "x"},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: {__system: 1}, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "replSetGetRBID",
        command: {replSetGetRBID: 1},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: {__system: 1}, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "replSetGetStatus",
        command: {replSetGetStatus: 1},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: {clusterMonitor: 1,
                                                   clusterManager: 1,
                                                   clusterAdmin: 1,
                                                   root: 1,
                                                   __system: 1}, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "replSetHeartbeat",
        command: {replSetHeartbeat: 1},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: {__system: 1}, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "replSetInitiate",
        command: {replSetInitiate: "x"},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "replSetMaintenance",
        command: {replSetMaintenance: "x"},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "replSetReconfig",
        command: {replSetReconfig: "x"},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "replSetStepDown",
        command: {replSetStepDown: "x"},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "replSetSyncFrom",
        command: {replSetSyncFrom: "x"},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "resetError",
        command: {resetError: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_all },
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    },
    {
        testname: "resync",
        command: {resync: 1},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: { hostManager: 1,
                                                    clusterManager: 1,
                                                    clusterAdmin: 1,
                                                    root: 1,
                                                    __system: 1}, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "serverStatus",
        command: {serverStatus: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_monitoring },
            { runOnDb: firstDbName, rolesAllowed: roles_monitoring },
            { runOnDb: secondDbName, rolesAllowed: roles_monitoring }
        ]
    },
    {
        testname: "setParameter",
        command: {setParameter: 1, quiet: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_hostManager },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "setShardVersion",
        command: {setShardVersion: 1},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: {__system: 1}, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "shardCollection",
        command: {shardCollection: "x"},
        skipStandalone: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "shardingState",
        command: {shardingState: 1},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_monitoring },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "shutdown",
        command: {shutdown: 1},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "split",
        command: {split: "x"},
        skipStandalone: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "splitChunk",
        command: {splitChunk: "x"},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "splitVector",
        command: {splitVector: "x"},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: roles_clusterManager, expectFail: true },
            { runOnDb: secondDbName, rolesAllowed: roles_clusterManager, expectFail: true }
        ]
    },
    {
        testname: "storageDetails",
        command: {storageDetails: "x", analyze: "diskStorage"},
        skipSharded: true,
        setup: function (db) { db.x.save( {} ); },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {dbAdmin: 1, dbAdminAnyDatabase: 1, root: 1, __system: 1} },
            { runOnDb: secondDbName, rolesAllowed: {dbAdminAnyDatabase: 1, root:1, __system: 1} }
        ]
    },
    {
        testname: "text",
        command: {text: "x"},
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: roles_readWrite, expectFail: true },
            { runOnDb: secondDbName, rolesAllowed: roles_readWriteAny, expectFail: true }
        ]
    },
    {
        testname: "top",
        command: {top: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_monitoring },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "touch",
        command: {touch: "x", data: true, index: false},
        skipSharded: true,
        setup: function (db) { db.x.save( {} ); },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_hostManager },
            { runOnDb: firstDbName, rolesAllowed: roles_hostManager },
            { runOnDb: secondDbName, rolesAllowed: roles_hostManager }
        ]
    },
    {
        testname: "unsetSharding",
        command: {unsetSharding: "x"},
        skipSharded: true,
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: {__system: 1}, expectFail: true },
            { runOnDb: firstDbName, rolesAllowed: {} },
            { runOnDb: secondDbName, rolesAllowed: {} }
        ]
    },
    {
        testname: "validate",
        command: {validate: "x"},
        setup: function (db) { db.x.save( {} ); },
        teardown: function (db) { db.x.drop(); },
        testcases: [
            { runOnDb: firstDbName, rolesAllowed: {dbAdmin: 1, dbAdminAnyDatabase: 1, root: 1, __system: 1} },
            { runOnDb: secondDbName, rolesAllowed: {dbAdminAnyDatabase: 1, root: 1, __system: 1} }
        ]
    },
    {
        testname: "whatsmyuri",
        command: {whatsmyuri: 1},
        testcases: [
            { runOnDb: adminDbName, rolesAllowed: roles_all },
            { runOnDb: firstDbName, rolesAllowed: roles_all },
            { runOnDb: secondDbName, rolesAllowed: roles_all }
        ]
    }
];

/************* TEST LOGIC ****************/

/**
 * Parameters:
 *   conn -- connection, either to standalone mongod,
 *      or to mongos in sharded cluster
 *   t -- a test object from the "tests" array above
 *   testcase -- the particular testcase from t to test
 *   r -- a role object from the "roles" array above
 *
 * Returns:
 *   An empty string on success, or an error string
 *   on test failure.
 */
function testProperAuthorization(conn, t, testcase, r) {
    var out = "";

    var runOnDb = conn.getDB(testcase.runOnDb);
    var adminDb = conn.getDB(adminDbName);
    if (t.setup) {
        adminDb.auth("admin", "password");
        t.setup(runOnDb);
        runOnDb.getLastError();
        adminDb.logout();
    }
    assert(r.db.auth("user|" + r.role, "password"));
    var res = runOnDb.runCommand(t.command);

    if (testcase.rolesAllowed[r.role]) {
        if (res.ok == 0 && res.code == authErrCode) {
            out = "expected authorization success" +
                  " but received " + tojson(res) + 
                  " on db " + testcase.runOnDb +
                  " with role " + r.key;
        }
        else if (res.ok == 0 && !testcase.expectFail) {
            out = "command failed with " + tojson(res) +
                  " on db " + testcase.runOnDb +
                  " with role " + r.key;
        }
        // test can provide a function that will run if
        // the command completed successfully
        else if (testcase.onSuccess) {
            testcase.onSuccess(res);
        }
    }
    else {
        if (res.ok == 1 || (res.ok == 0 && res.code != authErrCode)) {
            out = "expected authorization failure" +
                  " but received result " + tojson(res) +
                  " on db " + testcase.runOnDb +
                  " with role " + r.key;
        }
    }

    r.db.logout();
    if (t.teardown) {
        adminDb.auth("admin", "password");
        t.teardown(runOnDb);
        adminDb.logout();
    }
    return out;
}

function isMongos(conn) {
    var res = conn.getDB("admin").runCommand({isdbgrid: 1});
    return (res.ok == 1 && res.isdbgrid == 1);
}

/**
 * Runs a single test object from the "tests" array above.
 * Calls testProperAuthorization for each role-testcase 
 * combination.
 *
 * Parameters:
 *   conn -- a connection to either mongod or mongos
 *   t -- a single test object from the "tests" array
 *
 * Returns:
 *  An array of strings. Each string in the array reports
 *  a particular test error.
 */
function runOneTest(conn, t) {
    jsTest.log("Running test: " + t.testname);

    var failures = [];

    // some tests shouldn't run in a sharded environment
    if (t.skipSharded && isMongos(conn)) {
        return failures;
    }
    // others shouldn't run in a standalone environment
    if (t.skipStandalone && !isMongos(conn)) {
        return failures;
    }

    for (var i = 0; i < t.testcases.length; i++) {
        var testcase = t.testcases[i];
        for (var j = 0; j < roles.length; j++) {
            var msg = testProperAuthorization(conn, t, testcase, roles[j]);
            if (msg) {
                failures.push(t.testname + ": " + msg);
            }
        }
    }

    return failures;
}

function createUsers(conn) {
    var adminDb = conn.getDB(adminDbName);
    adminDb.addUser({
        user: "admin",
        pwd: "password",
        roles: ["userAdminAnyDatabase", "readWriteAnyDatabase", "clusterAdmin"]
    });

    assert(adminDb.auth("admin", "password"));
    for (var i = 0; i < roles.length; i++) {
        r = roles[i];
        r.db = conn.getDB(r.dbname);
        r.db.addUser({user: "user|" + r.role, pwd: "password", roles: [r.role]});
    }
    adminDb.logout();
}

function runTests(conn) {
    createUsers(conn);

    var failures = [];

    for (var i = 0; i < tests.length; i++) {
        res = runOneTest(conn, tests[i]);
        failures = failures.concat(res);
    }

    failures.forEach(function(i) { jsTest.log(i) });
    assert.eq(0, failures.length);
}

var opts = {
    auth:"",
    enableExperimentalIndexStatsCmd: "",
    enableExperimentalStorageDetailsCmd: ""
}

var conn = MongoRunner.runMongod(opts);
runTests(conn);
MongoRunner.stopMongod(conn);

conn = new ShardingTest({
    shards: 2,
    mongos: 1,
    keyFile: "jstests/libs/key1",
    other: { shardOptions: opts }
});
runTests(conn);
conn.stop();

