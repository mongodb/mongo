/**
 * Tests bulk write command profiling outputs contain expected metrics.
 *
 * The test runs commands that are not allowed with security token: bulkWrite.
 * @tags: [
 *   not_allowed_with_security_token,
 *   command_not_supported_in_serverless,
 *   # TODO SERVER-52419 Remove this tag.
 *   featureFlagBulkWriteCommand,
 *   # TODO SERVER-79506 Remove this tag.
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   requires_non_retryable_writes,
 *   requires_profiling,
 *   assumes_no_implicit_index_creation,
 * ]
 */

import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {isLinux} from "jstests/libs/os_helpers.js";
import {getNLatestProfilerEntries} from "jstests/libs/profiler.js";

var testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const coll1 = testDB.getCollection("test_1");
const coll2 = testDB.getCollection("test_2");
const isClustered = ClusteredCollectionUtil.areAllCollectionsClustered(testDB);

for (var i = 0; i < 10; ++i) {
    assert.commandWorked(coll2.insert({a: i}));
}
assert.commandWorked(coll2.createIndex({a: 1}));

testDB.setProfilingLevel(1, {filter: {ns: {$in: [coll1.getFullName(), coll2.getFullName()]}}});

assert.commandWorked(testDB.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 1, document: {a: 10}},
        {insert: 1, document: {a: 11}},
        {insert: 1, document: {a: 12}},
        {insert: 0, document: {b: 1}},
        {
            update: 1,
            filter: {a: {$lt: 4}},
            updateMods: {$push: {b: "mdb"}},
            multi: true,
            upsert: false
        },
        {delete: 1, filter: {a: {$gte: 8}}, multi: true, hint: {a: 1}}
    ],
    nsInfo: [{ns: coll1.getFullName()}, {ns: coll2.getFullName()}]
}));

jsTestLog("Verifying bulkWrite command profiling outputs");

const profileEntries = getNLatestProfilerEntries(testDB, 4).reverse();
jsTestLog(`BulkWrite profiling outputs: ${tojson(profileEntries)}`);

assert.eq(profileEntries.length, 4);

// Verify first insert profile output.
var profileInsert = profileEntries[0];
assert.eq(profileInsert.ns, coll2.getFullName(), tojson(profileInsert));
assert.eq(profileInsert.op, "bulkWrite", tojson(profileInsert));
assert.eq(profileInsert.command.insert, 1, tojson(profileInsert));
assert.eq(profileInsert.command.documents, [{a: 10}, {a: 11}, {a: 12}], tojson(profileInsert));
assert.eq(profileInsert.ninserted, 3, tojson(profileInsert));
assert.eq(profileInsert.keysInserted, isClustered ? 3 : 3 * 2, tojson(profileInsert));
assert(profileInsert.hasOwnProperty("numYield"), tojson(profileInsert));
assert(profileInsert.hasOwnProperty("locks"), tojson(profileInsert));
assert(profileInsert.hasOwnProperty("millis"), tojson(profileInsert));
assert(profileInsert.hasOwnProperty("ts"), tojson(profileInsert));
assert(profileInsert.hasOwnProperty("client"), tojson(profileInsert));
if (isLinux()) {
    assert(profileInsert.hasOwnProperty("cpuNanos"), tojson(profileInsert));
}
assert.eq(profileInsert.appName, "MongoDB Shell", tojson(profileInsert));

// Verify second insert profile output.
var profileInsert = profileEntries[1];
assert.eq(profileInsert.ns, coll1.getFullName(), tojson(profileInsert));
assert.eq(profileInsert.op, "bulkWrite", tojson(profileInsert));
assert.eq(profileInsert.command.insert, 0, tojson(profileInsert));
assert.eq(profileInsert.command.documents, [{b: 1}], tojson(profileInsert));
assert.eq(profileInsert.ninserted, 1, tojson(profileInsert));
assert.eq(profileInsert.keysInserted, isClustered ? 0 : 1, tojson(profileInsert));
assert(profileInsert.hasOwnProperty("numYield"), tojson(profileInsert));
assert(profileInsert.hasOwnProperty("locks"), tojson(profileInsert));
assert(profileInsert.hasOwnProperty("millis"), tojson(profileInsert));
assert(profileInsert.hasOwnProperty("ts"), tojson(profileInsert));
assert(profileInsert.hasOwnProperty("client"), tojson(profileInsert));
if (isLinux()) {
    assert(profileInsert.hasOwnProperty("cpuNanos"), tojson(profileInsert));
}
assert.eq(profileInsert.appName, "MongoDB Shell", tojson(profileInsert));

