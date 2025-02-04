// Confirms that profiled insert execution contains all expected metrics with proper values.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   # Asserts on the number of index keys inserted.
//   assumes_no_implicit_index_creation,
//   assumes_write_concern_unchanged,
//   does_not_support_stepdowns,
//   requires_fcv_70,
//   requires_profiling,
//   # The test runs getLatestProfileEntry(). The downstream syncing node affects the profiler.
//   run_getLatestProfilerEntry,
// ]

import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {isLinux} from "jstests/libs/os_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

var testDB = db.getSiblingDB("profile_insert");
assert.commandWorked(testDB.dropDatabase());
const collName = jsTestName();
var coll = testDB.getCollection(collName);

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(testDB.setProfilingLevel(
    1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));

//
// Test single insert.
//
var doc = {_id: 1};
assert.commandWorked(coll.insert(doc));

var profileObj = getLatestProfilerEntry(testDB);

const collectionIsClustered = ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo());
// A clustered collection has no actual index on _id.
let expectedKeysInserted = collectionIsClustered ? 0 : 1;

assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.op, "insert", tojson(profileObj));
assert.eq(profileObj.ninserted, 1, tojson(profileObj));
assert.eq(profileObj.keysInserted, expectedKeysInserted, tojson(profileObj));
assert.eq(profileObj.command.ordered, true, tojson(profileObj));
assert.eq(profileObj.protocol, "op_msg", tojson(profileObj));
assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
if (isLinux()) {
    assert(profileObj.hasOwnProperty("cpuNanos"), tojson(profileObj));
}
assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
assert(profileObj.hasOwnProperty("ts"), tojson(profileObj));
assert(profileObj.hasOwnProperty("client"), tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Test multi-insert.
//
coll.drop();

var docArray = [{_id: 1}, {_id: 2}];
var bulk = coll.initializeUnorderedBulkOp();
bulk.insert(docArray[0]);
bulk.insert(docArray[1]);
assert.commandWorked(bulk.execute());

profileObj = getLatestProfilerEntry(testDB);

expectedKeysInserted = collectionIsClustered ? 0 : 2;

assert.eq(profileObj.ninserted, 2, tojson(profileObj));
assert.eq(profileObj.keysInserted, expectedKeysInserted, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Test insert options.
//
coll.drop();
doc = {
    _id: 1
};
var wtimeout = 60000;
assert.commandWorked(coll.insert(doc, {writeConcern: {w: 1, wtimeout: wtimeout}, ordered: false}));

profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.command.ordered, false, tojson(profileObj));
assert.eq(profileObj.command.writeConcern.w, 1, tojson(profileObj));
assert.eq(profileObj.command.writeConcern.wtimeout, wtimeout, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Test that inserts over 'kMaxWriteBatchSize' have the correct 'ninserted'.
//
coll.drop();
let docs = [];
for (let i = 1; i <= 150000; i++) {
    docs.push({number: i});
}

assert.commandWorked(coll.insertMany(docs));
coll.aggregate([{$match: {number: {$lte: 100001}}}, {$out: "col2"}]);

profileObj = getLatestProfilerEntry(testDB);
expectedKeysInserted = collectionIsClustered ? 0 : 100001;

assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.ninserted, 100001, tojson(profileObj));
assert.eq(profileObj.keysInserted, expectedKeysInserted, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));
