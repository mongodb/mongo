// Tests that authoritative metadata recovery works as expected on a very basic test.
//
// TODO SERVER-122509: We can remove this once we've comprehensively made more DDLs authoritative.
// This is just a sanity check for now.
//
// @tags: [
//   featureFlagShardAuthoritativeCollMetadata
// ]

import {ShardingTest} from "jstests/libs/shardingtest.js";

const DOC = {x: 1};

let st = new ShardingTest({shards: 1});
let s = st.s;
let testDb = st.getDB("test");
assert.commandWorked(s.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName}));
assert.commandWorked(s.adminCommand({shardCollection: "test.test", key: {_id: 1}}));
testDb.test.insertOne(DOC);

// At this point the collection is potentially non-authoritative on the shard. Let's copy the contents
// of the global catalog to the collection and force the CSR to become authoritative pending a recovery.
const colls = st.config.collections.find({}).toArray();
const chunks = st.config.chunks.find({}).toArray();

const shardPrimary = st.rs0.getPrimary();
const shardConfigDb = shardPrimary.getDB("config");
shardConfigDb["shard.catalog.collections"].insertMany(colls);
shardConfigDb["shard.catalog.chunks"].insertMany(chunks);

// At this point the collection is not marked as authoritative, force a clear and set the internal flag
// to authoritative in order to force authoritative recovery.
assert.commandWorked(
    shardPrimary.adminCommand({
        _internalClearCollectionShardingMetadata: "test.test",
        isAuthoritative: true,
    }),
);

const result = testDb.test.findOne({}, {_id: 0});
assert.docEq(result, DOC);

st.stop();
