/**
 * Tests that views properly reject queries in legacy read mode, and reject writes performed in
 * legacy write mode.
 *
 * TODO(SERVER-25641): If the views test suite is moved under core, we can get rid of this test
 * after ensuring that it is included in a legacy passthrough suite.
 */
(function() {
    "use strict";

    let conn = MongoRunner.runMongod({});

    let viewsDB = conn.getDB("views_legacy");
    assert.commandWorked(viewsDB.dropDatabase());
    assert.commandWorked(viewsDB.createView("view", "collection", []));

    // Helper function for performing getLastError.
    function assertGetLastErrorFailed() {
        let gle = viewsDB.runCommand({getLastError: 1});
        assert.commandWorked(gle);
        assert.eq(gle.code, ErrorCodes.CommandNotSupportedOnView, tojson(gle));
    }

    // A view should reject all write CRUD operations performed in legacy write mode.
    conn.forceWriteMode("legacy");

    viewsDB.view.insert({x: 1});
    assertGetLastErrorFailed();

    viewsDB.view.remove({x: 1});
    assertGetLastErrorFailed();

    viewsDB.view.update({x: 1}, {x: 2});
    assertGetLastErrorFailed();

    // Legacy find is explicitly prohibited on views; you must use the find command.
    conn.forceReadMode("legacy");
    let res = assert.throws(function() {
        viewsDB.view.find({x: 1}).toArray();
    });
    assert.eq(res.code, ErrorCodes.CommandNotSupportedOnView, tojson(res));
}());
