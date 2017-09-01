/**
 * Tests that shellHelper.use() updates the global 'db' object.
 */

// We explicitly declare the global 'db' object since the rest of the test runs with strict-mode
// enabled.
var db;

(function() {
    "use strict";

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");

    db = conn.getDB("db1");
    assert.eq("db1", db.getName());

    // Tests that shellHelper.use() updates the global 'db' object to refer to a DB object with the
    // database name specified.
    shellHelper.use("db2");
    assert.eq("db2", db.getName());

    // Replace the global 'db' object with a DB object from a new session and verify that
    // shellHelper.use() still works.
    db = conn.startSession().getDatabase("db1");
    assert.eq("db1", db.getName());

    const session = db.getSession();

    // Tests that shellHelper.use() updates the global 'db' object to refer to a DB object with the
    // database name specified. The DB objects should have the same underlying DriverSession object.
    shellHelper.use("db2");
    assert.eq("db2", db.getName());

    assert(session === db.getSession(), "session wasn't inherited as part of switching databases");

    session.endSession();
    MongoRunner.stopMongod(conn);
})();
