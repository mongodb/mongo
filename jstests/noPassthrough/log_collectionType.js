/**
 * This test verifies the correctness of the "collectionType" value in the slow query logs.
 * @tags: []
 */

import {findMatchingLogLine} from "jstests/libs/log.js";

(function() {

"use strict";

// Asserts slow query log contains expectedCollType.
function checkLogForCollectionType(ns, expectedCollType, command = "aggregate") {
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const line = findMatchingLogLine(
        globalLog.log,
        {msg: "Slow query", command: command, ns: ns, collectionType: expectedCollType});
    assert(line,
           "Failed to find a log line matching the ns " + ns + " and collectionType " +
               expectedCollType);
}

const conn = MongoRunner.runMongod({setParameter: {}});
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

// Set slow query threshold to -1 so every query gets logged.
db.setProfilingLevel(0, -1);

db.coll.drop();
assert.commandWorked(db.coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));

let ns = "test.coll";
const pipeline = [{$project: {"abc": "def"}}];

// Check for normal collectionType.
db.coll.aggregate(pipeline);
checkLogForCollectionType(ns, "normal");

// Check for view collectionType.
assert.commandWorked(db.createView("viewOnColl", "coll", [{$match: {a: 1}}]));
ns = "test.viewOnColl";
db.viewOnColl.aggregate(pipeline);
checkLogForCollectionType(ns, "view");

// Check for timeseries and timeseriesBuckets collectionType.
assert.commandWorked(db.createCollection(
    "timeseries_coll", {timeseries: {timeField: "timestamp", metaField: "metadata"}}));
assert.commandWorked(db.timeseries_coll.insert({
    "metadata": {"type": "measurement"},
    "timestamp": ISODate("2021-05-18T00:00:00.000Z"),
}));
db.timeseries_coll.aggregate(pipeline);
checkLogForCollectionType("test.timeseries_coll", "timeseries");
db.test.system.buckets.timeseries_coll.aggregate(pipeline);
checkLogForCollectionType("test.system.buckets.timeseries_coll", "timeseriesBuckets");

// Check for system collectionType.
db.system.profile.aggregate(pipeline);
checkLogForCollectionType("test.system.profile", "system");

// Check for admin collectionType.
db.getSiblingDB("admin").setProfilingLevel(0, -1);
db.getSiblingDB("admin").aggregate([{$currentOp: {}}]);
checkLogForCollectionType("admin.$cmd.aggregate", "admin");

// Check for local collectionType.
db.getSiblingDB("local").setProfilingLevel(0, -1);
db.getSiblingDB("local").startup_log.aggregate(pipeline);
checkLogForCollectionType("local.startup_log", "local");

// Check for config collectionType.
db.getSiblingDB("config").setProfilingLevel(0, -1);
db.getSiblingDB("config").aggregate([{'$listLocalSessions': {}}]);
checkLogForCollectionType("config.$cmd.aggregate", "config");

MongoRunner.stopMongod(conn);
})();
