/**
 * Verifies that the mongos count explain query shape folds in the 'rawData' flag: a rawData:true
 * count explain reports a different queryShapeHash than a non-rawData count, and rawData:false is
 * normalized to absent (same hash as no rawData).
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, mongos: 1});

const db = st.s.getDB("test");
const coll = db[jsTestName()];
assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 2}]));

// Runs 'count' explain through mongos and returns the reported queryShapeHash.
function countExplainHash(rawData) {
    const countCmd = {count: coll.getName(), query: {a: {$gt: 0}}};
    if (rawData !== undefined) {
        countCmd.rawData = rawData;
    }
    const res = assert.commandWorked(db.runCommand({explain: countCmd, verbosity: "queryPlanner"}));
    assert(res.hasOwnProperty("queryShapeHash"), "count explain did not report a queryShapeHash", {
        res,
    });
    return res.queryShapeHash;
}

const hashAbsent = countExplainHash(undefined);
const hashFalse = countExplainHash(false);
const hashTrue = countExplainHash(true);

jsTest.log.info("count explain queryShapeHash values", {hashAbsent, hashFalse, hashTrue});

// rawData:false is normalized to absent -> same hash.
assert.eq(
    hashAbsent,
    hashFalse,
    "count explain with rawData:false should hash the same as without rawData",
);
// rawData:true must be distinct from the non-rawData shape.
assert.neq(
    hashTrue,
    hashAbsent,
    "count explain with rawData:true must hash differently from a non-rawData count",
);

st.stop();
