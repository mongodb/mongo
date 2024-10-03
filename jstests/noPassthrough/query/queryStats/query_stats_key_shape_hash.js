/**
 * This test confirms that the hash generated alongside query stats entries and shapes is unique and
 * stable across restarts.
 */
import {
    getQueryStatsFindCmd,
    getQueryStatsKeyHashes,
    getQueryStatsShapeHashes
} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({name: 'queryStatsKeyShapeHashTest', nodes: 2});

// Turn on the collecting of query stats metrics.
replTest.startSet({setParameter: {internalQueryStatsRateLimit: -1}});
replTest.initiate();

const conn = replTest.getPrimary();
const collName = jsTestName();
const coll = conn.getDB("test")[collName];
coll.drop();
coll.insert({x: 5});

let totalKeys = 0;
let totalShapes = 0;

// Simple query, different field names.
coll.find({x: 5}).toArray();
++totalKeys;
++totalShapes;
coll.find({y: 5}).toArray();
++totalKeys;
++totalShapes;

// Simple $regex query, sort, and limit.
coll.find({x: {$regex: ".*"}}).sort({y: -1}).limit(10).toArray();
++totalKeys;
++totalShapes;
coll.find({x: {$regex: "."}}).sort({y: -1}).limit(10).toArray();
// Don't increment counters - different $regex value still means the same shape.
coll.find({x: {$regex: "."}}).sort({y: -1}).limit(10).toArray();

// Simple query with projection and limit.
coll.find({x: {$regex: ".*"}}, {_id: 0, x: "$$ROOT", y: {$bsonSize: ["$$ROOT"]}})
    .limit(5)
    .toArray();
++totalKeys;
++totalShapes;
coll.find({x: {$regex: ".*"}}, {_id: 0, x: "$$ROOT", y: {$bsonSize: ["$$ROOT"]}})
    .limit(10)
    .toArray();
// Don't increment counters - different limit value still means the same shape.
coll.find({x: {$regex: ".*"}}, {_id: false, x: "$$ROOT", y: {$bsonSize: ["$$ROOT"]}})
    .limit(5)
    .toArray();
// Don't increment counters - 'false' projection is the same as '0'.
coll.find({x: {$regex: ".*"}}, {_id: 0, x: "$$ROOT", y: {$bsonSize: ["$$CURRENT"]}})
    .limit(5)
    .toArray();
++totalKeys;  // different $bsonSize argument
++totalShapes;
coll.find({x: {$regex: ".*"}}, {x: "$$ROOT", y: {$bsonSize: ["$$ROOT"]}, _id: true})
    .limit(5)
    .toArray();
++totalKeys;  // explicit '_id' inclusion projection
++totalShapes;
coll.find({x: {$regex: ".*"}}, {x: "$$ROOT", y: {$bsonSize: ["$$ROOT"]}}).limit(5).toArray();
// Don't increment counters - '_id' inclusion projection is implicit.

// Different command options.
const filter = {
    x: 5
};
const maxTimeMS = 123;
const batchSize = 10;
const readPref = "primary";
// Vary values for the options specified - key should not change for 'maxTimeMS' and
// 'batchSize', but should for 'readPreference'; the shape does not change for any of them.
coll.find(filter).maxTimeMS(maxTimeMS).batchSize(batchSize).readPref(readPref).toArray();
++totalKeys;
coll.find(filter).maxTimeMS(456).batchSize(batchSize).readPref(readPref).toArray();
coll.find(filter).maxTimeMS(maxTimeMS).batchSize(20).readPref(readPref).toArray();
coll.find(filter).maxTimeMS(maxTimeMS).batchSize(batchSize).readPref("nearest").toArray();
++totalKeys;

const untransformedEntries = getQueryStatsFindCmd(conn, {collName, transformIdentifiers: false});
const untransformedKeyHashes = getQueryStatsKeyHashes(untransformedEntries);
const untransformedShapeHashes = getQueryStatsShapeHashes(untransformedEntries);

// Ensure that we got as many unique key hashes as we expect give the queries that we ran.
assert.eq(untransformedKeyHashes.length, totalKeys, tojson(untransformedEntries));
// We have less shapes than entries because the query shape includes fewer query options to
// differentiate entries with. Verify that we kept a correct track of them.
assert.eq(untransformedShapeHashes.length, totalShapes, tojson(untransformedEntries));

// We need to filter on the HMAC-ed collection name when we set {transformIdentifiers: true}.
const transformedCollName = "uxMLCvpiJ5N/IRqt4c28/2fNlHUyCLcxnrHfncmv1vs=";
const transformedEntries =
    getQueryStatsFindCmd(conn, {collName: transformedCollName, transformIdentifiers: true});
const transformedKeyHashes = getQueryStatsKeyHashes(transformedEntries);
const transformedShapeHashes = getQueryStatsShapeHashes(transformedEntries);

// We expect the hash to be derived from the untransformed shape, so the transformed $queryStats
// call should produce the same hash values as the untransformed one.
assert.sameMembers(untransformedKeyHashes,
                   transformedKeyHashes,
                   `untransformedEntries = ${tojson(untransformedEntries)}, transformedEntries = ${
                       tojson(transformedEntries)}`);
assert.sameMembers(untransformedShapeHashes,
                   transformedShapeHashes,
                   `untransformedEntries = ${tojson(untransformedEntries)}, transformedEntries = ${
                       tojson(transformedEntries)}`);

assert.sameMembers(
    [
        "02E273ADB7DC245BBE9024B173F6921D5D21DD166DB10AC7AC7684B957B755A5",
        "D530C5E14E37A7D6A92A2E984CB9C7D99AAA1FD1F670E18441460D7A69B88DE4",
        "EDE499D8DCE24B15D9323D0ACCF5C6474D8A742D7D352A9896E098FEE18E94B9",
        "1266E64A0C1CED3971E1AE852925F1C34C291DFC8EC6EEEEC96783B8837215BD",
        "5A4285B8F9C7CB62B94A85AC6272459E3C91D945474C3013163608C96F80F72A",
        "49124828ED6D64F9A68D9A38EFE1F1C73952FD6086FEA7B836C38310B1176241"
    ],
    untransformedShapeHashes,
    `untransformedEntries = ${tojson(untransformedEntries)}, transformedEntries = ${
        tojson(transformedEntries)}`);

replTest.stopSet();
