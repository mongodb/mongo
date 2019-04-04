//
// Tests merizos-only query behavior
//

var st = new ShardingTest({shards: 1, merizos: 1, verbose: 0});

var merizos = st.s0;
var coll = merizos.getCollection("foo.bar");

//
//
// Ensure we can't use exhaust option through merizos
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
// Ensure we can't trick merizos by inserting exhaust option on a command through merizos
coll.remove({});
assert.writeOK(coll.insert({a: 'b'}));
var cmdColl = merizos.getCollection(coll.getDB().toString() + ".$cmd");
var cmdQuery = cmdColl.find({ping: 1}).limit(1);
assert.commandWorked(cmdQuery.next());
cmdQuery = cmdColl.find({ping: 1}).limit(1).addOption(DBQuery.Option.exhaust);
assert.throws(function() {
    assert.commandWorked(cmdQuery.next());
});

jsTest.log("DONE!");

st.stop();
