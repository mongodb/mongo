
try {

/* With pre-created system.profile (capped) */
db.runCommand({profile: 0});
db.getCollection("system.profile").drop();
assert(!db.getLastError(), "Z");
assert.eq(0, db.runCommand({profile: -1}).was, "A");

db.createCollection("system.profile", {capped: true, size: 1000});
db.runCommand({profile: 2});
assert.eq(2, db.runCommand({profile: -1}).was, "B");
assert.eq(1, db.system.profile.stats().capped, "C");
var capped_size = db.system.profile.storageSize();
assert.gt(capped_size, 999, "D");
assert.lt(capped_size, 2000, "E");

db.foo.findOne()

assert.eq( 4 , db.system.profile.find().count() , "E2" );

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

} finally {
    // disable profiling for subsequent tests
    assert.commandWorked( db.runCommand( {profile:0} ) );
}