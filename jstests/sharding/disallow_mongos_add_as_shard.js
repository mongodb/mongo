(function() {

var st = new ShardingTest({ name: "add_shard3", shards: 1 });

var result = st.admin.runCommand({addshard: st.s.host});

printjson(result);

assert.eq(result.ok, 0, "don't add mongos as a shard");

})();
