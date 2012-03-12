// Create {v:0} index on primary. Add new secondary. Make sure same index on secondary is {v:1} - SERVER-3852

var rs = new ReplSetTest( {name: 'rs', nodes: 1, host: 'localhost'} );
rs.startSet();
rs.initiate();
var r1 = rs.getMaster();
var db1 = r1.getDB('test');

var t = '';
for (var i = 0; i < 1000; i++) t += 'a';
for (var i = 0; i < 10000; i++) db1.foo.insert( {_id:i, x:i%1000, t:t} );
db1.foo.createIndex( {x:1}, {v: 0} );

var r2 = rs.add();
rs.reInitiate();
rs.awaitSecondaryNodes();
var db2 = r2.getDB('test');
r2.setSlaveOk();

var idx = db2.system.indexes.findOne( {key: {x:1}} );
assert.eq (idx.v, 1, 'expected all indexes generated on Mongo version >= 2.0 to be {v:1}. See SERVER-3852');

// add another new node, make sure ports _aren't_ closed SERVER-4315
r1 = rs.getMaster();
rs.add();
var c = r1.getDB("local").system.replset.findOne();
var config = rs.getReplSetConfig();
config.version = c.version+1;
var result = r1.getDB("admin").runCommand({replSetReconfig:config});
assert.eq(result.ok, 1);

rs.stopSet(15);
