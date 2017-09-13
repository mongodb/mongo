// Test that a db does not exist after it is dropped.
// Disabled in the small oplog suite because the slave may create a master db
// with the same name as the dropped db when requesting a clone.

m = db.getMongo();
baseName = "jstests_dropdb";
ddb = db.getSisterDB(baseName);

print("initial dbs: " + tojson(m.getDBNames()));

function check(shouldExist) {
    var dbs = m.getDBNames();
    assert.eq(Array.contains(dbs, baseName),
              shouldExist,
              "DB " + baseName + " should " + (shouldExist ? "" : "not ") + "exist." + " dbs: " +
                  tojson(dbs) + "\n" + tojson(m.getDBs()));
}

ddb.c.save({});
check(true);

var res = ddb.dropDatabase();
assert.commandWorked(res);
assert.eq(res.dropped, baseName, "dropped field did not contain correct database name");
check(false);

var res = ddb.dropDatabase();
assert.commandWorked(res);
assert.eq(res.dropped,
          undefined,
          "dropped field was populated even though nothing should have been dropped");
check(false);
