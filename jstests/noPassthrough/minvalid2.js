/**
 * This checks rollback, which shouldn't happen unless we have reached minvalid.
 *  1. make 3-member set w/arb (2)
 *  2. shut down 1
 *  3. do writes to 0
 *  4. modify 0's minvalid
 *  5. shut down 0
 *  6. start up 1
 *  7. writes on 1
 *  8. start up 0
 *  9. check 0 does not rollback
 */

print("1. make 3-member set w/arb (2)");
var name = "minvalid"
var replTest = new ReplSetTest({name: name, nodes: 3, oplogSize:1});
var host = getHostName();

var nodes = replTest.startSet();
replTest.initiate({_id : name, members : [
    {_id : 0, host : host+":"+replTest.ports[0]},
    {_id : 1, host : host+":"+replTest.ports[1]},
    {_id : 2, host : host+":"+replTest.ports[2], arbiterOnly : true}
]});
var master = replTest.getMaster();
var mdb = master.getDB("foo");

mdb.foo.save({a: 1000});
replTest.awaitReplication();

print("2: shut down 1");
replTest.stop(1);

print("3: do writes to 0");
mdb.foo.save({a: 1001});

print("4: modify 0's minvalid");
var local = master.getDB("local");
var lastOp = local.oplog.rs.find().sort({$natural:-1}).limit(1).next();
printjson(lastOp);

local.replset.minvalid.insert({ts:new Timestamp(lastOp.ts.t, lastOp.ts.i+1),
                               h:new NumberLong("1234567890")});
printjson(local.replset.minvalid.findOne());

print("5: shut down 0");
replTest.stop(0);

print("6: start up 1");
replTest.restart(1);

print("7: writes on 1")
master = replTest.getMaster();
mdb1 = master.getDB("foo");
mdb1.foo.save({a:1002});

print("8: start up 0");
replTest.restart(0);

print("9: check 0 does not rollback");
assert.soon(function(){
    var status = master.adminCommand({replSetGetStatus:1});
    var stateStr = status.members[0].stateStr;
    assert(stateStr != "ROLLBACK" &&
           stateStr != "SECONDARY" &&
           stateStr != "PRIMARY", tojson(status));
    return stateStr == "FATAL";
});

replTest.stopSet(15);
