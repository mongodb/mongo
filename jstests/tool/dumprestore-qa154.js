/**
 * https://jira.mongodb.org/browse/QA-154
 */

var myTestName = "dumprestore-qa154";
var keyFile = "jstests/tool/" + myTestName + ".key";
run( "chmod" , "600" , keyFile );

var s = new ShardingTest({
    name: myTestName
  , keyFile: keyFile
  , nopreallocj: true
  , shards: 2
  , mongos: 1
})

var mongos = new Mongo( "localhost:" + s.s0.port )
var coll = mongos.getCollection( "test.foo" )
coll.remove();

s.shardColl(coll, { _id : 1 })
coll.insert({ x: 1 })

var x = runMongoProgram( "mongodump", "--host", "127.0.0.1:" + s.s0.port);
assert.eq(x, 0, "mongodump with no options on sharded cluster failed");
print("dumped", x);

var x = runMongoProgram( "mongorestore", "--host", "127.0.0.1:" + s.s0.port);
assert.gt(x, 0, "expected mongorestore with no options on sharded cluster to fail");
print("restored w/ no opts", x);

var x = runMongoProgram( "mongorestore", "--host", "127.0.0.1:" + s.s0.port, "-d", "test", "--dbpath", "dump/test");
assert.eq(x, 0, "mongorestore with specified db on sharded cluster failed");
print("restored w/ opts", x);

var mongos = new Mongo( "localhost:" + s.s0.port )
var coll = mongos.getCollection( "test.foo" )
var count = coll.count()
print("test.count:", count);
assert.eq(1, count);

s.stop();
