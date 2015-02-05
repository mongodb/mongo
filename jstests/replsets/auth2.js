
var testInvalidAuthStates = function() {
    print("check that 0 is in recovering");
    rs.waitForState(rs.nodes[0], rs.RECOVERING);

    print("shut down 1, 0 still in recovering.");
    rs.stop(1);
    sleep(5);

    rs.waitForState(rs.nodes[0], rs.RECOVERING);

    print("shut down 2, 0 becomes a secondary.");
    rs.stop(2);

    rs.waitForState(rs.nodes[0], rs.SECONDARY);

    rs.restart(1, {"keyFile" : path+"key1"});
    rs.restart(2, {"keyFile" : path+"key1"});
};

var name = "rs_auth2";
var path = "jstests/libs/";

print("change permissions on #1 & #2");
run("chmod", "600", path+"key1");
run("chmod", "600", path+"key2");

var rs = new ReplSetTest({name: name, nodes: 3});
var nodes = rs.startSet();
var hostnames = rs.nodeList();
rs.initiate({ "_id" : name,
                    "members" : [
                        {"_id" : 0, "host" : hostnames[0], "priority" : 2},
                        {"_id" : 1, "host" : hostnames[1], priority: 0},
                        {"_id" : 2, "host" : hostnames[2], priority: 0}
                    ]});

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
