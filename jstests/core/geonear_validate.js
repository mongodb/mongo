// Test to make sure that geoNear validates numWanted
t = db.geonear_validate
t.drop();
t.ensureIndex({ geo : "2dsphere" })
origin = { "type" : "Point", "coordinates": [ 0, 0] }
t.insert({geo: origin})
res = db.runCommand({geoNear: t.getName(), near: [0,0], spherical: true, num: -1});
assert.eq(0, res.ok);
