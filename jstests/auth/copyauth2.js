// Basic test that copydb works with auth enabled when copying within the same cluster

function runTest(a, b) {
    a.createUser({user: "chevy", pwd: "chase", roles: ["read", {role: 'readWrite', db: b._name}]});
    a.foo.insert({a: 1});
    b.getSiblingDB("admin").logout();

    a.auth("chevy", "chase");

    assert.eq(1, a.foo.count(), "A");
    assert.eq(0, b.foo.count(), "B");

    a.copyDatabase(a._name, b._name);
    assert.eq(1, a.foo.count(), "C");
    assert.eq(1, b.foo.count(), "D");
}

// run all tests standalone
var conn = MongoRunner.runMongod({auth: ""});
var a = conn.getDB("copydb2-test-a");
var b = conn.getDB("copydb2-test-b");
var adminDB = conn.getDB("admin");
adminDB.createUser({user: "root", pwd: "root", roles: ["root"]});
adminDB.auth("root", "root");
runTest(a, b);
MongoRunner.stopMongod(conn);

/** Doesn't work in a sharded setup due to SERVER-13080
// run all tests sharded
var st = new ShardingTest({
    shards: 2,
    mongos: 1,
    keyFile: "jstests/libs/key1",
});
var a = st.s.getDB( "copydb2-test-a" );
var b = st.s.getDB( "copydb2-test-b" );
st.s.getDB( "admin" ).createUser({user: "root", pwd: "root", roles: ["root"]});
st.s.getDB( "admin" ).auth("root", "root");
runTest(a, b);
st.stop();
*/

print("Successfully completed copyauth2.js test.");
