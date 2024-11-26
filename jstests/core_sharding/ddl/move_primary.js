/*
 * Tests basic movePrimary behaviour.
 */

import {getNumShards, getRandomShardId} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

// TODO SERVER-96953 Remove the if clause and add a 'requires_2_or_more_shards' exclusion tag.
if (getNumShards(db) < 2) {
    jsTestLog("This test requires at least two shards.");
    quit();
}

const testDB = db.getSiblingDB('test_db');
testDB.dropDatabase();

const coll = testDB['coll'];

const N = 250;

function doInserts(n) {
    jsTestLog("Inserting " + n + " entries.");

    let docs = [];
    for (let i = -(n - 1) / 2; i < n / 2; i++) {
        docs.push({x: i});
    }
    coll.insertMany(docs);
}

assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));

let initPrimaryShard = testDB.getDatabasePrimaryShardId();

doInserts(N);
assert.eq(N, coll.countDocuments({}));

let otherShard = getRandomShardId(db, /* exclude = */ initPrimaryShard);

jsTestLog("Move primary to another shard and check content.");
assert.commandWorked(testDB.adminCommand({movePrimary: testDB.getName(), to: otherShard}));
doInserts(N);
assert.eq(2 * N, coll.countDocuments({}));
assert.eq(otherShard, testDB.getDatabasePrimaryShardId());

jsTestLog("Move primary to the original shard and check content.");
assert.commandWorked(testDB.adminCommand({movePrimary: testDB.getName(), to: initPrimaryShard}));
doInserts(N);
assert.eq(3 * N, coll.countDocuments({}));
assert.eq(initPrimaryShard, testDB.getDatabasePrimaryShardId());
