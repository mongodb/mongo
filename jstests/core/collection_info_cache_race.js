// Show case of initialized collection_info_cache crashing update

// Create collection without an index, then try to save a doc.
var coll = db.collection_info_cache_race;
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {autoIndexId: false}));
// Fails when SERVER-16502 was not fixed, due to invariant
assert.writeOK(coll.save({_id: false}, {writeConcern: {w: 1}}));

coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {autoIndexId: false}));
assert.eq(null, coll.findOne());
assert.writeOK(coll.save({_id: false}, {writeConcern: {w: 1}}));
