//
// Ensures spherical queries report invalid latitude values in points and center positions
//

t = db.geooobsphere;
t.drop();

t.insert({loc: {x: 30, y: 89}});
t.insert({loc: {x: 30, y: 89}});
t.insert({loc: {x: 30, y: 89}});
t.insert({loc: {x: 30, y: 89}});
t.insert({loc: {x: 30, y: 89}});
t.insert({loc: {x: 30, y: 89}});
t.insert({loc: {x: 30, y: 91}});

assert.commandWorked(t.ensureIndex({loc: "2d"}));

assert.throws(function() {
    t.find({loc: {$nearSphere: [30, 91], $maxDistance: 0.25}}).count();
});

// TODO: SERVER-9986 - it's not clear that throwing is correct behavior here
// assert.throws( function() { t.find({ loc : { $nearSphere : [ 30, 89 ], $maxDistance : 0.25 }
// }).count() } );

assert.throws(function() {
    t.find({loc: {$within: {$centerSphere: [[-180, -91], 0.25]}}}).count();
});

var res;
res =
    db.runCommand({geoNear: "geooobsphere", near: [179, -91], maxDistance: 0.25, spherical: true});
assert.commandFailed(res);
printjson(res);

// TODO: SERVER-9986 - it's not clear that throwing is correct behavior here
// res = db.runCommand({ geoNear : "geooobsphere", near : [30, 89], maxDistance : 0.25, spherical :
// true })
// assert.commandFailed( res )
// printjson( res )
