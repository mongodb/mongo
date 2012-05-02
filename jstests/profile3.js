
// special db so that it can be run in parallel tests
var stddb = db;
var db = db.getSisterDB("profile3");

t = db.profile3;
t.drop();

function profileCursor( query ) {
    query = query || {};
    Object.extend( query, { user:username } );
    return db.system.profile.find( query );
}

try {
    username = "jstests_profile3_user";
    db.addUser( username, "password" );
    db.auth( username, "password" );
    
    db.setProfilingLevel(0);

    db.system.profile.drop();
    assert.eq( 0 , profileCursor().count() )
    
    db.setProfilingLevel(2);
    
    t.insert( { x : 1 } );
    t.findOne( { x : 1 } );
    t.find( { x : 1 } ).count();
    t.update( { x : 1 }, {$inc:{a:1}} );
    t.update( { x : 1 }, {$inc:{a:1}} );
    t.update( { x : 0 }, {$inc:{a:1}} );
    
    profileCursor().forEach( printjson )

    db.setProfilingLevel(0);


    assert.eq(profileCursor({nupdated: {$exists:1}}).count(), 3)
    assert.eq(profileCursor({nupdated: 1}).count(), 2)
    assert.eq(profileCursor({nupdated: 0}).count(), 1)
    assert.eq(profileCursor({nmoved: 1}).count(), 1)

    db.system.profile.drop();

}
finally {
    db.setProfilingLevel(0);
    db = stddb;
}
    
