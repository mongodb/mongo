/**
 * This tests using DB commands with authentication enabled when sharded.
 */

var rsOpts = { oplogSize: 10 };
var st = new ShardingTest({ keyFile : 'jstests/libs/key1', shards : 2, chunksize : 1,
                            rs : rsOpts, other : { nopreallocj : 1, verbose :1 }});

var mongos = st.s;
var adminDB = mongos.getDB( 'admin' );
var configDB = mongos.getDB( 'config' );
var testDB = mongos.getDB( 'test' );

// Secondaries should be up here, since we awaitReplication in the ShardingTest, but we *don't*
// wait for the mongos to explicitly detect them.
ReplSetTest.awaitRSClientHosts( mongos, st.rs0.getSecondaries(), { ok : true, secondary : true });
ReplSetTest.awaitRSClientHosts( mongos, st.rs1.getSecondaries(), { ok : true, secondary : true });

var rwUser = 'rwUser';
var roUser = 'roUser';
var password = 'password';

jsTestLog('Setting up initial users');
adminDB.addUser( rwUser, password, false, st.rs0.numNodes );
assert( adminDB.auth( rwUser, password ) );
adminDB.addUser( roUser, password, true, st.rs0.numNodes );
testDB.addUser( rwUser, password, false, st.rs0.numNodes );
testDB.addUser( roUser, password, true, st.rs0.numNodes );


jsTestLog('Creating initial data');

st.adminCommand( { enablesharding : "test" } );
st.adminCommand( { shardcollection : "test.foo" , key : { i : 1, j : 1 } } );

var str = 'a';
while ( str.length < 1024 * 16 ) {
    str += str;
}
for ( var i = 0; i < 100; i++ ) {
    for ( var j = 0; j < 10; j++ ) {
        testDB.foo.save({i:i, j:j, str:str});
    }
}
testDB.getLastError( 'majority' );
assert.soon( function() {
    var x = st.chunkDiff( "foo", "test" );
    print( "chunk diff: " + x );
    return x < 2 && configDB.locks.findOne({ _id : 'test.foo' }).state == 0;
}, "no balance happened" );

var map = function() { emit (this.i, this.j) };
var reduce = function( key, values ) {
    var jCount = 0;
    values.forEach( function(j) { jCount += j; } );
    return jCount;
};

var checkCommandSucceeded = function( resultObj ) {
    printjson( resultObj )
    assert ( resultObj.ok );
}

var checkCommandFailed = function( resultObj ) {
    print("INTHETHING")
    printjson( resultObj )
    assert ( !resultObj.ok );
}

var checkReadOps = function( shouldWork ) {
    if ( shouldWork ) {
        print( "Checking read operations, should work" );
        assert.eq( 1000, testDB.foo.find().itcount() );
        assert.eq( 1000, testDB.foo.count() );
        assert.eq( null, testDB.runCommand({getlasterror : 1}).err );
        checkCommandSucceeded( testDB.runCommand({dbstats : 1}) );
        checkCommandSucceeded( testDB.runCommand({collstats : 'foo'}) );

        // TODO: Uncomment this once inline mapreduce has been handled.
        /*var res = testDB.runCommand({mapreduce : 'foo', map : map, reduce : reduce,
                                     out : {inline : 1}});
        checkCommandSucceeded( res );
        assert.eq( 100, res.results.length );
        assert.eq( 45, res.results[0].value );*/

    } else {
        print( "Checking read operations, should fail" );
        assert.throws( function() { testDB.foo.find().itcount(); } );
        assert.eq(0, testDB.runCommand({getlasterror : 1}).err.indexOf('unauthorized') );
        checkCommandFailed( testDB.runCommand({dbstats : 1}) );
        checkCommandFailed( testDB.runCommand({collstats : 'foo'}) );
        checkCommandFailed( testDB.runCommand({mapreduce : 'foo', map : map, reduce : reduce,
                                               out : { inline : 1 }}) );
    }
}

