//
// Make sure that a mixed-version replset correctly replicates a document
// with an over-size index key from a 2.4 master to a 2.5.5+ secondary
//
load( './jstests/multiVersion/libs/multi_rs.js' )

var oldVersion = "2.4"
var newVersion = "latest"

var nodes = { n1 : { binVersion : oldVersion }, n2 : { binVersion : newVersion } }

var host = getHostName();
var name = "test";

var replTest = new ReplSetTest( { name : name, nodes : nodes } );

var nodes = replTest.startSet();
var port = replTest.ports;
replTest.initiate();

replTest.awaitReplication();

var primary = replTest.getPrimary();
var secondary = replTest.getSecondary();

var coll = primary.getCollection("foo.bar");
var largeKey = new Array(2024).toString();
coll.ensureIndex({ a : 1 });
coll.insert({ a : largeKey });
assert.eq(1, coll.count());

replTest.awaitReplication();

// make sure the document exists, but the index entry does not
var secondaryColl = secondary.getCollection("foo.bar");
assert.eq(1, secondaryColl.count());
printjson(secondaryColl.validate().keysPerIndex);
assert.eq(0, secondaryColl.find().hint('a_1').itcount());

jsTest.log("DONE!");
replTest.stopSet();
