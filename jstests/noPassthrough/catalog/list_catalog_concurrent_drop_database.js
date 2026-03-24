/**
 * Tests that collectionless $listCatalog does not crash when a database is dropped concurrently
 * between its two internal catalog reads to get the namespaces in system.views.
 * @tags: [
 *   uses_parallel_shell,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const conn = MongoRunner.runMongod({});
const testDB = conn.getDB(jsTestName());
const adminDB = conn.getDB("admin");

// Create a DB with a view so system.views exists.
assert.commandWorked(testDB.createCollection("baseColl"));
assert.commandWorked(testDB.createView("myView", "baseColl", [{$match: {x: 1}}]));

// Pause $listCatalog after its first catalog read.
const fp = configureFailPoint(conn, "hangAfterFirstListCatalogRead");

// Start $listCatalog in a parallel shell — it will hang at the failpoint
const awaitListCatalog = startParallelShell(function () {
    const result = db
        .getSiblingDB("admin")
        .aggregate([{$listCatalog: {}}])
        .toArray();
}, conn.port);

fp.wait();

// Drop the database while $listCatalog is paused between its two reads
assert.commandWorked(testDB.dropDatabase());

fp.off();
awaitListCatalog();

MongoRunner.stopMongod(conn);
