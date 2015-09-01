(function() {

var st = new ShardingTest("add_shard3", 1);

var result = st.admin.runCommand({addshard: st.s.host});

printjson(result);

assert.eq(result.ok, 0, "don't add mongos as a shard");

})();
