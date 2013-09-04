if ( !_isWindows() ) { //SERVER-5024
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

var checkNoAuth = function() {
    print("without an admin user, things should work");

    master.getDB("foo").bar.insert({x:1});
    var result = master.getDB("admin").runCommand({getLastError:1});

    printjson(result);
    assert.eq(result.err, null);
}

var checkInvalidAuthStates = function() {
    print("check that 0 is in recovering");
    assert.soon(function() {
        try {
            var result = m.getDB("admin").runCommand({isMaster: 1});
            printjson(result);
            return !result.ismaster && !result.secondary;
        }
        catch ( e ) {
            print( e );
        }
    });

    print("shut down 1, 0 still in recovering.");
    rs.stop(1);
    sleep(5);

    assert.soon(function() {
        var result = m.getDB("admin").runCommand({isMaster: 1});
        printjson(result);
        return !result.ismaster && !result.secondary;
    });

    print("shut down 2, 0 becomes a secondary.");
    rs.stop(2);

    assert.soon(function() {
        var result = m.getDB("admin").runCommand({isMaster: 1});
        printjson(result);
        return result.secondary;
    });

    rs.restart(1, {"keyFile" : path+"key1"});
    rs.restart(2, {"keyFile" : path+"key1"});
};

var checkValidAuthState = function() {
    assert.soon(function() {
        var result = m.getDB("admin").runCommand({isMaster : 1});
        printjson(result);
        return result.secondary;
    });
};

var rs = setupReplSet();
var master = rs.getMaster();

print("add an admin user");
master.getDB("admin").addUser("foo","bar",jsTest.adminUserRoles,3);
m = rs.nodes[0];

print("starting 1 and 2 with key file");
rs.stop(1);
rs.restart(1, {"keyFile" : path+"key1"});
rs.stop(2);
rs.restart(2, {"keyFile" : path+"key1"});

checkInvalidAuthStates();

print("restart mongod with bad keyFile");

rs.stop(0);
m = rs.restart(0, {"keyFile" : path+"key2"});

checkInvalidAuthStates();

rs.stop(0);
m = rs.restart(0, {"keyFile" : path+"key1"});

print("0 becomes a secondary");
} // !_isWindows()