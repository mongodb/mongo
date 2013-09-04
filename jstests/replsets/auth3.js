if ( !_isWindows() ) { // SERVER-5024
var path = "jstests/libs/";

var rs = new ReplSetTest({"nodes" : {node0 : {}, node1 : {}, arbiter : {}}, keyFile : path+"key1"});
rs.startSet();
rs.initiate();

master = rs.getMaster();
print("adding user");
master.getDB("admin").addUser("foo", "bar", jsTest.adminUserRoles, 2);

var checkValidState = function(i) {
    assert.soon(function() {
        var result = rs.nodes[i].getDB("admin").runCommand({isMaster : 1});
        printjson(result);
        return result.secondary || result.ismaster;
    });
};

var safeInsert = function() {
    master = rs.getMaster();
    master.getDB("admin").auth("foo", "bar");
    master.getDB("foo").bar.insert({x:1});
    var insertWorked = master.getDB("foo").runCommand({getlasterror:1});
    printjson(insertWorked);
    assert.eq(insertWorked.ok, 1);
}

print("authing");
assert.soon(function() {
    for (var i=0; i<2; i++) {
        checkValidState(i);

        // if this is run before initial sync finishes, we won't be logged in
        var res = rs.nodes[i].getDB("admin").auth("foo", "bar");
        if (res != 1) {
            print("couldn't log into "+rs.nodes[i].host);
            return false;
        }
    }
    return true;
});

print("make common point");

safeInsert();
rs.awaitReplication();

print("write stuff to 0&2")
rs.stop(1);

master = rs.getMaster();
master.getDB("foo").bar.drop();
print("last op: "+tojson(master.getDB("local").oplog.rs.find().sort({$natural:-1}).limit(1).next()));

print("write stuff to 1&2")
rs.stop(0);
rs.restart(1);

safeInsert();
print("last op: "+tojson(master.getDB("local").oplog.rs.find().sort({$natural:-1}).limit(1).next()));

rs.restart(0);

print("doing rollback!");

checkValidState(0);
checkValidState(1);
} // !_isWindows()