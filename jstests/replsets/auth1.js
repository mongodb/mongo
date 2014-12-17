// check replica set authentication

load("jstests/replsets/rslib.js");

var name = "rs_auth1";
var port = allocatePorts(5);
var path = "jstests/libs/";


print("try starting mongod with auth");
var m = MongoRunner.runMongod({auth : "", port : port[4], dbpath : MongoRunner.dataDir + "/wrong-auth"});

assert.eq(m.getDB("local").auth("__system", ""), 0);

stopMongod(port[4]);


print("reset permissions");
run("chmod", "644", path+"key1");
run("chmod", "644", path+"key2");


print("try starting mongod");
m = runMongoProgram( "mongod", "--keyFile", path+"key1", "--port", port[0], "--dbpath", MongoRunner.dataPath + name);


print("should fail with wrong permissions");
assert.eq(m, _isWindows()? 100 : 1, "mongod should exit w/ 1 (EXIT_FAILURE): permissions too open");
stopMongod(port[0]);


print("change permissions on #1 & #2");
run("chmod", "600", path+"key1");
run("chmod", "600", path+"key2");


print("add a user to server0: foo");
m = startMongodTest( port[0], name+"-0", 0 );
m.getDB("admin").createUser({user: "foo", pwd: "bar", roles: jsTest.adminUserRoles});
m.getDB("test").createUser({user: "bar", pwd: "baz", roles: jsTest.basicUserRoles});
print("make sure user is written before shutting down");
stopMongod(port[0]);

print("start up rs");
var rs = new ReplSetTest({"name" : name, "nodes" : 3, "startPort" : port[0]});
print("restart 0 with keyFile");
m = rs.restart(0, {"keyFile" : path+"key1"});
print("restart 1 with keyFile");
rs.start(1, {"keyFile" : path+"key1"});
print("restart 2 with keyFile");
rs.start(2, {"keyFile" : path+"key1"});

var result = m.getDB("admin").auth("foo", "bar");
assert.eq(result, 1, "login failed");
print("Initializing replSet with config: " + tojson(rs.getReplSetConfig()));
result = m.getDB("admin").runCommand({replSetInitiate : rs.getReplSetConfig()});
assert.eq(result.ok, 1, "couldn't initiate: "+tojson(result));

var master = rs.getMaster();
rs.awaitSecondaryNodes();
var mId = rs.getNodeId(master);
var slave = rs.liveNodes.slaves[0];

master.getDB("test").foo.insert({ x: 1 }, { writeConcern: { w:3, wtimeout:60000 }});

print("try some legal and illegal reads");
var r = master.getDB("test").foo.findOne();
assert.eq(r.x, 1);

slave.setSlaveOk();

function doQueryOn(p) {
    var err = {};
    try {
        r = p.getDB("test").foo.findOne();
    }
    catch(e) {
        if (typeof(JSON) != "undefined") {
            err = JSON.parse(e.message.substring(6));
        }
        else if (e.indexOf("13") > 0) {
            err.code = 13;
        }
    }
    assert.eq(err.code, 13);
};

doQueryOn(slave);
master.adminCommand({logout:1});

print("unauthorized:");
printjson(master.adminCommand({replSetGetStatus : 1}));

doQueryOn(master);


result = slave.getDB("test").auth("bar", "baz");
assert.eq(result, 1);

r = slave.getDB("test").foo.findOne();
assert.eq(r.x, 1);


print("add some data");
master.getDB("test").auth("bar", "baz");
var bulk = master.getDB("test").foo.initializeUnorderedBulkOp();
for (var i=0; i<1000; i++) {
    bulk.insert({ x: i, foo: "bar" });
}
assert.writeOK(bulk.execute({ w: 3, wtimeout: 60000 }));

print("fail over");
rs.stop(mId);

master = rs.getMaster();

print("add some more data 1");
master.getDB("test").auth("bar", "baz");
bulk = master.getDB("test").foo.initializeUnorderedBulkOp();
for (var i=0; i<1000; i++) {
    bulk.insert({ x: i, foo: "bar" });
}
assert.writeOK(bulk.execute({ w: 2 }));

print("resync");
rs.restart(mId, {"keyFile" : path+"key1"});
master = rs.getMaster();

print("add some more data 2");
bulk = master.getDB("test").foo.initializeUnorderedBulkOp();
for (var i=0; i<1000; i++) {
    bulk.insert({ x: i, foo: "bar" });
}
bulk.execute({ w:3, wtimeout:60000 });

print("add member with wrong key");
var conn = new MongodRunner(port[3], MongoRunner.dataPath+name+"-3", null, null, ["--replSet","rs_auth1","--rest","--oplogSize","2", "--keyFile", path+"key2"], {no_bind : true});
conn.start();


master.getDB("admin").auth("foo", "bar");
var config = master.getDB("local").system.replset.findOne();
config.members.push({_id : 3, host : rs.host+":"+port[3]});
config.version++;
try {
    master.adminCommand({replSetReconfig:config});
}
catch (e) {
    print("error: "+e);
}
master = rs.getMaster();
master.getDB("admin").auth("foo", "bar");


print("shouldn't ever sync");
for (var i = 0; i<10; i++) {
    print("iteration: " +i);
    var results = master.adminCommand({replSetGetStatus:1});
    printjson(results);
    assert(results.members[3].state != 2);
    sleep(1000);
}


print("stop member");
stopMongod(port[3]);


print("start back up with correct key");
conn = new MongodRunner(port[3], MongoRunner.dataPath+name+"-3", null, null, ["--replSet","rs_auth1","--rest","--oplogSize","2", "--keyFile", path+"key1"], {no_bind : true});
conn.start();

wait(function() {
    try {
        var results = master.adminCommand({replSetGetStatus:1});
        printjson(results);
        return results.members[3].state == 2;
    }
    catch (e) {
        print(e);
    }
    return false;
    });

print("make sure it has the config, too");
assert.soon(function() {
        for (var i in rs.nodes) {
            rs.nodes[i].setSlaveOk();
            rs.nodes[i].getDB("admin").auth("foo","bar");
            config = rs.nodes[i].getDB("local").system.replset.findOne();
            if (config.version != 2) {
                return false;
            }
        }
        return true;
    });
