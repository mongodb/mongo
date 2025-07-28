/**
 * Tests for the absence/presence of a shard filtering stage for queries which may/may not
 * be single shard targeted.
 * TODO SERVER-94611: Extend testing once shard key may have non-simple collation.
 */
import {getWinningPlan, planHasStage} from 'jstests/libs/analyze_plan.js';
import {configureFailPoint} from 'jstests/libs/fail_point_util.js';

const st = new ShardingTest({shards: 2});

const db = st.getDB("test");
const coll = db[jsTestName()];

const setupCollection = ({shardKey, splits}) => {
    coll.drop();

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

    let doc = {_id: "a", a: "a"};

    assert.commandWorked(db.adminCommand(
        {moveChunk: coll.getFullName(), find: {_id: MinKey, a: MinKey}, to: st.shard0.shardName}));
    assert.commandWorked(
        db.adminCommand({moveChunk: coll.getFullName(), find: doc, to: st.shard0.shardName}));
    assert.commandWorked(db.adminCommand(
        {moveChunk: coll.getFullName(), find: {_id: MaxKey, a: MaxKey}, to: st.shard1.shardName}));

    // Put data on shard0, that will go into chunk 2.
    assert.commandWorked(coll.insert(doc));

    // Perform a chunk migration of chunk 2 from shard0 to shard1, but do not clean
    // up orphans on shard0 (see suspendRangeDeletionShard0 failpoint)
    db.adminCommand({moveChunk: coll.getFullName(), find: doc, to: st.shard1.shardName});
};

/**
 * Verify that if a SHARD_MERGE is required, each shard includes a SHARD_FILTER
 * in their plan.
 * (Note that !SHARD_MERGE && SHARD_FILTER is a pessimisation but not a functional issue).
 *
 * Accepts a DBQuery object - e.g.,
 *  assertShardMergeImpliesShardFilter(db.coll.find({foo:"bar"}))
 */
function assertShardMergeImpliesShardFilter(queryObj, testInfo) {
    const explain = queryObj.explain();
    const winningPlan = getWinningPlan(explain.queryPlanner);

    const shardMerge = planHasStage(db, winningPlan, "SHARD_MERGE");

    if (!shardMerge) {
        return;
    }

    const express = winningPlan.shards.every((shard) => {
        return planHasStage(db, getWinningPlan(shard), "EXPRESS_IXSCAN");
    });

    if (express) {
        // Lookup by a specific _id.
        // TODO: SERVER-98300 - explain for express path doesn't indicate if shard
        // filtering will be applied, so it can't be asserted here.
        // The document count assertions will still be applied, and would fail if
        // duplicate results were returned due to missing shard filtering + orphans.
        return;
    }

    const shardFiltered = winningPlan.shards.every((shard) => {
        return planHasStage(db, getWinningPlan(shard), "SHARDING_FILTER");
    });

    assert(shardFiltered,
           {msg: "SHARD_MERGE but missing SHARDING_FILTER", explain: explain, testInfo: testInfo});
}

const caseInsensitive = {
    locale: 'en_US',
    strength: 2
};

const doQueries = (shardKey, collation) => {
    let evalFn = (query, expectedCount = 1) => {
        // Information about the current combination of parameters under test,
        // to log in the event of a failure.
        const testInfo = {query: query, collation: collation, shardKey: shardKey};
        let queryObj = coll.find(query);
        if (collation !== undefined) {
            queryObj = queryObj.collation(collation);
        }
        assert.eq(expectedCount, queryObj.toArray().length, testInfo);
        assertShardMergeImpliesShardFilter(queryObj, testInfo);
    };
    for (let fieldNames of [["a"], ["_id"], ["a", "_id"]]) {
        // Helper to compose an object like:
        //  {a: <value>}, {_id:<value>}, {a:<value>, _id:<value>}
        // using the above field names.
        let query = value => Object.fromEntries(fieldNames.map(name => [name, value]));
        // Equality.
        evalFn(query("a"));
        // A document was not inserted with {a:1} or {_id:1} (or both), no docs should match.
        evalFn(query(1), /*expectedCount */ 0);
        // Ranges within a single chunk.
        evalFn(query({"$lte": "a"}));
        evalFn(query({"$gte": "a"}));
        evalFn(query({"$gte": "a", "$lte": "a"}));
    }
};

let suspendRangeDeletionShard0;

function getSplitPoints(shardKey) {
    // We wish to isolate a document in a particular chunk, by splitting at 0, and 'b'.
    // However, we also have wider testing of hashed and compound shard keys.
    let results = [];
    for (const splitPoint of [0, 'b']) {
        let split = {};
        for (const [key, value] of Object.entries(shardKey)) {
            split[key] = value == "hashed" ? convertShardKeyToHashed(splitPoint) : splitPoint;
        }
        results.push(split);
    }
    return results;
}
const shardKeys = [
    {a: 1},
    {_id: 1},
    {a: 1, _id: 1},
    {a: "hashed"},
    {_id: "hashed"},
    {a: "hashed", _id: 1},
    {a: 1, _id: "hashed"}
];
for (let shardKey of shardKeys) {
    let splitPoints = getSplitPoints(shardKey);
    suspendRangeDeletionShard0 = configureFailPoint(st.shard0, 'suspendRangeDeletion');
    setupCollection({shardKey: shardKey, splits: splitPoints});

    // Queries without collation.
    doQueries(shardKey);

    // Queries WITH collation.
    // Since the query has non-simple collation we will have to broadcast to all shards (since
    // the shard key is on a simple collation index), and should have a SHARD_FILTER stage.
    doQueries(shardKey, caseInsensitive);

    suspendRangeDeletionShard0.off();
    coll.drop();
}
st.stop();
