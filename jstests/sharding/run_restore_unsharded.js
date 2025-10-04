/**
 * Tests that the "_configsvrRunRestore" command restores databases with unsharded collections
 * referenced in the "local.system.collections_to_restore" collection.
 *
 * @tags: [
 *      requires_persistence,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Because we restart nodes in standalone mode, it's possible for fast count, which doesn't
// discriminate between majority committed data and locally committed data, and the true count,
// which only includes majority committed data on standalones, to diverge. Therefore skip
// validating fast count.
TestData.skipEnforceFastCountOnValidate = true;

const s = new ShardingTest({name: "runRestoreUnsharded", shards: 2, mongos: 1, config: 1, other: {chunkSize: 1}});

let mongos = s.s0;
let db = s.getDB("test");

// Create an unsharded collection.
assert.commandWorked(db.createCollection("a"));
const collUUID = mongos.getDB("test").runCommand({listCollections: 1}).cursor.firstBatch[0].info.uuid;

// Only sharded collections appear in config.collections
assert.eq(0, mongos.getDB("config").getCollection("collections").find({_id: "test.a"}).count());

assert.eq(1, mongos.getDB("config").getCollection("databases").find({_id: "test"}).count());

s.stop({noCleanData: true});

const configDbPath = s.c0.dbpath;

// Start the config server in standalone restore mode.
let conn = MongoRunner.runMongod({noCleanData: true, dbpath: configDbPath, restore: "", maintenanceMode: "standalone"});
assert(conn);

assert.commandWorked(conn.getDB("admin").runCommand({setParameter: 1, logLevel: 1}));

// Create the "local.system.collections_to_restore" collection and insert "test.a".
assert.commandWorked(conn.getDB("local").createCollection("system.collections_to_restore"));
assert.commandWorked(
    conn.getDB("local").getCollection("system.collections_to_restore").insert({
        ns: "test.a",
        uuid: collUUID,
    }),
);

assert.commandWorked(conn.getDB("admin").runCommand({_configsvrRunRestore: 1}));

// Only sharded collections appear in config.collections
assert.eq(0, conn.getDB("config").getCollection("collections").find({_id: "test.a"}).count());

assert.eq(1, conn.getDB("config").getCollection("databases").find({_id: "test"}).count());

MongoRunner.stopMongod(conn);
