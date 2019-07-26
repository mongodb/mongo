// Tests that dropping a database also removes the zones associated with the
// collections in the database.
(function() {
var st = new ShardingTest({shards: 1});
var configDB = st.s.getDB('config');
var shardName = configDB.shards.findOne()._id;

assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: 'x'}));
assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: 'test.user', min: {x: 0}, max: {x: 10}, zone: 'x'}));

var tagDoc = configDB.tags.findOne();
assert.eq(1, configDB.tags.find().length());
assert.eq('test.user', tagDoc.ns);
assert.eq({x: 0}, tagDoc.min);
assert.eq({x: 10}, tagDoc.max);
assert.eq('x', tagDoc.tag);

var db = st.s.getDB("test");
db.dropDatabase();

assert.eq(null, configDB.tags.findOne());
assert.commandWorked(st.s.adminCommand({removeShardFromZone: shardName, zone: 'x'}));
assert.commandWorked(st.removeRangeFromZone('test.user', {x: 0}, {x: 10}));

st.stop();
})();
