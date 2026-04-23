/**
 * Tests expected operations on time-series collections produce oplog entries with
 * isTimeseries set to true.
 * 
 * @tags: [
 *  requires_fcv_83,
 *  assumes_against_mongod_not_mongos,
 *  requires_capped,
 *  requires_getmore,
 *  requires_replication,
 *  requires_timeseries,
 *  featureFlagMarkTimeseriesEventsInOplog,
 *  incompatible_with_snapshot_reads,
 *  no_selinux,
 * ]

 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDB = db.getSiblingDB(jsTestName());
const collName = "ts";

// Skips for burnin
if (FixtureHelpers.isMongos(db)) {
    jsTestLog("Skipping test because it is not compatible with mongos");
    quit();
}
if (!FixtureHelpers.isReplSet(db)) {
    jsTestLog("Skipping test because it is not compatible with standalone");
    quit();
}

// Test creation oplog entry
assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
let opEntries = db.getSiblingDB("local").runCommand({
    find: "oplog.rs",
    filter: {
        op: "c",
        ns: `${jsTestName()}.$cmd`,
        "o.create": {$exists: true},
        isTimeseries: true,
    },
    limit: 1,
    singleBatch: true,
    readConcern: {level: "local"},
}).cursor.firstBatch;
assert.eq(opEntries.length, 1, tojson(opEntries));

// Test oplog entries from index creation
const testcoll = testDB.getCollection(collName);
assert.commandWorked(testcoll.insert({t: ISODate(), m: 1, a: 1}));
assert.commandWorked(testcoll.createIndex({m: 1}));
opEntries = db.getSiblingDB("local").runCommand({
    find: "oplog.rs",
    filter: {
        op: "c",
        ns: `${jsTestName()}.$cmd`,
        "o.createIndexes": {$exists: true},
        "o.key.meta": 1,
        isTimeseries: true,
    },
    limit: 1,
    singleBatch: true,
    readConcern: {level: "local"},
}).cursor.firstBatch;
assert.eq(opEntries.length, 1, tojson(opEntries));
opEntries = db.getSiblingDB("local").runCommand({
    find: "oplog.rs",
    filter: {
        op: "c",
        ns: `${jsTestName()}.$cmd`,
        "o.startIndexBuild": {$exists: true},
        "o.indexes.key.meta": 1,
        isTimeseries: true,
    },
    limit: 1,
    singleBatch: true,
    readConcern: {level: "local"},
}).cursor.firstBatch;
assert.eq(opEntries.length, 1, tojson(opEntries));
opEntries = db.getSiblingDB("local").runCommand({
    find: "oplog.rs",
    filter: {
        op: "c",
        ns: `${jsTestName()}.$cmd`,
        "o.commitIndexBuild": {$exists: true},
        "o.indexes.key.meta": 1,
        isTimeseries: true,
    },
    limit: 1,
    singleBatch: true,
    readConcern: {level: "local"},
}).cursor.firstBatch;
assert.eq(opEntries.length, 1, tojson(opEntries));

// Test modifications
assert.commandWorked(
    testDB.runCommand({
        collMod: collName,
        timeseriesBucketsMayHaveMixedSchemaData: true,
    }),
);
opEntries = db.getSiblingDB("local").runCommand({
    find: "oplog.rs",
    filter: {
        op: "c",
        ns: `${jsTestName()}.$cmd`,
        "o.collMod": {$exists: true},
        isTimeseries: true,
    },
    limit: 1,
    singleBatch: true,
    readConcern: {level: "local"},
}).cursor.firstBatch;
assert.eq(opEntries.length, 1, tojson(opEntries));

// Test teardown
assert.commandWorked(testDB.runCommand({dropIndexes: collName, index: "m_1_t_1"}));
opEntries = db.getSiblingDB("local").runCommand({
    find: "oplog.rs",
    filter: {
        op: "c",
        ns: `${jsTestName()}.$cmd`,
        "o.dropIndexes": {$exists: true},
        isTimeseries: true,
    },
    limit: 1,
    singleBatch: true,
    readConcern: {level: "local"},
}).cursor.firstBatch;
assert.eq(opEntries.length, 1, tojson(opEntries));
assert.commandWorked(testDB.runCommand({drop: collName}));
opEntries = db.getSiblingDB("local").runCommand({
    find: "oplog.rs",
    filter: {
        op: "c",
        ns: `${jsTestName()}.$cmd`,
        "o.drop": {$exists: true},
        isTimeseries: true,
    },
    limit: 1,
    singleBatch: true,
    readConcern: {level: "local"},
}).cursor.firstBatch;
assert.eq(opEntries.length, 1, tojson(opEntries));
