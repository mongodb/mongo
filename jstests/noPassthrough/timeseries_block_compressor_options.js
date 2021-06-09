/**
 * Tests the collection block compressor during table creation for the following scenarios:
 * 1. The default collection block compressor for regular collections is snappy and can be
 *    configured globally.
 * 2. The default collection block compressor for time-series collections is zstd and ignores the
 *    configured global.
 * 3. The collection block compressor passed into the 'create' command has the highest precedence
 *    for all types of collections.
 *
 * @tags: [requires_persistence, requires_wiredtiger]
 */
(function() {
"use strict";

jsTestLog("Scenario 1a: testing the default compressor for regular collections");
let conn = MongoRunner.runMongod({});

// The default for regular collections is snappy.
assert.commandWorked(conn.getDB("db").createCollection("a"));
let stats = conn.getDB("db").getCollection("a").stats();
assert(stats["wiredTiger"]["creationString"].search("block_compressor=snappy") > -1);

MongoRunner.stopMongod(conn);

jsTestLog("Scenario 1b: testing the globally configured compressor for regular collections");
conn = MongoRunner.runMongod({wiredTigerCollectionBlockCompressor: "none"});

assert.commandWorked(conn.getDB("db").createCollection("a"));
stats = conn.getDB("db").getCollection("a").stats();
assert(stats["wiredTiger"]["creationString"].search("block_compressor=none") > -1);

MongoRunner.stopMongod(conn);

jsTestLog("Scenario 2a: testing the default compressor for time-series collections");
conn = MongoRunner.runMongod({});

// The default for time-series collections is zstd.
const timeFieldName = 'time';
assert.commandWorked(
    conn.getDB("db").createCollection("a", {timeseries: {timeField: timeFieldName}}));
stats = conn.getDB("db").getCollection("a").stats();
assert(stats["wiredTiger"]["creationString"].search("block_compressor=zstd") > -1);

MongoRunner.stopMongod(conn);

jsTestLog("Scenario 2b: testing the globally configured compressor for time-series collections");
conn = MongoRunner.runMongod({wiredTigerCollectionBlockCompressor: "none"});

// Time-series collections ignore the globally configured compressor
assert.commandWorked(
    conn.getDB("db").createCollection("a", {timeseries: {timeField: timeFieldName}}));
stats = conn.getDB("db").getCollection("a").stats();
assert(stats["wiredTiger"]["creationString"].search("block_compressor=zstd") > -1);

MongoRunner.stopMongod(conn);

jsTestLog("Scenario 3: testing the compressor passed into the 'create' command");

// The globally configured compressor will be ignored.
conn = MongoRunner.runMongod({wiredTigerCollectionBlockCompressor: "none"});
assert.commandWorked(conn.getDB("db").createCollection(
    "a", {storageEngine: {wiredTiger: {configString: "block_compressor=zlib"}}}));
assert.commandWorked(conn.getDB("db").createCollection("b", {
    storageEngine: {wiredTiger: {configString: "block_compressor=zlib"}},
    timeseries: {timeField: timeFieldName}
}));

stats = conn.getDB("db").getCollection("a").stats();
jsTestLog(stats);
assert(stats["wiredTiger"]["creationString"].search("block_compressor=zlib") > -1);

stats = conn.getDB("db").getCollection("b").stats();
assert(stats["wiredTiger"]["creationString"].search("block_compressor=zlib") > -1);

MongoRunner.stopMongod(conn);
}());
