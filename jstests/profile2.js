print("profile2.js BEGIN");

// special db so that it can be run in parallel tests
var stddb = db;
var db = db.getSisterDB("profile2");

try {

    assert.commandWorked( db.runCommand( {profile:2} ) );

    var str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    huge = str;
    while (huge.length < 2*1024*1024){
        huge += str;
    }

    db.profile2.count({huge:huge}) // would make a huge entry in db.system.profile

    print("profile2.js SUCCESS OK");
    
} finally {
    // disable profiling for subsequent tests
    assert.commandWorked( db.runCommand( {profile:0} ) );
    db = stddb;
}
