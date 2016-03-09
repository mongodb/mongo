
// this tests that members will stay in RECOVERING state on startup if they have not reached
// their stored minvalid

var name = "minvalid";
var replTest = new ReplSetTest({name: name, nodes: 1, oplogSize: 1});
var host = getHostName();

var nodes = replTest.startSet();
replTest.initiate();

var master = replTest.getPrimary();
var mdb = master.getDB("foo");

print("1: initial insert");
mdb.foo.save({a: 1000});

print("2. get last op");
var local = master.getDB("local");
var lastOp = local.oplog.rs.find().sort({$natural: -1}).limit(1).next();
printjson(lastOp);

print("3: change minvalid");
// primaries don't populate minvalid by default
local.replset.minvalid.insert(
    {ts: new Timestamp(lastOp.ts.t, lastOp.ts.i + 1), h: new NumberLong("1234567890")});
printjson(local.replset.minvalid.findOne());

print("4: restart");
replTest.restart(0);

print("5: make sure it stays in recovering");
var timeout = (new Date()).getTime() + 30000;
while ((new Date().getTime()) < timeout) {
    var status = replTest.nodes[0].getDB("admin").runCommand({isMaster: 1});
    assert(!status.secondary && !status.primary, tojson(status));
    sleep(2000);
}

replTest.stopSet(15);
