// Check debug information recorded for a query.

// special db so that it can be run in parallel tests
var stddb = db;
var db = db.getSisterDB("profile4");

db.removeAllUsers();
t = db.profile4;
t.drop();

function profileCursor() {
    return db.system.profile.find( { user:username + "@" + db.getName() } );
}

function lastOp() {
    p = profileCursor().sort( { $natural:-1 } ).next();
//    printjson( p );
    return p;
}

function checkLastOp( spec ) {
    p = lastOp();
    for( i in spec ) {
        s = spec[ i ];
        assert.eq( s[ 1 ], p[ s[ 0 ] ], s[ 0 ] );
    }
}

try {
    username = "jstests_profile4_user";
    db.addUser( username, "password", jsTest.basicUserRoles, 1 );
    db.auth( username, "password" );
    
    db.setProfilingLevel(0);
    
    db.system.profile.drop();
    assert.eq( 0 , profileCursor().count() )
    
    db.setProfilingLevel(2);

    t.find().itcount();
    checkLastOp( [ [ "op", "query" ],
                  [ "ns", "profile4.profile4" ],
                  [ "query", {} ],
                  [ "ntoreturn", 0 ],
                  [ "ntoskip", 0 ],
                  [ "nscanned", 0 ],
                  [ "keyUpdates", 0 ],
                  [ "nreturned", 0 ],
                  [ "responseLength", 20 ] ] );
    
    t.save( {} );

    // check write lock stats are set
    o = lastOp();
    assert.eq('insert', o.op);
    assert.eq( 0, o.lockStats.timeLockedMicros.r );
    assert.lt( 0, o.lockStats.timeLockedMicros.w );
    assert.eq( 0, o.lockStats.timeAcquiringMicros.r );
    //assert.lt( 0, o.lockStats.timeAcquiringMicros.w );    // Removed due to SERVER-8331

    // check read lock stats are set
    t.find();
    o = lastOp();
    assert.eq('query', o.op);
    assert.lt( 0, o.lockStats.timeLockedMicros.r );
    assert.eq( 0, o.lockStats.timeLockedMicros.w );
    //assert.lt( 0, o.lockStats.timeAcquiringMicros.r );    // Removed due to SERVER-8331
    //assert.lt( 0, o.lockStats.timeAcquiringMicros.w );    // Removed due to SERVER-8331

    t.save( {} );
    t.save( {} );
    t.find().skip( 1 ).limit( 4 ).itcount();
    checkLastOp( [ [ "ntoreturn", 4 ],
                  [ "ntoskip", 1 ],
                  [ "nscanned", 3 ],
                  [ "nreturned", 2 ] ] );

    t.find().batchSize( 2 ).next();
    o = lastOp();
    assert.lt( 0, o.cursorid );
    
    t.find( {a:1} ).itcount();
    checkLastOp( [ [ "query", {a:1} ] ] );
    
    t.find( {_id:0} ).itcount();
    checkLastOp( [ [ "idhack", true ] ] );
    
    t.find().sort( {a:1} ).itcount();
    checkLastOp( [ [ "scanAndOrder", true ] ] );
    
    db.setProfilingLevel(0);
    db.system.profile.drop();    
}
finally {
    db.setProfilingLevel(0);
    db = stddb;
}
