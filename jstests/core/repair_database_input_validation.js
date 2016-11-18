/**
 * This tests checks that repair database validates the database exists before executing the
 * repair. Particularly when a command is issued on a different casing than a database that does
 * exist.
 * 1.) Drop "repairDB", "repairdb" and "nonExistantDb" database.
 * 2.) Create "repairDB" by inserting a document.
 * 3.) Repair "repairdb", expect an input validation error.
 * 4.) Repair "nonExistantDb", expect an OK.
 * 5.) Using "listDatabases" verify only "repairDB" exists.
 */

(function() {
    "use strict";

    // 1. Drop databases.
    var casing1db = db.getSisterDB("repair_database_input_validation_REPAIRDB");
    casing1db.dropDatabase();
    var casing2db = db.getSisterDB("repair_database_input_validation_repairdb");
    casing2db.dropDatabase();
    var nonExistentDb = db.getSisterDB("repair_database_input_validation_nonExistentDb");
    nonExistentDb.dropDatabase();

    // 2. Create "repairDB".
    casing1db.a.insert({_id: 1, a: "hello world"});

    var foundDoc = casing1db.a.findOne();
    assert.neq(null, foundDoc);
    assert.eq(1, foundDoc._id);

    // 3. Repair "repairdb", verify error.
    var res1 = casing2db.repairDatabase();
    assert.eq(0, res1["ok"]);
    assert.eq(
        true,
        res1["errmsg"].indexOf(
            "Database exists with a different case. Given: `repair_database_input_validation_repairdb` Found: `repair_database_input_validation_REPAIRDB`") >
            -1,
        tojson(res1));

    // 4. Repair "nonExistentDb", verify error.
    assert.commandWorked(nonExistentDb.repairDatabase());

    // 5. Verify only "repairDB" exists.
    var dbNames = db.getMongo().getDBNames();
    assert.contains(
        "repair_database_input_validation_REPAIRDB", dbNames, "Should contain REPAIRDB");
    assert.eq(false, dbNames.some(function(x) {
        return x == "repair_database_input_validation_repairdb";
    }));
    assert.eq(false, dbNames.some(function(x) {
        return x == "repair_database_input_validation_nonExistentDb";
    }));
})();
