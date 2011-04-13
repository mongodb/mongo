m = db.getMongo();
baseName = "jstests_dropdb";
ddb = db.getSisterDB( baseName );

ddb.c.save( {} );
assert.neq( -1, m.getDBNames().indexOf( baseName ) );

ddb.dropDatabase();
assert.eq( -1, m.getDBNames().indexOf( baseName ) );

ddb.dropDatabase();
assert.eq( -1, m.getDBNames().indexOf( baseName ) );
