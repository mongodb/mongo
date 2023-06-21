/**
 * This test sets getLastErrorDefaults to w:2 to test that migrations do not use this default with a
 * session checked out.
 *
 * @tags: [requires_fcv_44]
 */
(function() {
'use strict';

let getLastErrorDefaultsSettings = {getLastErrorDefaults: {w: 2, wtimeout: 0}};
const st = new ShardingTest({
    shards: {
        rs0: {nodes: 3, settings: getLastErrorDefaultsSettings},
        rs1: {nodes: 3, settings: getLastErrorDefaultsSettings}
    },
    configReplSetTestOptions: {settings: getLastErrorDefaultsSettings}
});

const dbName = 'test';
const collName = 'foo';
const nss = dbName + '.' + collName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {x: 1}}));

assert.commandWorked(st.s.adminCommand({split: nss, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: nss, find: {x: 0}, to: st.shard1.shardName}));

st.stop();
})();
