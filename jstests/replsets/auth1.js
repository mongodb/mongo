// check replica set authentication

load("jstests/replsets/rslib.js");

var name = "rs_auth1";
var port = allocatePorts(5);
var path = "jstests/libs/";


print("try starting mongod with auth");
var m = MongoRunner.runMongod({auth : "", port : port[4], dbpath : "/data/db/wrong-auth"});

assert.eq(m.getDB("local").auth("__system", ""), 0);

stopMongod(port[4]);


print("reset permissions");
run("chmod", "644", path+"key1");
run("chmod", "644", path+"key2");


print("try starting mongod");
m = runMongoProgram( "mongod", "--keyFile", path+"key1", "--port", port[0], "--dbpath", "/data/db/" + name);


print("should fail with wrong permissions");
assert.eq(m, _isWindows()? 100 : 2, "mongod should exit w/ 2: permissions too open");
stopMongod(port[0]);


print("change permissions on #1 & #2");
run("chmod", "600", path+"key1");
run("chmod", "600", path+"key2");


print("add a user to server0: foo");
m = startMongodTest( port[0], name+"-0", 0 );
m.getDB("admin").addUser("foo", "bar");
m.getDB("test").addUser("bar", "baz");
print("make sure user is written before shutting down");
m.getDB("test").getLastError();
stopMongod(port[0]);

if ( !_isWindows() ) {  // SERVER-5024

print("start up rs");
var rs = new ReplSetTest({"name" : name, "nodes" : 3, "startPort" : port[0]});
m = rs.restart(0, {"keyFile" : path+"key1"});
var s = rs.start(1, {"keyFile" : path+"key1"});
var s2 = rs.start(2, {"keyFile" : path+"key1"});

var result = m.getDB("admin").auth("foo", "bar");
assert.eq(result, 1, "login failed");
result = m.getDB("admin").runCommand({replSetInitiate : rs.getReplSetConfig()});
assert.eq(result.ok, 1, "couldn't initiate: "+tojson(result));

var master = rs.getMaster().getDB("test");
wait(function() {
        var status = master.adminCommand({replSetGetStatus:1});
        return status.members && status.members[1].state == 2 && status.members[2].state == 2;
    });

master.foo.insert({x:1});
master.runCommand({getlasterror:1, w:3, wtimeout:60000});


print("try some legal and illegal reads");
var r = master.foo.findOne();
assert.eq(r.x, 1);

s.setSlaveOk();
slave = s.getDB("test");

function doQueryOn(p) {
    var err = {};
    try {
        r = p.foo.findOne();
    }
    catch(e) {
        if (typeof(JSON) != "undefined") {
            err = JSON.parse(e.substring(6));
        }
        else if (e.indexOf("10057") > 0) {
            err.code = 10057;
        }
    }
    assert.eq(err.code, 10057);
};

doQueryOn(slave);
master.adminCommand({logout:1});

print("unauthorized:");
printjson(master.adminCommand({replSetGetStatus : 1}));

doQueryOn(master);


result = slave.auth("bar", "baz");
assert.eq(result, 1);

r = slave.foo.findOne();
assert.eq(r.x, 1);


print("add some data");
master.auth("bar", "baz");
for (var i=0; i<1000; i++) {
    master.foo.insert({x:i, foo : "bar"});
}
master.runCommand({getlasterror:1, w:3, wtimeout:60000});


print("fail over");
rs.stop(0);

wait(function() {
        function getMaster(s) {
            var result = s.getDB("admin").runCommand({isMaster: 1});
            printjson(result);
            if (result.ismaster) {
                master = s.getDB("test");
                return true;
            }
            return false;
        }

        if (getMaster(s) || getMaster(s2)) {
            return true;
        }
        return false;
    });


print("add some more data 1");
master.auth("bar", "baz");
for (var i=0; i<1000; i++) {
    master.foo.insert({x:i, foo : "bar"});
}
var result = master.runCommand({getlasterror:1, w:2, wtimeout:60000});
printjson(result);


print("resync");
rs.restart(0, {"keyFile" : path+"key1"});


print("add some more data 2");
for (var i=0; i<1000; i++) {
    master.foo.insert({x:i, foo : "bar"});
}
master.runCommand({getlasterror:1, w:3, wtimeout:60000});


print("add member with wrong key");
var conn = new MongodRunner(port[3], "/data/db/"+name+"-3", null, null, ["--replSet","rs_auth1","--rest","--oplogSize","2", "--keyFile", path+"key2"], {no_bind : true});
conn.start();


master.getSisterDB("admin").auth("foo", "bar");
var config = master.getSisterDB("local").system.replset.findOne();
config.members.push({_id : 3, host : getHostName()+":"+port[3]});
config.version++;
try {
    master.adminCommand({replSetReconfig:config});
}
catch (e) {
    print("error: "+e);
}
reconnect(master);
master.getSisterDB("admin").auth("foo", "bar");


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
conn = new MongodRunner(port[3], "/data/db/"+name+"-3", null, null, ["--replSet","rs_auth1","--rest","--oplogSize","2", "--keyFile", path+"key1"], {no_bind : true});
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
    } // !isWindows