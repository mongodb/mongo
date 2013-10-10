// SERVER-11125
// should be able to use any integer value for slowms

var origDb = db;
var db = db.getSisterDB("profile_slowms");

var origStatus = db.getProfilingStatus();
var origLevel = origStatus.was;
var origSlowms = origStatus.slowms;

try {
    // profile level is not the focus of this test
    var level = 2;

    // setProfilingLevel() should accept all these values for slowms
    var slowms_list = [ 100, 1, 0, -1, -100 ];

    // set slowms and confirm using getProfilingStatus()
    for ( var i = 0; i < slowms_list.length; i++ ) {
        var slowms = slowms_list[i];
        var result = db.setProfilingLevel( level, slowms );
        assert( result.ok );

        var status = db.getProfilingStatus();
        assert.eq( status.slowms, slowms, "failed to set slowms to " + slowms );
    }

    // explicit test for slowms = null
    var slowms = db.getProfilingStatus().slowms;
    var result = db.setProfilingLevel( level, null );
    assert( result.ok );
    assert.eq( db.getProfilingStatus().slowms, slowms, "setting slowms = null should leave slowms unchanged" );
} catch ( e ) {
    assert( false, "Caught exception: " + tojson( e ) );    
} finally {
    db.setProfilingLevel( origLevel, origSlowms );
    db = origDb;
}

