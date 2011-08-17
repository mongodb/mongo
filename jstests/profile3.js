
t = db.profile2;
t.drop();

try {
    db.setProfilingLevel(0);

    db.system.profile.drop();
    assert.eq( 0 , db.system.profile.count() )
    
    db.setProfilingLevel(2);
    
    t.insert( { x : 1 } );
    t.findOne( { x : 1 } );
    t.find( { x : 1 } ).count();
    
    db.system.profile.find().forEach( printjson )

    db.setProfilingLevel(0);
    db.system.profile.drop();

}
finally {
    db.setProfilingLevel(0);
}
    
