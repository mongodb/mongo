/**
 * Test to make sure that you can addShard with only a hidden member specified in the
 * connection string.
 */

var replTest = new ReplSetTest({ nodes: 2 });
replTest.startSet();

var config = replTest.getReplSetConfig();
config.members[1].hidden = true;
config.members[1].priority = 0;
var hiddenHost = config.members[1].host;
replTest.initiate(config);

var st = new ShardingTest({ shards: [{ useHostName: true }] });
var mongos = st.s0;
var connString = replTest.name + "/" + hiddenHost;
res = st.s.adminCommand({ addShard: connString });
assert.eq(res.ok, 1);

st.stop();

