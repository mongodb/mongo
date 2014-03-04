// This checks to make sure that the query is untargetted when using a regex
var st = new ShardingTest({ shards: 3});

st.adminCommand({ enablesharding: "test" });
st.adminCommand({ shardcollection: "test.server11430", key: { "path" : "hashed" } });

var col = st.s.getDB('test').getCollection('server11430');

var doc1 = { path: "thisisastring", val: true }
var doc2 = { path: "thisisabigString", val: true }

col.insert([doc1, doc2])
printjson(col.find({ path : /isa/ }).explain());
var res  = col.update({ path : /isa/ }, { $set: { val: false }}, { multi: true });
var result  = col.findOne();

assert.eq(false, result.val);
assert.eq(2, res.nModified);

st.stop();
