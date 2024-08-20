/**
 * Tests TTL indexes with invalid values for 'expireAfterSeconds'.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

const nonIntVal = 10000.7;
const intVal = Math.floor(nonIntVal);

jsTestLog("Testing expireAfterSeconds = " + nonIntVal);

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {votes: 0, priority: 0}}],
    nodeOptions: {setParameter: {ttlMonitorSleepSecs: 5}},
    // Sync from primary only so that we have a well-defined node to check listIndexes behavior.
    settings: {chainingAllowed: false},
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

// Insert a document before creating the index. Index builds on empty collections skip the
// collection scan phase, which we look for using checkLog below.
assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

// First let's test normal behavior after this issue has been resolved. Catalog contents
// should reflect integer values.
assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: nonIntVal}));
const catalog = coll.aggregate([{$listCatalog: {}}]).toArray();
jsTestLog(catalog);
assert.eq(catalog[0].db, db.getName());
assert.eq(catalog[0].name, coll.getName());
assert.eq(catalog[0].md.indexes.length, 2);
assert.eq(catalog[0].md.indexes[0].spec.name, "_id_");
assert.eq(catalog[0].md.indexes[1].spec.expireAfterSeconds, intVal);
assert.commandWorked(coll.dropIndex(catalog[0].md.indexes[1].spec.name));

// The rest of the test cases here revolve around having a TTL index in the catalog with a
// non-integer 'expireAfterSeconds'. The current createIndexes behavior will reject index creation
// for such values of expireAfterSeconds, so we use a failpoint to disable that checking to
// simulate a value leftover from older MongoDB versions.
const fpCreate = configureFailPoint(primary, 'skipTTLIndexExpireAfterSecondsValidation');
try {
    assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: nonIntVal}));
} finally {
    fpCreate.off();
}

// Log the contents of the catalog for debugging purposes in case of failure.
let catalogContents = coll.aggregate([{$listCatalog: {}}]).toArray();
jsTestLog("Catalog contents: " + tojson(catalogContents));

// TTL index should be replicated to the secondary with non-integer 'expireAfterSeconds'.
const secondary = rst.getSecondary();
checkLog.containsJson(secondary, 20384, {
    namespace: coll.getFullName(),
    properties: (spec) => {
        jsTestLog('TTL index on secondary: ' + tojson(spec));
        return spec.expireAfterSeconds == nonIntVal;
    }
});

// Confirm that TTL index is replicated with an integer 'expireAfterSeconds' during initial
// sync.
const newNode = rst.add({rsConfig: {votes: 0, priority: 0}});
rst.reInitiate();
rst.waitForState(newNode, ReplSetTest.State.SECONDARY);
rst.awaitReplication();
let newNodeTestDB = newNode.getDB(db.getName());
let newNodeColl = newNodeTestDB.getCollection(coll.getName());
const newNodeIndexes = IndexBuildTest.assertIndexes(newNodeColl, 2, ['_id_', 't_1']);
const newNodeSpec = newNodeIndexes.t_1;
jsTestLog('TTL index on initial sync node: ' + tojson(newNodeSpec));
assert(newNodeSpec.hasOwnProperty('expireAfterSeconds'),
       'Index was not replicated as a TTL index during initial sync.');
assert.eq(
    newNodeSpec.expireAfterSeconds,
    intVal,
    nonIntVal +
        ' expireAferSeconds was replicated as something other than expected during initial sync.');

jsTestLog('Beginning step down');
// Confirm that a node with an existing TTL index with a non-integer 'expireAfterSeconds' will
// convert the duration on the TTL index from the non-integer value to the associated integer value
// when it becomes the primary node. When stepping down the primary, we use 'force' because there's
// no other electable node.  Subsequently, we wait for the stepped down node to become primary
// again.  To confirm that the TTL index has been fixed, we check the oplog for a collMod
// operation on the TTL index that changes the `expireAfterSeconds` field from the non-integer
// value to the associated integer value.
assert.commandWorked(primary.adminCommand({replSetStepDown: 5, force: true}));
primary = rst.waitForPrimary();

jsTestLog('Found new primary');

// Log the contents of the catalog for debugging purposes in case of failure.
let newPrimaryColl = primary.getDB(db.getName()).getCollection(coll.getName());
const newPrimaryCatalogContents = newPrimaryColl.aggregate([{$listCatalog: {}}]).toArray();
jsTestLog("Catalog contents on new primary: " + tojson(newPrimaryCatalogContents));

assert.soon(
    () => {
        return 1 ===
            rst.findOplog(primary,
                          {
                              op: 'c',
                              ns: newPrimaryColl.getDB().getCollection('$cmd').getFullName(),
                              'o.collMod': coll.getName(),
                              'o.index.name': 't_1',
                              'o.index.expireAfterSeconds': newNodeSpec.expireAfterSeconds
                          },
                          /*limit=*/ 1)
                .toArray()
                .length;
    },
    'TTL index with ' + nonIntVal +
        ' expireAfterSeconds was not fixed using collMod during step-up: ' +
        tojson(rst.findOplog(primary, {op: {$ne: 'n'}}, /*limit=*/ 10).toArray()));

rst.stopSet();
