// This test fails when run with authentication due to SERVER-6327
/**
 * Test SyncClusterConnection commands using call instead of findOne
 */

// Note: count command uses call

var st = new ShardingTest({ shards: [], other: { sync: true, separateConfig: true }});
var configDB = st.config;
var coll = configDB.test;

for( var x = 0; x < 10; x++ ){
    coll.insert({ v: x });
}

var testNormalCount = function(){
    var cmdRes = configDB.runCommand({ count: coll.getName() });
    assert( cmdRes.ok );
    assert.eq( 10, cmdRes.n );
};

var testCountWithQuery = function(){
    var cmdRes = configDB.runCommand({ count: coll.getName(), query: { v: { $gt: 6 }}});
    assert( cmdRes.ok );
    assert.eq( 3, cmdRes.n );
};

// Use invalid query operator to make the count return error
var testInvalidCount = function(){
    var cmdRes = configDB.runCommand({ count: coll.getName(), query: { $c: { $abc: 3 }}});
    assert( !cmdRes.ok );
    assert( cmdRes.errmsg.length > 0 );
};

// Test with all config servers up
testNormalCount();
testCountWithQuery();
testInvalidCount();

// Test with the first config server down
var firstConfigOpts = st.c0.commandLine;
MongoRunner.stopMongod( firstConfigOpts.port );

testNormalCount();
testCountWithQuery();
testInvalidCount();

firstConfigOpts.restart = true;
MongoRunner.runMongod( firstConfigOpts );

// Test with the second config server down
MongoRunner.stopMongod( st.c1.commandLine.port );
jsTest.log( 'Second server is down' );

testNormalCount();
testCountWithQuery();
testInvalidCount();

st.stop();

