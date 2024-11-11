/**
 * Tests for capped collection functionality of the move collection feature.
 *
 * @tags: [
 *  requires_fcv_80,
 *  requires_collstats,
 *  featureFlagMoveCollection,
 *  assumes_balancer_off,
 *  requires_capped,
 *  # Stepdown test coverage is already provided by the resharding FSM suites.
 *  does_not_support_stepdowns,
 * ]
 */

import {getPrimaryShardNameForDB, getShardNames} from "jstests/sharding/libs/sharding_util.js";

function verifyOrderMatches() {
    let postReshardingOrder = coll.find({}).toArray();
    assert.eq(preReshardingOrder.length, postReshardingOrder.length);
    for (let i = 0; i < preReshardingOrder.length; i++) {
        assert.eq(preReshardingOrder[i], postReshardingOrder[i]);
    }
}

const shardNames = getShardNames(db);
if (shardNames.length < 2) {
    jsTestLog("This test requires at least two shards.");
    quit();
}

const collName = jsTestName();
const dbName = db.getName();
const ns = dbName + '.' + collName;

let shard0 = shardNames[0];
let shard1 = shardNames[1];

jsTestLog("Setting up the capped collection.");
assert.commandWorked(db.createCollection(collName, {capped: true, size: 1048576}));

const coll = db.getCollection(collName);
const stats = coll.stats();
assert(stats.sharded != true);

// Insert more than one document to it. This tests that capped collections can clone multiple docs.
const numDocs = 1000;
var bulk = coll.initializeUnorderedBulkOp();
for (var i = 0; i < numDocs; i++) {
    bulk.insert({x: i});
}
assert.commandWorked(bulk.execute());
assert.eq(coll.find({}).itcount(), numDocs);
const preReshardingOrder = coll.find({}).toArray();

const configDb = db.getSiblingDB('config');
const primaryShard = getPrimaryShardNameForDB(db);
const nonPrimaryShard = (shard0 == primaryShard) ? shard1 : shard0;

jsTestLog("Move to non-primary shard (" + nonPrimaryShard + ")");
assert.commandWorked(db.adminCommand({moveCollection: ns, toShard: nonPrimaryShard}));

let collEntry = configDb.collections.findOne({_id: ns});
assert.eq(collEntry._id, ns);
assert.eq(collEntry.unsplittable, true);
assert.eq(collEntry.key, {_id: 1});
assert.eq(coll.find({}).itcount(), numDocs);

verifyOrderMatches();  // Order matches after resharding.

jsTestLog("Move to primary shard (" + primaryShard + ")");
assert.commandWorked(db.adminCommand({moveCollection: ns, toShard: primaryShard}));
assert.eq(coll.find({}).itcount(), numDocs);
verifyOrderMatches();  // Order matches after resharding.

coll.drop();
