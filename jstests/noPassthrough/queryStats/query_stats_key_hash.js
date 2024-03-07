/**
 * This test confirms that the hash generated alongside query stats entries is unique and stable
 * across restarts.
 */
import {getQueryStatsFindCmd, getQueryStatsKeyHashes} from "jstests/libs/query_stats_utils.js";

const replTest = new ReplSetTest({name: 'queryStatsKeyHashTest', nodes: 2});

// Turn on the collecting of query stats metrics.
replTest.startSet({setParameter: {internalQueryStatsRateLimit: -1}});
replTest.initiate();

const conn = replTest.getPrimary();
const collName = jsTestName();
const coll = conn.getDB("test")[collName];
coll.drop();
coll.insert({x: 5});

let totalShapes = 0;

// Simple query, different field names.
coll.find({x: 5}).toArray();
++totalShapes;
coll.find({y: 5}).toArray();
++totalShapes;

// Simple $regex query, sort, and limit.
coll.find({x: {$regex: ".*"}}).sort({y: -1}).limit(10).toArray();
++totalShapes;
coll.find({x: {$regex: "."}}).sort({y: -1}).limit(10).toArray();
// Don't increment 'totalShapes' - different $regex value still means the same shape.
coll.find({x: {$regex: "."}}).sort({y: -1}).limit(10).toArray();

// Simple query with projection and limit.
coll.find({x: {$regex: ".*"}}, {_id: 0, x: "$$ROOT", y: {$bsonSize: ["$$ROOT"]}})
    .limit(5)
    .toArray();
++totalShapes;
coll.find({x: {$regex: ".*"}}, {_id: 0, x: "$$ROOT", y: {$bsonSize: ["$$ROOT"]}})
    .limit(10)
    .toArray();
// Don't increment 'totalShapes' - different limit value still means the same shape.
coll.find({x: {$regex: ".*"}}, {_id: false, x: "$$ROOT", y: {$bsonSize: ["$$ROOT"]}})
    .limit(5)
    .toArray();
// Don't increment 'totalShapes' - 'false' projection is the same as '0'.
coll.find({x: {$regex: ".*"}}, {_id: 0, x: "$$ROOT", y: {$bsonSize: ["$$CURRENT"]}})
    .limit(5)
    .toArray();
++totalShapes;  // different $bsonSize argument
coll.find({x: {$regex: ".*"}}, {x: "$$ROOT", y: {$bsonSize: ["$$ROOT"]}, _id: true})
    .limit(5)
    .toArray();
++totalShapes;  // explicit '_id' inclusion projection
coll.find({x: {$regex: ".*"}}, {x: "$$ROOT", y: {$bsonSize: ["$$ROOT"]}}).limit(5).toArray();
// Don't increment 'totalShapes' - '_id' inclusion projection is implicit.

// Different command options.
const filter = {
    x: 5
};
const maxTimeMS = 123;
const batchSize = 10;
const readPref = "primary";
coll.find(filter).maxTimeMS(maxTimeMS).batchSize(batchSize).readPref(readPref).toArray();
++totalShapes;
// Vary values for the options specified - shape should not change for 'maxTimeMS' and
// 'batchSize', but should for 'readPreference'.
coll.find(filter).maxTimeMS(456).batchSize(batchSize).readPref(readPref).toArray();
coll.find(filter).maxTimeMS(maxTimeMS).batchSize(20).readPref(readPref).toArray();
coll.find(filter).maxTimeMS(maxTimeMS).batchSize(batchSize).readPref("nearest").toArray();
++totalShapes;

const untransformedEntries = getQueryStatsFindCmd(conn, {collName, transformIdentifiers: false});
const untransformedKeyHashes = getQueryStatsKeyHashes(untransformedEntries);

// Ensure that we got as many unique key hashes as we expect give the queries that we ran.
assert.eq(untransformedKeyHashes.length, totalShapes, tojson(untransformedEntries));

// We need to filter on the HMAC-ed collection name when we set {transformIdentifiers: true}.
const transformedCollName = "h52faC1z+jJCQp/Hq008ffChPpnk/nhAgo70uX1FFFI=";
const transformedEntries =
    getQueryStatsFindCmd(conn, {collName: transformedCollName, transformIdentifiers: true});
const transformedKeyHashes = getQueryStatsKeyHashes(transformedEntries);

// We expect the hash to be derived from the untransformed shape, so the transformed $queryStats
// call should produce the same hash values as the untransformed one.
assert.sameMembers(untransformedKeyHashes,
                   transformedKeyHashes,
                   `untransformedEntries = ${tojson(untransformedEntries)}, transformedEntries = ${
                       tojson(transformedEntries)}`);

replTest.stopSet();
