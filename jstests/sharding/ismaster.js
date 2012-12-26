var st = new ShardingTest({shards:1, mongos:1});
var res = st.s0.getDB("admin").runCommand("ismaster");
assert( res.maxBsonObjectSize &&
        isNumber(res.maxBsonObjectSize) &&
        res.maxBsonObjectSize > 0, "maxBsonObjectSize possibly missing:" + tojson(res))
assert( res.maxMessageSizeBytes &&
        isNumber(res.maxMessageSizeBytes) &&
        res.maxBsonObjectSize > 0, "maxMessageSizeBytes possibly missing:" + tojson(res))
assert(res.ismaster, "ismaster missing or false:" + tojson(res))
assert(res.localTime, "localTime possibly missing:" + tojson(res))
assert(res.msg && res.msg == "isdbgrid", "msg possibly missing or wrong:" + tojson(res))
