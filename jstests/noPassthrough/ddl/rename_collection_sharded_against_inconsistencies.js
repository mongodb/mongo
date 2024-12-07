// Tests the expected behaviour of renameCollection against catalog inconsistencies caused by direct
// writes to shards (which need a noPassthrough environment to be performed).

import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({});

const dbName = 'test';
const primaryShardName = st.shard0.shardName;
const directConnToNonPrimaryShard = st.shard1;

assert.commandWorked(st.s0.adminCommand({enablesharding: dbName, primaryShard: primaryShardName}));

// Create collection on non-primary shard (shard1 for test db) to simulate wrong creation via
// direct connection: collection rename should fail since `badcollection` uuids are inconsistent
// across shards
jsTest.log("Testing uuid consistency across shards");
assert.commandWorked(directConnToNonPrimaryShard.getDB(dbName).badcollection.insert({_id: 1}));
assert.commandWorked(st.s0.getDB(dbName).badcollection.insert({_id: 1}));
assert.commandFailedWithCode(
    st.s0.getDB(dbName).badcollection.renameCollection('goodcollection'),
    [ErrorCodes.InvalidUUID],
    "collection rename should fail since test.badcollection uuids are inconsistent across shards");
directConnToNonPrimaryShard.getDB(dbName).badcollection.drop();

// Target collection existing on non-primary shard: rename with `dropTarget=false` must fail
jsTest.log("Testing rename behavior when target collection [wrongly] exists on non-primary shards");
assert.commandWorked(directConnToNonPrimaryShard.getDB(dbName).superbadcollection.insert({_id: 1}));
assert.commandWorked(st.s0.getDB(dbName).goodcollection.insert({_id: 1}));
assert.commandFailedWithCode(
    st.s0.getDB(dbName).goodcollection.renameCollection('superbadcollection', false),
    [ErrorCodes.NamespaceExists],
    "Collection rename with `dropTarget=false` must have failed because target collection exists on a non-primary shard");
// Target collection existing on non-primary shard: rename with `dropTarget=true` must succeed
assert.commandWorked(
    st.s0.getDB(dbName).goodcollection.renameCollection('superbadcollection', true));

st.stop();
