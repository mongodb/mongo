/**
 * Verify that the shards produce a $sortKey metafield when the router requires it, even in cases
 * when the shard pipeline does not have any dependencies on any fields. This serves as a regression
 * test for SERVER-103724.
 * @tags: [
 *   requires_fcv_83
 * ]
 */

import {normalizeArray} from "jstests/libs/golden_test.js";
import {code, line, section, subSection} from "jstests/libs/query/pretty_md.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});
const db = st.getDB("test");
const coll = db[jsTestName()];
coll.drop();

section("Collection setup");
line("Creating index and shard key");
code(tojson({a: 1}));
assert.commandWorked(coll.createIndex({a: 1}));
assert(st.adminCommand({shardcollection: coll.getFullName(), key: {a: 1}}));

// Now split the and move the data between the shards
line("Split point");
code(tojson({a: 0}));
assert(st.adminCommand({split: coll.getFullName(), middle: {a: 0}}));
assert(
    st.adminCommand({
        moveChunk: coll.getFullName(),
        find: {a: 1},
        to: st.getOther(st.getPrimaryShard("test")).name,
        _waitForDelete: true,
    }),
);

const docs = [
    {
        "_id": 0,
        "m": NumberInt(0),
        "a": NumberInt(0),
    },
    {
        "_id": 1,
        "m": NumberInt(0),
        "a": NumberInt(10),
    },
    {
        "_id": 2,
        "m": NumberInt(0),
        "a": NumberInt(-10),
    },
];
line("Inserting documents");
code(tojson(docs));
assert.commandWorked(coll.insert(docs));

const pipeline = [{"$sort": {"m": 1}}, {"$addFields": {"a": NumberInt(0)}}, {"$project": {"_id": 0, "a": 1}}];

subSection("Pipeline");
code(tojson(pipeline));

subSection("Results");
const results = coll.aggregate(pipeline).toArray();
code(normalizeArray(results));

st.stop();
