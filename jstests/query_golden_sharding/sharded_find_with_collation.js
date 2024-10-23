/**
 * Tests for the absence/presence of a shard filtering stage for queries which may/may not
 * be single shard targeted.
 * TODO SERVER-94611: Extend testing once sharding may have non-simple collation.
 */
import {configureFailPoint} from 'jstests/libs/fail_point_util.js';
import {
    code,
    outputShardedFindSummaryAndResults,
    section,
    subSection
} from "jstests/libs/pretty_md.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const db = st.getDB("test");

// Enable sharding.
const primaryShard = st.shard0.shardName;
assert.commandWorked(st.s0.adminCommand({enableSharding: db.getName(), primaryShard}));

const coll = db[jsTestName()];
const setupCollection = ({shardKey, splits}) => {
    coll.drop();

    let doc = {_id: "a", a: "a"};

    let [shardField] = Object.keys(shardKey);

    assert.commandWorked(db.createCollection(coll.getName()));

    // Shard the collection on with the provided spec, implicitly creating an index with simple
    // collation.
    assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

    // Split the collection. e.g.,
    // shard0: [chunk 1] { <shardField> : { "$minKey" : 1 } } -->> { <shardField> : 0 }
    // shard0: [chunk 2] { <shardField> : 0 } -->> { <shardField> : "b"}
    // shard1: [chunk 3] { <shardField> : "b" } -->> { <shardField> : { "$maxKey" : 1 }}
    // Chunk 2 will be moved between the shards.
    for (let mid of splits) {
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: mid}));
    }

    assert.commandWorked(db.adminCommand(
        {moveChunk: coll.getFullName(), find: {[shardField]: MinKey}, to: st.shard0.shardName}));
    assert.commandWorked(db.adminCommand(
        {moveChunk: coll.getFullName(), find: {[shardField]: doc.a}, to: st.shard0.shardName}));
    assert.commandWorked(db.adminCommand(
        {moveChunk: coll.getFullName(), find: {[shardField]: MaxKey}, to: st.shard1.shardName}));

    // Put data on shard0, that will go into chunk 2.
    assert.commandWorked(coll.insert(doc));

    // Perform a chunk migration of chunk 2 from shard0 to shard1, but do not clean
    // up orphans on shard0.
    db.adminCommand({moveChunk: coll.getFullName(), find: doc, to: st.shard1.shardName});
};

const caseInsensitive = {
    locale: 'en_US',
    strength: 2
};

const doQueries = (withCollation) => {
    let evalFn = (query) => {
        let queryObj = coll.find(query);
        if (withCollation) {
            queryObj = queryObj.collation(caseInsensitive);
        }
        let results = outputShardedFindSummaryAndResults(queryObj);
        // Only one document was inserted, and all queries should include it.
        // Anything greater than one may indicate the query is not correctly shard
        // filtering, and is seeing orphans.
        assert.eq(1, results.length);
    };
    for (let fieldName of ["a", "_id"]) {
        // Equality.
        evalFn({[fieldName]: "a"});
        // Ranges within a single chunk.
        evalFn({[fieldName]: {"$lte": "a"}});
        evalFn({[fieldName]: {"$gte": "a"}});
        evalFn({[fieldName]: {"$gte": "a", "$lte": "a"}});
        // These queries would return values from any chunk.
        evalFn({[fieldName]: {"$gt": MinKey}});
        evalFn({[fieldName]: {"$lt": MaxKey}});
        evalFn({[fieldName]: {"$gt": MinKey, "$lt": MaxKey}});
    }
};

let suspendRangeDeletionShard0;

for (let shardKey of [{a: 1}, {_id: 1}, {a: "hashed"}, {_id: "hashed"}]) {
    let shardKeyJson = tojson(shardKey);
    let splitPoints = Object.values(shardKey)[0] == "hashed"
        ? [{a: convertShardKeyToHashed(0)}, {a: convertShardKeyToHashed('b')}]
        : [{a: 0}, {a: 'b'}];
    suspendRangeDeletionShard0 = configureFailPoint(st.shard0, 'suspendRangeDeletion');
    setupCollection({shardKey: {a: 1}, splits: splitPoints});

    section(`Find *without* collation on collection sharded on ${shardKeyJson}`);
    doQueries(false);

    // Since the query has non-simple collation we will have to broadcast to all shards (since
    // the shard key is on a simple collation index), and should have a SHARD_FILTER stage.
    section(`Find with collation on collection sharded on ${shardKeyJson}`);
    doQueries(true);

    suspendRangeDeletionShard0.off();
    coll.drop();
}
st.stop();
