/*
 * Tests basic movePrimary behavior.
 *
 * @tags: [
 *  # movePrimary command is not allowed in clusters with a single shard.
 *  requires_2_or_more_shards,
 *  # movePrimary will fail if the destination shard steps down while cloning data.
 *  does_not_support_stepdowns,
 * ]
 */

import {getRandomShardName} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

// Create a normal collection
const coll = db['coll'];

const N = 250;

function doInserts(n) {
    jsTestLog("Inserting " + n + " entries.");

    let docs = [];
    for (let i = -(n - 1) / 2; i < n / 2; i++) {
        docs.push({x: i});
    }
    coll.insertMany(docs);
}

assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));

let initPrimaryShard = db.getDatabasePrimaryShardId();

// Create normal collection
doInserts(N);
assert.eq(N, coll.countDocuments({}));

// Create view
const viewDataColl = db['view_data_coll'];
assert.commandWorked(viewDataColl.insertMany([{a: 1}, {a: 2}, {a: 3}]));
const view = db['view'];
assert.commandWorked(db.runCommand(
    {create: view.getName(), viewOn: viewDataColl.getName(), pipeline: [{$match: {a: 3}}]}));
assert.eq(1, view.countDocuments({}));

let otherShard = getRandomShardName(db, /* exclude = */ initPrimaryShard);

jsTestLog("Move primary to another shard and check content.");
assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: otherShard}));
assert.eq(otherShard, db.getDatabasePrimaryShardId());
assert.eq(1, view.countDocuments({}));
doInserts(N);
assert.eq(2 * N, coll.countDocuments({}));

jsTestLog("Move primary to the original shard and check content.");
assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: initPrimaryShard}));
assert.eq(initPrimaryShard, db.getDatabasePrimaryShardId());
assert.eq(1, view.countDocuments({}));
doInserts(N);
assert.eq(3 * N, coll.countDocuments({}));
