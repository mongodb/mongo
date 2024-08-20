/**
 * Tests write-concern-related bulkWrite protocol functionality
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   # The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_signed_security_token,
 *   command_not_supported_in_serverless,
 *   requires_fcv_80,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Skip this test when running with storage engines other than inMemory, as the test relies on
// journaling not being active.
if (jsTest.options().storageEngine !== "inMemory") {
    jsTest.log("Skipping test because it is only applicable for the inMemory storage engine");
    quit();
}

var request;
var result;

// NOTE: ALL TESTS BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

jsTest.log("Starting no journal/repl set tests...");

// Start a single-node replica set with no journal
// Allows testing immediate write concern failures and wc application failures
var rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
var mongod = rst.getPrimary();
var coll = mongod.getCollection("test.bulk_write_command_wc");

//
// Basic bulkWrite, default WC
coll.remove({});
printjson(request = {
    bulkWrite: 1,
    ops: [{insert: 0, document: {a: 1}}],
    nsInfo: [{ns: "test.bulk_write_command_wc"}]
});
printjson(result = mongod.adminCommand(request));
assert(result.ok);
assert.eq(1, result.cursor.firstBatch[0].n);
assert.eq(1, coll.find().itcount());

//
// Basic bulkWrite, majority WC
coll.remove({});
printjson(request = {
    bulkWrite: 1,
    ops: [{insert: 0, document: {a: 1}}],
    nsInfo: [{ns: "test.bulk_write_command_wc"}],
    writeConcern: {w: 'majority'}
});
printjson(result = mongod.adminCommand(request));
assert(result.ok);
assert.eq(1, result.cursor.firstBatch[0].n);
assert.eq(1, coll.find().itcount());

//
// Basic bulkWrite,  w:2 WC
coll.remove({});
printjson(request = {
    bulkWrite: 1,
    ops: [{insert: 0, document: {a: 1}}],
    nsInfo: [{ns: "test.bulk_write_command_wc"}],
    writeConcern: {w: 2}
});
printjson(result = mongod.adminCommand(request));
assert(result.ok);
assert.eq(1, result.cursor.firstBatch[0].n);
assert.eq(1, coll.find().itcount());

//
// Basic bulkWrite, immediate nojournal error
coll.remove({});
printjson(request = {
    bulkWrite: 1,
    ops: [{insert: 0, document: {a: 1}}],
    nsInfo: [{ns: "test.bulk_write_command_wc"}],
    writeConcern: {j: true}
});
printjson(result = mongod.adminCommand(request));
assert(!result.ok);
assert.eq(0, coll.find().itcount());

//
// Basic bulkWrite, timeout wc error
coll.remove({});
printjson(request = {
    bulkWrite: 1,
    ops: [{insert: 0, document: {a: 1}}],
    nsInfo: [{ns: "test.bulk_write_command_wc"}],
    writeConcern: {w: 3, wtimeout: 1}
});
printjson(result = mongod.adminCommand(request));
assert(result.ok);
assert.eq(1, result.cursor.firstBatch[0].n);
assert(result.writeConcernError);
assert.eq(100, result.writeConcernError.code);
assert.eq(1, coll.find().itcount());

//
// Basic bulkWrite, wmode wc error
coll.remove({});
printjson(request = {
    bulkWrite: 1,
    ops: [{insert: 0, document: {a: 1}}],
    nsInfo: [{ns: "test.bulk_write_command_wc"}],
    writeConcern: {w: 'invalid'}
});
printjson(result = mongod.adminCommand(request));
assert(result.ok);
assert.eq(1, result.cursor.firstBatch[0].n);
assert(result.writeConcernError);
assert.eq(1, coll.find().itcount());

//
// Two ordered inserts, write error and wc error both reported
coll.remove({});
printjson(request = {
    bulkWrite: 1,
    ops: [{insert: 0, document: {a: 1}}, {insert: 0, document: {_id: /a/}}],
    nsInfo: [{ns: "test.bulk_write_command_wc"}],
    writeConcern: {w: 'invalid'}
});
printjson(result = mongod.adminCommand(request));
assert(result.ok);
assert.eq(1, result.cursor.firstBatch[0].n);
assert.eq(0, result.cursor.firstBatch[1].ok);
assert.eq(1, result.cursor.firstBatch[1].idx);
assert(result.writeConcernError);
assert.eq(1, coll.find().itcount());

//
// Two unordered inserts, write error and wc error reported
coll.remove({});
printjson(request = {
    bulkWrite: 1,
    ops: [{insert: 0, document: {a: 1}}, {insert: 0, document: {_id: /a/}}],
    nsInfo: [{ns: "test.bulk_write_command_wc"}],
    ordered: false,
    writeConcern: {w: 'invalid'}
});
printjson(result = mongod.adminCommand(request));
assert(result.ok);
assert.eq(1, result.cursor.firstBatch[0].n);
assert.eq(0, result.cursor.firstBatch[1].ok);
assert.eq(1, result.cursor.firstBatch[1].idx);
assert(result.writeConcernError);
assert.eq(1, coll.find().itcount());

//
// Write error with empty writeConcern object.
coll.remove({});
request = {
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: 1}}, {insert: 0, document: {_id: 1}}],
    nsInfo: [{ns: "test.bulk_write_command_wc"}],
    ordered: false,
    writeConcern: {}
};
result = mongod.adminCommand(request);
assert(result.ok);
assert.eq(1, result.cursor.firstBatch[0].n);
assert.eq(0, result.cursor.firstBatch[1].ok);
assert.eq(1, result.cursor.firstBatch[1].idx);
assert.eq(null, result.writeConcernError);
assert.eq(1, coll.find().itcount());

//
// Write error with unspecified w.
coll.remove({});
request = {
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: 1}}, {insert: 0, document: {_id: 1}}],
    nsInfo: [{ns: "test.bulk_write_command_wc"}],
    ordered: false,
    writeConcern: {wtimeout: 1}
};
result = assert.commandWorkedIgnoringWriteErrors(mongod.adminCommand(request));
assert.eq(1, result.cursor.firstBatch[0].n);
assert.eq(0, result.cursor.firstBatch[1].ok);
assert.eq(1, result.cursor.firstBatch[1].idx);
assert.eq(null, result.writeConcernError);
assert.eq(1, coll.find().itcount());

jsTest.log("DONE no journal/repl tests");
rst.stopSet();
