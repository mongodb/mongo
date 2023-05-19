/**
 * Tests that the "_configsvrRunRestore" command removes documents in config collections not
 * referenced in the "local.system.collections_to_restore" collection.
 *
 * @tags: [
 *      requires_persistence,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");

const s =
    new ShardingTest({name: "runRestore", shards: 2, mongos: 1, config: 1, other: {chunkSize: 1}});

let mongos = s.s0;
let db = s.getDB("test");
if (!FeatureFlagUtil.isEnabled(s.configRS.getPrimary().getDB("test"), "SelectiveBackup")) {
    jsTestLog("Skipping as featureFlagSelectiveBackup is not enabled");
    s.stop();
    return;
}

s.adminCommand({enablesharding: "test"});
s.ensurePrimaryShard("test", s.shard1.shardName);
s.adminCommand({shardcollection: "test.a", key: {x: 1}});
s.adminCommand({shardcollection: "test.b", key: {x: 1}});

s.adminCommand({enablesharding: "unusedDB"});
s.ensurePrimaryShard("unusedDB", s.shard0.shardName);

let primary = s.getPrimaryShard("test").getDB("test");
let primaryName = s.getPrimaryShard("test").shardName;

let secondary = s.getOther(primary).getDB("test");
let secondaryName = s.getOther(primary).shardName;

for (let i = 0; i < 6; i++) {
    assert.commandWorked(db.getCollection("a").insert({x: i}));
    assert.commandWorked(db.getCollection("b").insert({x: i}));

    // Split chunks we just inserted.
    assert.commandWorked(mongos.adminCommand({split: "test.a", middle: {x: i}}));
    assert.commandWorked(mongos.adminCommand({split: "test.b", middle: {x: i}}));
}

const aCollUUID =
    mongos.getDB("config").getCollection("collections").find({_id: "test.a"}).toArray()[0].uuid;
const bCollUUID =
    mongos.getDB("config").getCollection("collections").find({_id: "test.b"}).toArray()[0].uuid;

for (const uuid of [aCollUUID, bCollUUID]) {
    assert.eq(7,
              mongos.getDB("config")
                  .getCollection("chunks")
                  .find({uuid: uuid, shard: primaryName})
                  .count());
    assert.eq(0,
              mongos.getDB("config")
                  .getCollection("chunks")
                  .find({uuid: uuid, shard: secondaryName})
                  .count());
}

// Move chunks between shards.
for (const x of [0, 2, 4]) {
    assert.commandWorked(s.s0.adminCommand(
        {moveChunk: "test.a", find: {x: x}, to: secondary.getMongo().name, _waitForDelete: true}));
    assert.commandWorked(s.s0.adminCommand(
        {moveChunk: "test.b", find: {x: x}, to: secondary.getMongo().name, _waitForDelete: true}));
}

// Check config collection counts.
for (const uuid of [aCollUUID, bCollUUID]) {
    assert.eq(4,
              mongos.getDB("config")
                  .getCollection("chunks")
                  .find({uuid: uuid, shard: primaryName})
                  .count());
    assert.eq(3,
              mongos.getDB("config")
                  .getCollection("chunks")
                  .find({uuid: uuid, shard: secondaryName})
                  .count());
}

assert.eq(1, mongos.getDB("config").getCollection("collections").find({_id: "test.a"}).count());
assert.eq(1, mongos.getDB("config").getCollection("collections").find({_id: "test.b"}).count());

assert.eq(1, mongos.getDB("config").getCollection("databases").find({_id: "test"}).count());

assert.eq(1, mongos.getDB("config").getCollection("databases").find({_id: "unusedDB"}).count());

s.stop({noCleanData: true});

const configDbPath = s.c0.dbpath;

// Start the config server in standalone mode.
let conn = MongoRunner.runMongod({noCleanData: true, dbpath: configDbPath});
assert(conn);

// Can't run the "_configsvrRunRestore" command without --restore.
assert.commandFailedWithCode(conn.getDB("admin").runCommand({_configsvrRunRestore: 1}),
                             ErrorCodes.CommandFailed);

MongoRunner.stopMongod(conn);

// Start the config server in standalone restore mode.
conn = MongoRunner.runMongod({noCleanData: true, dbpath: configDbPath, restore: ""});
assert(conn);

assert.commandWorked(conn.getDB("admin").runCommand({setParameter: 1, logLevel: 1}));

// Can't run if the "local.system.collections_to_restore" collection is missing.
assert.commandFailedWithCode(conn.getDB("admin").runCommand({_configsvrRunRestore: 1}),
                             ErrorCodes.NamespaceNotFound);

let [_, uuidStr] = aCollUUID.toString().match(/"((?:\\.|[^"\\])*)"/);
assert.commandWorked(conn.getDB("local").getCollection("system.collections_to_restore").insert({
    ns: "test.a",
    uuid: uuidStr
}));

// The "local.system.collections_to_restore" collection must have UUID as the correct type.
assert.commandFailedWithCode(conn.getDB("admin").runCommand({_configsvrRunRestore: 1}),
                             ErrorCodes.BadValue);

// Recreate the "local.system.collections_to_restore" collection and insert 'test.a'.
assert(conn.getDB("local").getCollection("system.collections_to_restore").drop());
assert.commandWorked(conn.getDB("local").createCollection("system.collections_to_restore"));

assert.commandWorked(conn.getDB("local").getCollection("system.collections_to_restore").insert({
    ns: "test.a",
    uuid: aCollUUID
}));

assert.commandWorked(conn.getDB("admin").runCommand({_configsvrRunRestore: 1}));

assert.eq(4,
          conn.getDB("config")
              .getCollection("chunks")
              .find({uuid: aCollUUID, shard: primaryName})
              .count());
assert.eq(3,
          conn.getDB("config")
              .getCollection("chunks")
              .find({uuid: aCollUUID, shard: secondaryName})
              .count());

assert.eq(0,
          conn.getDB("config")
              .getCollection("chunks")
              .find({uuid: bCollUUID, shard: primaryName})
              .count());
assert.eq(0,
          conn.getDB("config")
              .getCollection("chunks")
              .find({uuid: bCollUUID, shard: secondaryName})
              .count());

assert.eq(1, conn.getDB("config").getCollection("collections").find({_id: "test.a"}).count());
assert.eq(0, conn.getDB("config").getCollection("collections").find({_id: "test.b"}).count());

assert.eq(1, conn.getDB("config").getCollection("databases").find({_id: "test"}).count());
assert.eq(0, conn.getDB("config").getCollection("databases").find({_id: "unusedDB"}).count());

MongoRunner.stopMongod(conn);

// Start the config server in standalone restore mode.
conn = MongoRunner.runMongod({noCleanData: true, dbpath: configDbPath, restore: ""});
assert(conn);

// '_configsvrRunRestore' command ignores cache collections.
assert.commandWorked(conn.getDB("config").createCollection("cache.test"));
assert.commandWorked(conn.getDB("admin").runCommand({_configsvrRunRestore: 1}));

// Can't run during testing if the config server has unrecognized collections.
assert.commandWorked(conn.getDB("config").createCollection("unknown"));
let error = assert.throws(function() {
    conn.getDB("admin").runCommand({_configsvrRunRestore: 1});
});
assert(isNetworkError(error));

// The server should have crashed from fatally asserting on unknown config collection.
MongoRunner.stopMongod(conn, null, {allowedExitCode: MongoRunner.EXIT_ABORT});
}());
