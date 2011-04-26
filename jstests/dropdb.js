// Test that a db does not exist after it is dropped.
// Disabled in the small oplog suite because the slave may create a master db
// with the same name as the dropped db when requesting a clone.

m = db.getMongo();
baseName = "jstests_dropdb";
ddb = db.getSisterDB( baseName );

ddb.c.save( {} );
ddb.getLastError();
assert.neq( -1, m.getDBNames().indexOf( baseName ) );

ddb.dropDatabase();
assert.eq( -1, m.getDBNames().indexOf( baseName ) );

ddb.dropDatabase();
assert.eq( -1, m.getDBNames().indexOf( baseName ) );
