/**
 * Ensure that we allow mongod to shutdown cleanly while being fsync locked.
 */
(function() {
"use strict";

let conn = MongoRunner.runMongod();
let db = conn.getDB("test");

for (let i = 0; i < 10; i++) {
    assert.commandWorked(db.adminCommand({fsync: 1, lock: 1}));
}

MongoRunner.stopMongod(conn, MongoRunner.EXIT_CLEAN, {skipValidation: true});
}());
