/**
 * Checks that the compact command exits cleanly on EBUSY.
 *
 * @tags: [requires_wiredtiger, requires_persistence]
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod({});
const db = conn.getDB("test");
const coll = db.getCollection(jsTest.name());

for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

const failPoints = ["WTCompactRecordStoreEBUSY", "WTCompactIndexEBUSY"];
for (const failPoint of failPoints) {
    assert.commandWorked(db.adminCommand({configureFailPoint: failPoint, mode: "alwaysOn"}));
    assert.commandFailedWithCode(db.runCommand({compact: jsTest.name()}), ErrorCodes.Interrupted);
    assert.commandWorked(db.adminCommand({configureFailPoint: failPoint, mode: "off"}));
}

MongoRunner.stopMongod(conn);
}());
