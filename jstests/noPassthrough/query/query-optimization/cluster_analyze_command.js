import {ShardingTest} from "jstests/libs/shardingtest.js";

// Keep migrated documents on the donor so analyze must not count them.
TestData.skipCheckOrphans = true;
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {nodes: 1, setParameter: {disableResumableRangeDeleter: true}},
});

const db = st.getDB("test");
const coll = db.analyze_coll;
coll.drop();

assert.commandWorked(
    st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard1.shardName}),
);
assert.commandWorked(
    st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}),
);

assert.commandWorked(coll.insert({a: 1, b: 2}));

let res = db.runCommand({analyze: coll.getName(), mode: "histograms", key: "a"});
assert.commandWorked(res);

res = db.runCommand({
    analyze: coll.getName(),
    mode: "histograms",
    key: "a",
    apiVersion: "1",
    apiStrict: true,
});
assert.commandFailedWithCode(res, ErrorCodes.APIStrictError);

res = db.runCommand({
    analyze: coll.getName(),
    mode: "histograms",
    key: "a",
    writeConcern: {w: 1},
});
assert.commandWorked(res);

// Orphans left on the donor must not be counted by either shard.
const orphans = db.analyze_orphans;
assert.commandWorked(st.s.adminCommand({shardCollection: orphans.getFullName(), key: {_id: 1}}));
assert.commandWorked(orphans.insertMany(Array.from({length: 20}, (_, i) => ({_id: i, a: i}))));
assert.commandWorked(st.s.adminCommand({split: orphans.getFullName(), middle: {_id: 10}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: orphans.getFullName(), find: {_id: 10}, to: st.shard0.shardName}),
);
assert.commandWorked(db.runCommand({analyze: orphans.getName(), mode: "histograms", key: "a"}));
let observed = 0;
for (const shard of [st.shard0, st.shard1]) {
    observed += shard.getDB("test").system.statistics.analyze_orphans.findOne({_id: "a"})
        .statistics.documents;
}
assert.eq(20, observed);

st.stop();
