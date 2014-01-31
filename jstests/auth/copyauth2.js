// Basic test that copydb works with auth enabled when copying within the same cluster

function runTest(a, b) {
    a.foo.insert({a:1});
    a.createUser({user: "chevy" , pwd: "chase", roles: ["read", {role:'readWrite', db: b._name}]});
    a.auth('chevy', 'chase');

    assert.eq( 1 , a.foo.count() , "A" );
    assert.eq( 0 , b.foo.count() , "B" );

    a.copyDatabase(a._name , b._name);
    assert.eq( 1 , a.foo.count() , "C" );
    assert.eq( 1 , b.foo.count() , "D" );
}



// run all tests standalone
var conn = MongoRunner.runMongod({auth:""});
var a = conn.getDB( "copydb2-test-a" );
var b = conn.getDB( "copydb2-test-b" );
runTest(a, b);
MongoRunner.stopMongod(conn);

// run all tests sharded
var st = new ShardingTest({
    shards: 2,
    mongos: 1,
    keyFile: "jstests/libs/key1",
});
var a = st.s.getDB( "copydb2-test-a" );
var b = st.s.getDB( "copydb2-test-b" );
runTest(a, b);
st.stop();