var checkWriteOps = function( shouldWork ) {
    if ( shouldWork ) {
        print( "Checking write operations, should work" );
        testDB.foo.insert({a : 1, i : 1, j : 1});
        res = testDB.runCommand({ findAndModify: "foo", query: {a:1, i:1, j:1},
                                                   update: {$set: {b:1}}});
        checkCommandSucceeded(res);
        assert.eq(1, res.value.a);
        assert.eq(null, res.value.b);
        assert.eq(1, testDB.foo.findOne({a:1}).b);
        testDB.foo.remove({a : 1});
        assert.eq( null, testDB.runCommand({getlasterror : 1}).err );
        checkCommandSucceeded( testDB.runCommand({reIndex:'foo'}) );
        checkCommandSucceeded( testDB.runCommand({repairDatabase : 1}) );
        checkCommandSucceeded( testDB.runCommand({mapreduce : 'foo', map : map, reduce : reduce,
                                                  out : 'mrOutput'}) );
        assert.eq( 100, testDB.mrOutput.count() );
        assert.eq( 45, testDB.mrOutput.findOne().value );

        checkCommandSucceeded( testDB.runCommand({drop : 'foo'}) );
        assert.eq( 0, testDB.foo.count() );
        testDB.foo.insert({a:1});
        assert.eq( 1, testDB.foo.count() );
        checkCommandSucceeded( testDB.runCommand({dropDatabase : 1}) );
        assert.eq( 0, testDB.foo.count() );
        // TODO: Uncomment this once create follows regular command codepath
        //checkCommandSucceeded( testDB.runCommand({create : 'bar'}) );
    } else {
        print( "Checking write operations, should fail" );
        testDB.foo.insert({a : 1, i : 1, j : 1});
        assert.eq(0, testDB.runCommand({getlasterror:1}).err.indexOf('unauthorized') );
        checkCommandFailed( testDB.runCommand({ findAndModify: "foo", query: {a:1, i:1, j:1},
                                                update: {$set: {b:1}}}) );
        checkCommandFailed( testDB.runCommand({reIndex:'foo'}) );
        checkCommandFailed( testDB.runCommand({repairDatabase : 1}) );
        checkCommandFailed( testDB.runCommand({mapreduce : 'foo', map : map, reduce : reduce,
                                               out : 'mrOutput'}) );
        checkCommandFailed( testDB.runCommand({drop : 'foo'}) );
        checkCommandFailed( testDB.runCommand({dropDatabase : 1}) );
        passed = true;
        try {
            // For some reason when create fails it throws an exception instead of just returning ok:0
            checkCommandFailed( testDB.runCommand({create : 'bar'}) );
            passed = false;
        } catch (e) {
            // expected
            printjson(e);
            passed = false;
        }
        assert( !passed );
    }
}

var checkAdminReadOps = function( shouldWork ) {
    if ( shouldWork ) {
        checkCommandSucceeded( adminDB.runCommand('getCmdLineOpts') );
        checkCommandSucceeded( adminDB.runCommand('serverStatus') );
    } else {
        checkCommandFailed( adminDB.runCommand('getCmdLineOpts') );
        checkCommandFailed( adminDB.runCommand('serverStatus') );
    }
}

var checkAdminWriteOps = function( shouldWork ) {
    if ( shouldWork ) {
        checkCommandSucceeded( adminDB.runCommand({split : 'test.foo', find : {i : 1, j : 1}}) );
        chunk = configDB.chunks.findOne({ shard : st.rs0.name });
        checkCommandSucceeded( adminDB.runCommand({moveChunk : 'test.foo', find : chunk.min,
                                                   to : st.rs1.name}) );
    } else {
        // TODO(spencer): Either uncomment these or remove them once the proper behavior is established.
        /*checkCommandFailed( adminDB.runCommand({split : 'test.foo', find : {i : 1, j : 1}}) );
        chunkKey = { i : { $minKey : 1 }, j : { $minKey : 1 } };
        checkCommandFailed( adminDB.runCommand({moveChunk : 'test.foo', find : chunkKey,
                                                to : st.rs1.name}) );*/
    }
}

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

st.stop();
