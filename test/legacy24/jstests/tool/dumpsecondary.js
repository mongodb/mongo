var replTest = new ReplSetTest( {name: 'testSet', nodes: 2} );

var nodes = replTest.startSet();
replTest.initiate();

var master = replTest.getMaster();
db = master.getDB("foo")
db.foo.save({a: 1000});
replTest.awaitReplication();
replTest.awaitSecondaryNodes();

assert.eq( 1 , db.foo.count() , "setup" );

var slaves = replTest.liveNodes.slaves;
assert( slaves.length == 1, "Expected 1 slave but length was " + slaves.length );
slave = slaves[0];

var args = ['mongodump', '-h', slave.host, '--out', '/data/db/jstests_tool_dumpsecondary_external/'];
var authargs = ['--username', jsTest.options().authUser, '--password', jsTest.options().authPassword];
if (jsTest.options().keyFile) {
    args = args.concat(authargs);
}
runMongoProgram.apply(null, args);
db.foo.drop()

assert.eq( 0 , db.foo.count() , "after drop" );
args = ['mongorestore', '-h', master.host, '/data/db/jstests_tool_dumpsecondary_external/'];
if (jsTest.options().keyFile) {
    args = args.concat(authargs);
}
runMongoProgram.apply(null, args)
assert.soon( "db.foo.findOne()" , "no data after sleep" );
assert.eq( 1 , db.foo.count() , "after restore" );
assert.eq( 1000 , db.foo.findOne().a , "after restore 2" );

resetDbpath('/data/db/jstests_tool_dumpsecondary_external')

replTest.stopSet(15)
