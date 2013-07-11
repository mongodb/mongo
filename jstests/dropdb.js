// Test that a db does not exist after it is dropped.
// Disabled in the small oplog suite because the slave may create a master db
// with the same name as the dropped db when requesting a clone.

m = db.getMongo();
baseName = "jstests_dropdb";
ddb = db.getSisterDB( baseName );

print("initial dbs: " + tojson(m.getDBNames()));

function check(shouldExist) {
    var dbs = m.getDBNames();
    assert.eq(Array.contains(dbs, baseName), shouldExist,
              "DB " + baseName + " should " + (shouldExist ? "" : "not ") + "exist."
              + " dbs: " + tojson(dbs));
}

ddb.c.save( {} );
ddb.getLastError();
check(true);

ddb.dropDatabase();
check(false);

ddb.dropDatabase();
check(false);
