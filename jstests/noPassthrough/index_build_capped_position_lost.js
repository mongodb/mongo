/**
 * Capped cursors return CappedPositionLost when the document they were positioned on gets deleted.
 * When this occurs during the collection scan phase of an index build, it will get restarted.
 *
 * @tags: [
 *   requires_document_locking,
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const conn = MongoRunner.runMongod({});

const dbName = "test";
const collName = "index_build_capped_position_lost";

const db = conn.getDB(dbName);
assert.commandWorked(
    db.createCollection(collName, {capped: true, size: 1024 * 1024 * 1024, max: 5}));

const coll = db.getCollection(collName);

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({i: i}));
}

// Hang the collection scan phase of the index build when it's halfway finished.
assert.commandWorked(conn.adminCommand(
    {configureFailPoint: "hangAfterIndexBuildOf", mode: "alwaysOn", data: {i: 2}}));

const awaitCreateIndex = IndexBuildTest.startIndexBuild(conn, coll.getFullName(), {i: 1});
checkLog.contains(conn, "Hanging after index build of i=2");

// Rollover the capped collection.
for (let i = 5; i < 10; i++) {
    assert.commandWorked(coll.insert({i: i}));
}

assert.commandWorked(conn.adminCommand({configureFailPoint: "hangAfterIndexBuildOf", mode: "off"}));

checkLog.contains(
    conn,
    "index build: collection scan restarting due to 'CollectionScan died due to position in capped collection being deleted.");
checkLog.contains(conn, "index build: collection scan done. scanned 5 total records");

awaitCreateIndex();

IndexBuildTest.assertIndexes(coll, 2, ["_id_", "i_1"]);

MongoRunner.stopMongod(conn);
}());