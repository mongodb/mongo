jsTest.log("Starting sharded cluster for wrong duplicate error setup");

s = new ShardingTest( name="writeback_server7958", shards = 2, verbose=0, mongos = 4 );

var mongosA=s.s0;
var mongosB=s.s1;
var mongosC=s.s2;
var mongosD=s.s3;

ns1 = "test.trans";
ns2 = "test.node";

adminSA = mongosA.getDB( "admin" );
adminSB = mongosB.getDB( "admin" );
adminSD = mongosD.getDB( "admin" );
adminSA.runCommand({ enableSharding : "test"});
adminSA.runCommand({ shardCollection : ns1, key : { owner : 1 }, unique: true });
//adminSA.runCommand({ shardCollection : ns1, key : { owner : 1 } });

try {
   s.stopBalancer();
} catch (e) {
   print("coundn't stop balancer via command");
}

adminSA.settings.update({ _id: 'balancer' }, { $set: { stopped: true }});

var db = mongosA.getDB( "test" );
var dbB = mongosB.getDB( "test" );
var dbC = mongosC.getDB( "test" );
var dbD = mongosD.getDB( "test" );
var trans = db.trans;
var node = db.node;
var transB = dbB.trans;
var nodeB = dbB.node;
var transC = dbC.trans;
var nodeC = dbC.node;
var transD = dbD.trans;
var nodeD = dbD.node;

var primary = s.getServerName("test");
var shard1 = s._shardNames[0];
var shard2 = s._shardNames[1];
if (primary == shard1) {
    other = shard2;
} else {
    other = shard1;
}


trans.insert({"owner":NumberLong("1234567890"),"tid":"2c4ba280-450a-11e2-bcfd-0800200c9a66"});
db.runCommand({getLastError:1, j:1});

node.insert({"owner":NumberLong("1234567890"),"parent":NumberLong("0"),_id:NumberLong("1234567890"), "counts":0});
db.runCommand({getLastError:1, j:1});
for (var i=0; i<1000; i++) {
   trans.insert({"owner":NumberLong(i),"tid":"2c4ba280-450a-11e2-bcfd-0800200c9a66"});
   node.insert({"owner":NumberLong(i),"parent":NumberLong(i+1000),_id:NumberLong(i+1234567890), "counts":0});
}

transB.insert({"owner":NumberLong("1234567890"),"tid":"2c4ba280-450a-11e2-bcfd-0800200c9a66"});
var r1=dbB.runCommand( { getLastError: 1, w: 1 } );
assert( r1.n == 0 && r1.err.length > 0 && r1.hasOwnProperty("code"), tojson( r1 ) );

jsTest.log("Inserted dup (failed), now split chunks and move data");

adminSD.runCommand( { split: ns1, middle : { owner : 100} });
adminSD.runCommand( { movechunk: ns1, find : { owner : 105}, to: other});

jsTest.log("Kicking off dup inserts and updates");

errors=[];
i=0;
trans.insert({"owner":NumberLong("1234567890"),"tid":"2c4ba280-450a-11e2-bcfd-0800200c9a66"});
var r1=db.runCommand( { getLastError: 1, w: 1 } );
assert( r1.n == 0 && r1.err.length > 0 && r1.hasOwnProperty("code"), tojson( r1 ) );
transB.insert({"owner":NumberLong("1234567890"),"tid":"2c4ba280-450a-11e2-bcfd-0800200c9a66"});
var rB1=dbB.runCommand( { getLastError: 1, w: 1 } );
assert( rB1.n == 0 && rB1.err.length > 0 && rB1.hasOwnProperty("code"), tojson( r1 ) );

nodeB.update({"owner":NumberLong("1234567890"),"parent":NumberLong("0"),_id:NumberLong("1234567890")},{"$inc":{"counts":1}});
var resultB = dbB.runCommand( { getLastError: 1, w: 1 } )
node.update({"owner":NumberLong("1234567890"),"parent":NumberLong("0"),_id:NumberLong("1234567890")},{"$inc":{"counts":1}});
var result = db.runCommand( { getLastError: 1, w: 1 } )

assert.eq( 2, node.findOne().counts );

printjson( result )
printjson( resultB )

assert( result.n==1 && result.updatedExisting==true && result.err == null, "update succeeded on collection node on mongos A but GLE was\nn=" + result.n + ",\nupdatedExisting=" + result.updatedExisting + ",\nerr=" + result.err);
assert( resultB.n==1 && resultB.updatedExisting==true && resultB.err == null, "update succeeded on collection node on mongos B but GLE was\nn=" + resultB.n + ",\nupdatedExisting=" + resultB.updatedExisting + ",\nerr=" + resultB.err);

s.stop();