// Verify update profile output.
var profileUpdate = profileEntries[2];
assert.eq(profileUpdate.ns, coll2.getFullName(), tojson(profileUpdate));
assert.eq(profileUpdate.op, "bulkWrite", tojson(profileUpdate));
assert.eq(profileUpdate.command.update, 1, tojson(profileUpdate));
assert.eq(profileUpdate.command.filter, {a: {$lt: 4}}, tojson(profileUpdate));
assert.eq(profileUpdate.command.updateMods, {$push: {b: "mdb"}}, tojson(profileUpdate));
assert.eq(profileUpdate.command.multi, true, tojson(profileUpdate));
assert.eq(profileUpdate.command.upsert, false, tojson(profileUpdate));
assert.eq(profileUpdate.keysExamined, 4, tojson(profileUpdate));
assert.eq(profileUpdate.docsExamined, 4, tojson(profileUpdate));
assert.eq(profileUpdate.keysInserted, 0, tojson(profileUpdate));
assert.eq(profileUpdate.keysDeleted, 0, tojson(profileUpdate));
assert.eq(profileUpdate.nMatched, 4, tojson(profileUpdate));
assert.eq(profileUpdate.nModified, 4, tojson(profileUpdate));
assert.eq(profileUpdate.planSummary, "IXSCAN { a: 1 }", tojson(profileUpdate));
assert(profileUpdate.execStats.hasOwnProperty("stage"), tojson(profileUpdate));
assert(profileUpdate.hasOwnProperty("millis"), tojson(profileUpdate));
assert(profileUpdate.hasOwnProperty("numYield"), tojson(profileUpdate));
assert(profileUpdate.hasOwnProperty("locks"), tojson(profileUpdate));
if (isLinux()) {
    assert(profileUpdate.hasOwnProperty("cpuNanos"), tojson(profileInsert));
}
assert.eq(profileUpdate.appName, "MongoDB Shell", tojson(profileUpdate));

// Verify delete profile output.
var profileDelete = profileEntries[3];
assert.eq(profileDelete.ns, coll2.getFullName(), tojson(profileDelete));
assert.eq(profileDelete.op, "bulkWrite", tojson(profileDelete));
assert.eq(profileDelete.command.delete, 1, tojson(profileDelete));
assert.eq(profileDelete.command.filter, {a: {$gte: 8}}, tojson(profileDelete));
assert.eq(profileDelete.command.multi, true, tojson(profileDelete));
assert.eq(profileDelete.command.hint, {a: 1}, tojson(profileDelete));
assert.eq(profileDelete.keysExamined, 5, tojson(profileDelete));
assert.eq(profileDelete.docsExamined, 5, tojson(profileDelete));
assert.eq(profileDelete.ndeleted, 5, tojson(profileDelete));
assert.eq(profileDelete.keysDeleted, isClustered ? 5 : 5 * 2, tojson(profileDelete));
assert.eq(profileDelete.planSummary, "IXSCAN { a: 1 }", tojson(profileDelete));
assert(profileDelete.execStats.hasOwnProperty("stage"), tojson(profileDelete));
assert(profileDelete.hasOwnProperty("millis"), tojson(profileDelete));
assert(profileDelete.hasOwnProperty("numYield"), tojson(profileDelete));
assert(profileDelete.hasOwnProperty("locks"), tojson(profileDelete));
if (isLinux()) {
    assert(profileDelete.hasOwnProperty("cpuNanos"), tojson(profileDelete));
}
assert.eq(profileDelete.appName, "MongoDB Shell", tojson(profileDelete));
