// dumprestore4.js -- see SERVER-2186

// The point of this test is to ensure that mongorestore successfully
// constructs indexes when the database being restored into has a
// different name than the database dumped from.  There are 2
// issues here: (1) if you dumped from database "A" and restore into
// database "B", B should have exactly the right indexes; (2) if for
// some reason you have another database called "A" at the time of the
// restore, mongorestore shouldn't touch it.

t = new ToolTest("dumprestore4");

c = t.startDB("dumprestore4");

db = t.db;

dbname = db.getName();
dbname2 = "NOT_" + dbname;

db2 = db.getSisterDB(dbname2);

db.dropDatabase();   // make sure it's empty
db2.dropDatabase();  // make sure everybody's empty

assert.eq(0, c.getIndexes().length, "setup1");
c.ensureIndex({x: 1});
assert.eq(2, c.getIndexes().length, "setup2");  // _id and x_1

assert.eq(0, t.runTool("dump", "-d", dbname, "--out", t.ext), "dump");

// to ensure issue (2), we have to clear out the first db.
// By inspection, db.dropIndexes() doesn't get rid of the _id index on c,
// so we have to drop the collection.
c.drop();
assert.eq(0, t.runTool("restore", "--dir", t.ext + "/" + dbname, "-d", dbname2), "restore");

// issue (1)
assert.eq(2, db2.dumprestore4.getIndexes().length, "after restore 1");
// issue (2)
assert.eq(0, db.dumprestore4.getIndexes().length, "after restore 2");

t.stop();
