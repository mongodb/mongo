/**
 * This tests using DB commands with authentication enabled when sharded.
 */
var doTest = function() {

var rsOpts = { oplogSize: 10, verbose : 2, useHostname : false };
var st = new ShardingTest({ keyFile : 'jstests/libs/key1', shards : 2, chunksize : 1, config : 3,
                            rs : rsOpts, other : { nopreallocj : 1, verbose : 2, useHostname : false }});

var mongos = st.s;
var adminDB = mongos.getDB( 'admin' );
var configDB = mongos.getDB( 'config' );
var testDB = mongos.getDB( 'test' );

// Secondaries should be up here, since we awaitReplication in the ShardingTest, but we *don't*
// wait for the mongos to explicitly detect them.
ReplSetTest.awaitRSClientHosts( mongos, st.rs0.getSecondaries(), { ok : true, secondary : true });
ReplSetTest.awaitRSClientHosts( mongos, st.rs1.getSecondaries(), { ok : true, secondary : true });

st.printShardingStatus();

jsTestLog('Setting up initial users');
var rwUser = 'rwUser';
var roUser = 'roUser';
var password = 'password';

adminDB.addUser( rwUser, password, false, st.rs0.numNodes );

assert( adminDB.auth( rwUser, password ) );
adminDB.addUser( roUser, password, true );
testDB.addUser( rwUser, password, false, st.rs0.numNodes );
testDB.addUser( roUser, password, true, st.rs0.numNodes );

authenticatedConn = new Mongo( mongos.host );
authenticatedConn.getDB( 'admin' ).auth( rwUser, password );

// Add user to shards to prevent localhost connections from having automatic full access
st.rs0.getPrimary().getDB( 'admin' ).addUser( 'user', 'password', false, 3 );
st.rs1.getPrimary().getDB( 'admin' ).addUser( 'user', 'password', false, 3 );



jsTestLog('Creating initial data');

st.adminCommand( { enablesharding : "test" } );
st.adminCommand( { shardcollection : "test.foo" , key : { i : 1, j : 1 } } );

// Stop the balancer, so no moveChunks will interfere with the splits we're testing
st.stopBalancer()

var str = 'a';
while ( str.length < 8000 ) {
    str += str;
}
for ( var i = 0; i < 100; i++ ) {
    for ( var j = 0; j < 10; j++ ) {
        testDB.foo.save({i:i, j:j, str:str});
    }
}
testDB.getLastError( 'majority' );

assert.eq(1000, testDB.foo.count());

// Wait for the balancer to start back up
st.startBalancer()

// Make sure we've done at least some splitting, so the balancer will work
assert.gt( configDB.chunks.find({ ns : 'test.foo' }).count(), 2 )

// Make sure we eventually balance all the chunks we've created
assert.soon( function() {
    var x = st.chunkDiff( "foo", "test" );
    print( "chunk diff: " + x );
    return x < 2 && configDB.locks.findOne({ _id : 'test.foo' }).state == 0;
}, "no balance happened", 5 * 60 * 1000 );

assert.soon( function(){
    print( "Waiting for migration cleanup to occur..." )
    return testDB.foo.find().itcount() == testDB.foo.count();
})

var map = function() { emit (this.i, this.j) };
var reduce = function( key, values ) {
    var jCount = 0;
    values.forEach( function(j) { jCount += j; } );
    return jCount;
};

var checkCommandSucceeded = function( db, cmdObj ) {
    print( "Running command that should succeed: " );
    printjson( cmdObj );
    resultObj = db.runCommand( cmdObj );
    printjson( resultObj )
    assert ( resultObj.ok );
    return resultObj;
}

var checkCommandFailed = function( db, cmdObj ) {
    print( "Running command that should fail: " );
    printjson( cmdObj );
    resultObj = db.runCommand( cmdObj );
    printjson( resultObj )
    assert ( !resultObj.ok );
    return resultObj;
}

var checkReadOps = function( hasReadAuth ) {
    if ( hasReadAuth ) {
        print( "Checking read operations, should work" );
        assert.eq( 1000, testDB.foo.find().itcount() );
        assert.eq( 1000, testDB.foo.count() );
        assert.eq( null, testDB.runCommand({getlasterror : 1}).err );
        checkCommandSucceeded( testDB, {dbstats : 1} );
        checkCommandSucceeded( testDB, {collstats : 'foo'} );

        // inline map-reduce works read-only
        var res = checkCommandSucceeded( testDB, {mapreduce : 'foo', map : map, reduce : reduce,
                                                  out : {inline : 1}});
        assert.eq( 100, res.results.length );
        assert.eq( 45, res.results[0].value );

        res = checkCommandSucceeded( testDB,
                                     {aggregate:'foo',
                                      pipeline: [ {$project : {j : 1}},
                                                  {$group : {_id : 'j', sum : {$sum : '$j'}}}]} );
        assert.eq( 4500, res.result[0].sum );
    } else {
        print( "Checking read operations, should fail" );
        assert.throws( function() { testDB.foo.find().itcount(); } );
        checkCommandFailed( testDB, {dbstats : 1} );
        checkCommandFailed( testDB, {collstats : 'foo'} );
        checkCommandFailed( testDB, {mapreduce : 'foo', map : map, reduce : reduce,
                                     out : { inline : 1 }} );
        checkCommandFailed( testDB, {aggregate:'foo',
                                     pipeline: [ {$project : {j : 1}},
                                                 {$group : {_id : 'j', sum : {$sum : '$j'}}}]} );
    }
}

var checkWriteOps = function( hasWriteAuth ) {
    if ( hasWriteAuth ) {
        print( "Checking write operations, should work" );
        testDB.foo.insert({a : 1, i : 1, j : 1});
        res = checkCommandSucceeded( testDB, { findAndModify: "foo", query: {a:1, i:1, j:1},
                                               update: {$set: {b:1}}});
        assert.eq(1, res.value.a);
        assert.eq(null, res.value.b);
        assert.eq(1, testDB.foo.findOne({a:1}).b);
        testDB.foo.remove({a : 1});
        assert.eq( null, testDB.runCommand({getlasterror : 1}).err );
        checkCommandSucceeded( testDB, {reIndex:'foo'} );
        checkCommandSucceeded( testDB, {repairDatabase : 1} );
        checkCommandSucceeded( testDB, {mapreduce : 'foo', map : map, reduce : reduce,
                                        out : 'mrOutput'} );
        assert.eq( 100, testDB.mrOutput.count() );
        assert.eq( 45, testDB.mrOutput.findOne().value );

        checkCommandSucceeded( testDB, {drop : 'foo'} );
        assert.eq( 0, testDB.foo.count() );
        testDB.foo.insert({a:1});
        assert.eq( 1, testDB.foo.count() );
        checkCommandSucceeded( testDB, {dropDatabase : 1} );
        assert.eq( 0, testDB.foo.count() );
        checkCommandSucceeded( testDB, {create : 'baz'} );
    } else {
        print( "Checking write operations, should fail" );
        testDB.foo.insert({a : 1, i : 1, j : 1});
        assert.eq(0, authenticatedConn.getDB('test').foo.count({a : 1, i : 1, j : 1}));
        checkCommandFailed( testDB, { findAndModify: "foo", query: {a:1, i:1, j:1},
                                      update: {$set: {b:1}}} );
        checkCommandFailed( testDB, {reIndex:'foo'} );
        checkCommandFailed( testDB, {repairDatabase : 1} );
        checkCommandFailed( testDB, {mapreduce : 'foo', map : map, reduce : reduce,
                                     out : 'mrOutput'} );
        checkCommandFailed( testDB, {drop : 'foo'} );
        checkCommandFailed( testDB, {dropDatabase : 1} );
        passed = true;
        try {
            // For some reason when create fails it throws an exception instead of just returning ok:0
            res = testDB.runCommand( {create : 'baz'} );
            if ( !res.ok ) {
                passed = false;
            }
        } catch (e) {
            // expected
            printjson(e);
            passed = false;
        }
        assert( !passed );
    }
}

var checkAdminReadOps = function( hasReadAuth ) {
    if ( hasReadAuth ) {
        checkCommandSucceeded( adminDB, {getShardVersion : 'test.foo'} );
        checkCommandSucceeded( adminDB, {getCmdLineOpts : 1} );
        checkCommandSucceeded( adminDB, {serverStatus : 1} );
        checkCommandSucceeded( adminDB, {listShards : 1} );
        checkCommandSucceeded( adminDB, {whatsmyuri : 1} );
        checkCommandSucceeded( adminDB, {isdbgrid : 1} );
        checkCommandSucceeded( adminDB, {ismaster : 1} );
    } else {
        checkCommandFailed( adminDB, {getShardVersion : 'test.foo'} );
        checkCommandFailed( adminDB, {getCmdLineOpts : 1} );
        checkCommandFailed( adminDB, {serverStatus : 1} );
        checkCommandFailed( adminDB, {listShards : 1} );
        // whatsmyuri, isdbgrid, and ismaster don't require any auth
        checkCommandSucceeded( adminDB, {whatsmyuri : 1} );
        checkCommandSucceeded( adminDB, {isdbgrid : 1} );
        checkCommandSucceeded( adminDB, {ismaster : 1} );
    }
}

var checkAdminWriteOps = function( hasWriteAuth ) {
    if ( hasWriteAuth ) {
        checkCommandSucceeded( adminDB, {split : 'test.foo', find : {i : 1, j : 1}} );
        chunk = configDB.chunks.findOne({ shard : st.rs0.name });
        checkCommandSucceeded( adminDB, {moveChunk : 'test.foo', find : chunk.min,
                                         to : st.rs1.name, _waitForDelete : true} );
        // $eval is now an admin operation
        checkCommandSucceeded( testDB, { $eval : 'db.baz.insert({a:1});'} );
        assert.eq(1, testDB.baz.findOne().a);
        res = checkCommandSucceeded( testDB, { $eval : 'return db.baz.findOne();'} );
        assert.eq(1, res.retval.a);
    } else {
        checkCommandFailed( adminDB, {split : 'test.foo', find : {i : 1, j : 1}} );
        chunkKey = { i : { $minKey : 1 }, j : { $minKey : 1 } };
        checkCommandFailed( adminDB, {moveChunk : 'test.foo', find : chunkKey,
                                      to : st.rs1.name, _waitForDelete : true} );
        checkCommandFailed( testDB, { $eval : 'return db.baz.insert({a:1});'} );
        // Takes full admin privilege to run $eval, even if it's only doing a read operation
        checkCommandFailed( testDB, { $eval : 'return db.baz.findOne();'} );

    }
}

var checkRemoveShard = function( hasWriteAuth ) {
    if ( hasWriteAuth ) {
        // start draining
        checkCommandSucceeded( adminDB, { removeshard : st.rs1.name } );
        // Wait for shard to be completely removed
        checkRemoveShard = function() {
            res = checkCommandSucceeded( adminDB, { removeshard : st.rs1.name } );
            return res.msg == 'removeshard completed successfully';
        }
        assert.soon( checkRemoveShard , "failed to remove shard" );
    } else {
        checkCommandFailed( adminDB, { removeshard : st.rs1.name } );
    }
}

var checkAddShard = function( hasWriteAuth ) {
    if ( hasWriteAuth ) {
        checkCommandSucceeded( adminDB, { addshard : st.rs1.getURL() } );
    } else {
        checkCommandFailed( adminDB, { addshard : st.rs1.getURL() } );
    }
}


st.stopBalancer();

jsTestLog("Checking admin commands with read-write auth credentials");
checkAdminWriteOps( true );
assert( adminDB.logout().ok );

jsTestLog("Checking admin commands with no auth credentials");
checkAdminReadOps( false );
checkAdminWriteOps( false );

jsTestLog("Checking admin commands with read-only auth credentials");
assert( adminDB.auth( roUser, password ) );
checkAdminReadOps( true );
checkAdminWriteOps( false );
assert( adminDB.logout().ok );

jsTestLog("Checking commands with no auth credentials");
checkReadOps( false );
checkWriteOps( false );

// Authenticate as read-only user
jsTestLog("Checking commands with read-only auth credentials");
assert( testDB.auth( roUser, password ) );
checkReadOps( true );
checkWriteOps( false );

// Authenticate as read-write user
jsTestLog("Checking commands with read-write auth credentials");
assert( testDB.auth( rwUser, password ) );
checkReadOps( true );
checkWriteOps( true );


jsTestLog("Check drainging/removing a shard");
assert( testDB.logout().ok );
checkRemoveShard( false );
assert( adminDB.auth( roUser, password ) );
checkRemoveShard( false );
assert( adminDB.auth( rwUser, password ) );
assert( testDB.dropDatabase().ok );
checkRemoveShard( true );
adminDB.printShardingStatus();

jsTestLog("Check adding a shard")
assert( adminDB.logout().ok );
checkAddShard( false );
assert( adminDB.auth( roUser, password ) );
checkAddShard( false );
assert( adminDB.auth( rwUser, password ) );
checkAddShard( true );
adminDB.printShardingStatus();


st.stop();
}

if (0) { // SERVER-10668
    doTest();
}