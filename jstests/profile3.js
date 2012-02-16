
// special db so that it can be run in parallel tests
var stddb = db;
var db = db.getSisterDB("profile3");

t = db.profile3;
t.drop();

try {
    db.setProfilingLevel(0);

    db.system.profile.drop();
    assert.eq( 0 , db.system.profile.count() )
    
    db.setProfilingLevel(2);
    
    t.insert( { x : 1 } );
    t.findOne( { x : 1 } );
    t.find( { x : 1 } ).count();
    t.update( { x : 1 }, {$inc:{a:1}} );
    t.update( { x : 1 }, {$inc:{a:1}} );
    t.update( { x : 0 }, {$inc:{a:1}} );
    
    db.system.profile.find().forEach( printjson )

    db.setProfilingLevel(0);


    assert.eq(db.system.profile.count({nupdated: {$exists:1}}), 3)
    assert.eq(db.system.profile.count({nupdated: 1}), 2)
    assert.eq(db.system.profile.count({nupdated: 0}), 1)
    assert.eq(db.system.profile.count({nmoved: 1}), 1)

    db.system.profile.drop();

}
finally {
    db.setProfilingLevel(0);
    db = stddb;
}
    
