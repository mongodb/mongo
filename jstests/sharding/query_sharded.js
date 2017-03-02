//
// Tests bongos-only query behavior
//

var st = new ShardingTest({shards: 1, bongos: 1, verbose: 0});

var bongos = st.s0;
var coll = bongos.getCollection("foo.bar");

//
//
// Ensure we can't use exhaust option through bongos
coll.remove({});
assert.writeOK(coll.insert({a: 'b'}));
var query = coll.find({});
assert.neq(null, query.next());
query = coll.find({}).addOption(DBQuery.Option.exhaust);
assert.throws(function() {
    query.next();
});

//
//
// Ensure we can't trick bongos by inserting exhaust option on a command through bongos
coll.remove({});
assert.writeOK(coll.insert({a: 'b'}));
var cmdColl = bongos.getCollection(coll.getDB().toString() + ".$cmd");
var cmdQuery = cmdColl.find({ping: 1}).limit(1);
assert.commandWorked(cmdQuery.next());
cmdQuery = cmdColl.find({ping: 1}).limit(1).addOption(DBQuery.Option.exhaust);
assert.throws(function() {
    cmdQuery.next();
});

jsTest.log("DONE!");

st.stop();
