/**
 * Tests the mongos downconversion, specifically the resetError logic when it enforces
 * the write concern.
 */

var st = new ShardingTest({ shards: 1, other: { shardOptions: { binVersion: '2.4' }}});
st.stopBalancer();

var conn = st.s;
conn.forceWriteMode('commands');
db = conn.getDB('test');
load('./jstests/core/bulk_legacy_enforce_gle.js');

st.stop();
