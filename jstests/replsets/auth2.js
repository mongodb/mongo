var name = "rs_auth2";
var port = allocatePorts(3);
var path = "jstests/libs/";

print("change permissions on #1 & #2");
run("chmod", "600", path+"key1");
run("chmod", "600", path+"key2");

var setupReplSet = function() {
    print("start up rs");
    var rs = new ReplSetTest({"name" : name, "nodes" : 3, "startPort" : port[0]});
    rs.startSet();
    rs.initiate();

    print("getting master");
    rs.getMaster();

    print("getting secondaries");
    assert.soon(function() {
        var result1 = rs.nodes[1].getDB("admin").runCommand({isMaster: 1});
        var result2 = rs.nodes[2].getDB("admin").runCommand({isMaster: 1});
        return result1.secondary && result2.secondary;
    });

    return rs;
};

var testInvalidAuthStates = function() {
    print("check that 0 is in recovering");
    assert.soon(function() {
        try {
            var result = m.adminCommand({isMaster: 1});
            printjson(result);
            printjson(m.adminCommand("replSetGetStatus"));
            printjson(rs.nodes[1].adminCommand("replSetGetStatus"));
            printjson(rs.nodes[2].adminCommand("replSetGetStatus"));
            return !result.ismaster && !result.secondary;
        }
        catch ( e ) {
            print( e );
        }
    }, "node0 isn't recovering");

    print("shut down 1, 0 still in recovering.");
    rs.stop(1);
    sleep(5);

    assert.soon(function() {
        var result = m.adminCommand({isMaster: 1});
        printjson(m.adminCommand("replSetGetStatus"));
        printjson(result);
        return !result.ismaster && !result.secondary;
    }, "node0 isn't recovering");

    print("shut down 2, 0 becomes a secondary.");
    rs.stop(2);

    assert.soon(function() {
        var result = m.adminCommand({isMaster: 1});
        printjson(m.adminCommand("replSetGetStatus"));
        printjson(result);
        return result.secondary;
    }, "node0 isn't secondary");

    rs.restart(1, {"keyFile" : path+"key1"});
    rs.restart(2, {"keyFile" : path+"key1"});
};

var rs = setupReplSet();
var master = rs.getMaster();

print("add an admin user");
master.getDB("admin").createUser({user: "foo", pwd: "bar", roles: jsTest.adminUserRoles},
                                 {w: 3, wtimeout: 30000});
var m = rs.nodes[0];

print("starting 1 and 2 with key file");
rs.stop(1);
rs.restart(1, {"keyFile" : path+"key1"});
rs.stop(2);
rs.restart(2, {"keyFile" : path+"key1"});

// auth to all nodes with auth
rs.nodes[1].getDB("admin").auth("foo", "bar");
rs.nodes[2].getDB("admin").auth("foo", "bar");
testInvalidAuthStates();

print("restart mongod with bad keyFile");

rs.stop(0);
m = rs.restart(0, {"keyFile" : path+"key2"});

//auth to all nodes
rs.nodes[0].getDB("admin").auth("foo", "bar");
rs.nodes[1].getDB("admin").auth("foo", "bar");
rs.nodes[2].getDB("admin").auth("foo", "bar");
testInvalidAuthStates();

rs.stop(0);
m = rs.restart(0, {"keyFile" : path+"key1"});

print("0 becomes a secondary");
