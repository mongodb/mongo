(function() {
"use strict";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        shardOptions: {setParameter: {featureFlagCommonQueryFramework: true}},
        mongosOptions: {setParameter: {featureFlagCommonQueryFramework: true}}
    }
});

const db = st.getDB("test");
const coll = db.analyze_coll;
coll.drop();

assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard1.shardName);
assert.commandWorked(
    st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}));

assert.commandWorked(coll.insert({a: 1, b: 2}));

let res = db.runCommand({analyze: coll.getName()});
assert.commandFailedWithCode(res, ErrorCodes.NotImplemented);

res = db.runCommand({analyze: coll.getName(), apiVersion: "1", apiStrict: true});
assert.commandFailedWithCode(res, ErrorCodes.APIStrictError);

res = db.runCommand({analyze: coll.getName(), writeConcern: {w: 1}});
assert.commandFailedWithCode(res, ErrorCodes.NotImplemented);

st.stop();
})();
