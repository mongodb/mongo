print("profile1.js BEGIN");

// special db so that it can be run in parallel tests
var stddb = db;
var db = db.getSisterDB("profile1");

try {

    username = "jstests_profile1_user";
    db.addUser( username, "password" );
    db.auth( username, "password" );

    function profileCursor( query ) {
        query = query || {};
        Object.extend( query, { user:username } );
        return db.system.profile.find( query );
    }
    
    function getProfileAString() {
        var s = "\n";
        profileCursor().forEach( function(z){
            s += tojson( z ) + " ,\n" ;
        } );
        return s;
    }

    /* With pre-created system.profile (capped) */
    db.runCommand({profile: 0});
    db.getCollection("system.profile").drop();
    assert(!db.getLastError(), "Z");
    assert.eq(0, db.runCommand({profile: -1}).was, "A");
    
    db.createCollection("system.profile", {capped: true, size: 10000});
    db.runCommand({profile: 2});
    assert.eq(2, db.runCommand({profile: -1}).was, "B");
    assert.eq(1, db.system.profile.stats().capped, "C");
    var capped_size = db.system.profile.storageSize();
    assert.gt(capped_size, 9999, "D");
    assert.lt(capped_size, 20000, "E");
    
    db.foo.findOne()
    
    assert.eq( 4 , profileCursor().count() , "E2" );
    
    /* Make sure we can't drop if profiling is still on */
    assert.throws( function(z){ db.getCollection("system.profile").drop(); } )

    /* With pre-created system.profile (un-capped) */
    db.runCommand({profile: 0});
    db.getCollection("system.profile").drop();
    assert.eq(0, db.runCommand({profile: -1}).was, "F");
    
    db.createCollection("system.profile");
    db.runCommand({profile: 2});
    assert.eq(2, db.runCommand({profile: -1}).was, "G");
    assert.eq(null, db.system.profile.stats().capped, "G1");
    
    /* With no system.profile collection */
    db.runCommand({profile: 0});
    db.getCollection("system.profile").drop();
    assert.eq(0, db.runCommand({profile: -1}).was, "H");
    
    db.runCommand({profile: 2});
    assert.eq(2, db.runCommand({profile: -1}).was, "I");
    assert.eq(1, db.system.profile.stats().capped, "J");
    var auto_size = db.system.profile.storageSize();
    assert.gt(auto_size, capped_size, "K");
    

    db.eval("sleep(1)") // pre-load system.js

    function resetProfile( level , slowms ) {
        db.setProfilingLevel(0);
        db.system.profile.drop();
        db.setProfilingLevel(level,slowms);
    }

    resetProfile(2);

    db.eval( "sleep(25)" )
    db.eval( "sleep(120)" )
    
    assert.eq( 2 , profileCursor( { "command.$eval" : /^sleep/ } ).count() );

    assert.lte( 119 , profileCursor( { "command.$eval" : "sleep(120)" } )[0].millis );
    assert.lte( 24 , profileCursor( { "command.$eval" : "sleep(25)" } )[0].millis );

    /* sleep() could be inaccurate on certain platforms.  let's check */
    print("\nsleep 2 time actual:");
    for (var i = 0; i < 4; i++) {
        print(db.eval("var x = new Date(); sleep(2); return new Date() - x;"));
    }
    print();
    print("\nsleep 20 times actual:");
    for (var i = 0; i < 4; i++) {
        print(db.eval("var x = new Date(); sleep(20); return new Date() - x;"));
    }
    print();
    print("\nsleep 120 times actual:");
    for (var i = 0; i < 4; i++) {
        print(db.eval("var x = new Date(); sleep(120); return new Date() - x;"));
    }
    print();

    function evalSleepMoreThan(millis,max){
        var start = new Date();
        db.eval("sleep("+millis+")");
        var end = new Date();
        var actual = end.getTime() - start.getTime();
        if ( actual > ( millis + 5 ) ) {
            print( "warning wanted to sleep for: " + millis + " but took: " + actual );
        }
        return actual >= max ? 1 : 0;
    }

    resetProfile(1,100);
    var delta = 0;
    delta += evalSleepMoreThan( 15 , 100 );
    delta += evalSleepMoreThan( 120 , 100 );
    assert.eq( delta , profileCursor( { "command.$eval" : /^sleep/ } ).count() , "X2 : " + getProfileAString() )

    resetProfile(1,20);
    delta = 0;
    delta += evalSleepMoreThan( 5 , 20 );
    delta += evalSleepMoreThan( 120 , 20 );
    assert.eq( delta , profileCursor( { "command.$eval" : /^sleep/ } ).count() , "X3 : " + getProfileAString() )
        
    resetProfile(2);
    db.profile1.drop();
    var q = { _id : 5 };
    var u = { $inc : { x : 1 } };
    db.profile1.update( q , u );
    var r = profileCursor( { ns : db.profile1.getFullName() } ).sort( { $natural : -1 } )[0]
    assert.eq( q , r.query , "Y1: " + tojson(r) );
    assert.eq( u , r.updateobj , "Y2" );
    assert.eq( "update" , r.op , "Y3" );
    assert.eq("profile1.profile1", r.ns, "Y4");

    print("profile1.js SUCCESS OK");
    
} finally {
    // disable profiling for subsequent tests
    assert.commandWorked( db.runCommand( {profile:0} ) );
    db = stddb;
}
