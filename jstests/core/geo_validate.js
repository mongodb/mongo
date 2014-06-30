//
// Test to make sure that invalid geo options are caught
//

var coll = db.geo_validate;
coll.drop();

coll.ensureIndex({ geo : "2dsphere" });

//
//
// Make sure we can't do a $within search with an invalid circular region
assert.throws(function(){
   coll.findOne({ geo : { $within : { $center : [[0, 0], -1] } } });
});
assert.throws(function(){
   coll.findOne({ geo : { $within : { $centerSphere : [[0, 0], -1] } } });
});

//
//
// Make sure we can do a $within search with a zero-radius circular region
assert.writeOK(coll.insert({ geo : [0, 0] }));
assert.neq(null, coll.findOne({ geo : { $within : { $center : [[0, 0], 0] } } }));
assert.neq(null, coll.findOne({ geo : { $within : { $centerSphere : [[0, 0], 0] } } }));

//
//
// Make sure we can't do a $near search with an invalid circular region
assert.throws(function(){
   coll.findOne({ geo : { $geoNear : [0, 0, -1] } });
});
assert.throws(function(){
    coll.findOne({ geo : { $geoNear : [0, 0], $maxDistance : -1 } });
});

//
//
// Make sure we can't do a near search with a negative limit
assert.commandFailed(db.runCommand({geoNear: coll.getName(),
                                    near: [0,0], spherical: true, num: -1}));
