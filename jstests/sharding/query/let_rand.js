/**
 * Test that $rand in a command-level 'let' is evaluated only once.
 *
 * @tags: [
 *   # The bulkWrite command is not enabled on versions below 8.0.
 *   requires_fcv_80,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";

const st = new ShardingTest({shards: 2});
const db = st.s.getDB(dbName);

const coll = db.coll;
coll.drop();
assert.commandWorked(coll.insert([{x: -2}, {x: 2}]));
assert.commandWorked(coll.createIndex({x: 1}));

const viewName = 'view';
assert.commandWorked(db.createView(viewName, coll.getName(), []));
const view = db.getCollection(viewName);

// Split at zero so each shard gets one document.
st.shardColl(coll, {x: 1}, {x: 0}, {x: 1});

// Run the command and assert that both documents have the same value of 'rand'.
function test(cmd) {
    let result = assert.commandWorked(db.runCommand(cmd));
    let batch = result.cursor.firstBatch;
    assert.eq(batch.length, 2, result);
    const rand0 = batch[0].rand;
    const rand1 = batch[1].rand;
    assert.eq('number', typeof rand0, result);
    assert.eq('number', typeof rand1, result);

    // Assert that both shards see the same pseudo-random value.
    assert.eq(rand0, rand1, result);
}

test({
    find: coll.getName(),
    projection: {_id: 0, rand: "$$randVal"},
    let : {randVal: {$rand: {}}},
});
test({
    aggregate: coll.getName(),
    pipeline: [{$project: {_id: 0, rand: "$$randVal"}}],
    let : {randVal: {$rand: {}}},
    cursor: {}
});
test({
    find: view.getName(),
    projection: {_id: 0, rand: "$$randVal"},
    let : {randVal: {$rand: {}}},
});
test({
    aggregate: view.getName(),
    pipeline: [{$project: {_id: 0, rand: "$$randVal"}}],
    let : {randVal: {$rand: {}}},
    cursor: {}
});

// Test update command.
//
// Pick a random value and update all documents with it.
// Then we expect all documents to have the same value.
assert.commandWorked(db.runCommand({
    update: coll.getName(),
    updates: [{
        q: {},
        u: [{$set: {rand: "$$a"}}],
        multi: true,
    }],
    let : {a: {$rand: {}}},
}));
test({
    find: coll.getName(),
    projection: {rand: 1},
});

// Undo changes.
assert.commandWorked(coll.updateMany({}, {$unset: {rand: 1}}));

// Test the 'bulkWrite' command.
if (FeatureFlagUtil.isEnabled(db, 'BulkWriteCommand')) {
    assert.commandWorked(db.adminCommand('bulkWrite', {
        nsInfo: [{ns: coll.getFullName()}],
        ops: [
            {
                update: 0,
                filter: {},
                multi: true,
                updateMods: [{$set: {rand: "$$a"}}],
            },
        ],
        let : {a: {$rand: {}}},
    }));
    test({
        find: coll.getName(),
        projection: {rand: 1},
    });

    // Undo changes.
    assert.commandWorked(coll.updateMany({}, {$unset: {rand: 1}}));
}

// Test the 'findAndModify' command.
//
// Randomly choose an 'x' value to update, and update it.
// If we correctly evaluate $rand once up front, then we'll always have exactly one update.
for (let trial = 0; trial < 100; ++trial) {
    // Pick either {x: 2} or {x: -2} randomly, and update it.
    assert.commandWorked(db.runCommand({
        findAndModify: coll.getName(),
        query: {$expr: {$eq: ["$x", "$$r"]}},
        update: {$inc: {hit: 1}},
        let : {
            r: {
                $cond: {
                    if: {$lt: [{$rand: {}}, 0.5]},
                    then: -2,
                    else: +2,
                }
            }
        },
    }));
    // If we incorrectly evaluated $rand independently per shard, then it's possible for each
    // shard to pick the value it does not contain, resulting in no updates. If we correctly
    // evaluate $rand once, we expect exactly one update.
    const docs = coll.find().toArray();
    assert.eq(docs.length, 2);
    assert.eq(docs.filter(doc => doc.hit).length, 1, 'Expected exactly one hit: ' + tojson(docs));

    // Reset for next trial.
    assert.commandWorked(coll.updateMany({}, {$unset: {hit: 1}}));
}

// Test the 'delete' command.
//
// Put the values 0..99 on each shard.
// Pick 0..9 randomly and deleteMany that value.
// If we incorrectly evaluate per shard, we will very likely remove a different value.

// Put the values 0..99 on each shard.
const N = 100;
assert.commandWorked(coll.deleteMany({}));
assert.commandWorked(coll.insert(Array.from({length: N}, (_, i) => ({x: -2, i}))));
assert.commandWorked(coll.insert(Array.from({length: N}, (_, i) => ({x: +2, i}))));
// Randomly choose 0..99 to delete.
// If we correctly evaluate $rand once, we'll delete the same value on each shard.
// If we incorrectly evaluate per shard, we're 99% likely to delete two different values.
assert.commandWorked(db.runCommand({
    delete: coll.getName(),
    deletes: [
        {
            q: {$expr: {$eq: ["$i", "$$r"]}},
            limit: 0,  // no limit: delete all matching documents.
        },
    ],
    let : {r: {$floor: {$multiply: [N, {$rand: {}}]}}},
}));

const groups = coll.aggregate([
                       {$group: {_id: "$x", nums: {$push: "$i"}}},
                       {$sort: {_id: 1}},
                   ])
                   .toArray();
groups[0].nums.sort();
groups[1].nums.sort();

const originalSum = Array.sum(Array.from({length: N}, (_, i) => i));
const deletedLeft = originalSum - Array.sum(groups[0].nums);
const deletedRight = originalSum - Array.sum(groups[1].nums);
assert.eq(deletedLeft, deletedRight, 'Deleted a different value on each shard');

st.stop();
