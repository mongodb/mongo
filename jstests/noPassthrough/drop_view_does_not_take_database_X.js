/**
 * Test that drop view only takes database IX lock.
 *
 * @tags: [requires_db_locking]
 */

(function() {
    "use strict";
    load("jstests/libs/check_log.js");

    const conn = MongoRunner.runMongod({});
    const db = conn.getDB("test");

    assert.commandWorked(db.runCommand({insert: "a", documents: [{x: 1}]}));
    assert.commandWorked(db.createView("view", "a", []));

    assert.commandWorked(
        db.adminCommand({configureFailPoint: "hangDuringDropCollection", mode: "alwaysOn"}));

    // This only holds a database IX lock.
    const awaitDrop =
        startParallelShell(() => assert(db.getSiblingDB("test")["view"].drop()), conn.port);
    checkLog.contains(conn, "hangDuringDropCollection fail point enabled");

    // This takes a database IX lock and should not be blocked.
    assert.commandWorked(db.runCommand({insert: "a", documents: [{y: 1}]}));

    assert.commandWorked(
        db.adminCommand({configureFailPoint: "hangDuringDropCollection", mode: "off"}));

    awaitDrop();
    MongoRunner.stopMongod(conn);
})();
