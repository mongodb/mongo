/**
 * Validate that the 'collMod' command with 'hidden,' 'unique,' or 'prepareUnique' fields
 * will return expected result document for the command and generate expected oplog entries in which
 * the index modifications (hiding/unhiding/convert to unique/allowing duplicates/disallowing
 * duplicates) will be no-ops if no other index option (TTL, for example) is involved.
 *
 * @tags: [
 *  # TODO(SERVER-61182): Fix WiredTigerKVEngine::alterIdentMetadata() under inMemory.
 *  requires_persistence,
 *  requires_replication,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/index_catalog_helpers.js");

const rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: "latest"}});
rst.startSet();
rst.initiate();

const dbName = jsTestName();
const collName = "collmod_index_noop";
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const oplogColl = primary.getDB("local")['oplog.rs'];

const collModIndexUniqueEnabled =
    assert.commandWorked(primary.adminCommand({getParameter: 1, featureFlagCollModIndexUnique: 1}))
        .featureFlagCollModIndexUnique.value;

// Validate that the generated oplog entries filtered by given filter are what we expect.
function validateCollModOplogEntryCount(hiddenFilter, expectedCount) {
    let filter = {
        "ns": `${dbName}.$cmd`,
        "o.collMod": collName,
    };
    filter = Object.assign(filter, hiddenFilter);
    const oplogDocs = oplogColl.find(filter).toArray();
    assert.eq(oplogDocs.length, expectedCount, tojson(oplogDocs));
}

// Validate that the index-related fields in the result document for the 'collMod' command are what
// we expect.
function validateResultForCollMod(result, expectedResult) {
    assert.eq(result.hidden_old, expectedResult.hidden_old, result);
    assert.eq(result.hidden_new, expectedResult.hidden_new, result);
    assert.eq(result.expireAfterSeconds_old, expectedResult.expireAfterSeconds_old, result);
    assert.eq(result.expireAfterSeconds_new, expectedResult.expireAfterSeconds_new, result);
    assert.eq(result.unique_new, expectedResult.unique_new, result);
    assert.eq(result.prepareUnique_old, expectedResult.prepareUnique_old, result);
    assert.eq(result.prepareUnique_new, expectedResult.prepareUnique_new, result);
}

primaryColl.drop();
assert.commandWorked(primaryColl.createIndex({a: 1}));
assert.commandWorked(primaryColl.createIndex({b: 1}, {expireAfterSeconds: 5}));
assert.commandWorked(primaryColl.createIndex({c: 1}));
assert.commandWorked(primaryColl.createIndex({d: 1}, {unique: true}));
assert.commandWorked(primaryColl.createIndex({e: 1}, {hidden: true, unique: true}));
assert.commandWorked(primaryColl.createIndex({f: 1}, {unique: true, expireAfterSeconds: 15}));
assert.commandWorked(
    primaryColl.createIndex({g: 1}, {hidden: true, unique: true, expireAfterSeconds: 25}));
if (collModIndexUniqueEnabled) {
    assert.commandWorked(primaryColl.createIndex({h: 1}, {prepareUnique: true}));
}

// Hiding a non-hidden index will generate the oplog entry with a 'hidden_old: false'.
let result = assert.commandWorked(primaryColl.hideIndex('a_1'));
validateResultForCollMod(result, {hidden_old: false, hidden_new: true});
validateCollModOplogEntryCount({"o.index.hidden": true, "o2.indexOptions_old.hidden": false}, 1);

// Hiding a hidden index won't generate both 'hidden' and 'hidden_old' field as it's a no-op. The
// result for no-op 'collMod' command shouldn't contain 'hidden' field.
result = assert.commandWorked(primaryColl.hideIndex('a_1'));
validateResultForCollMod(result, {});
validateCollModOplogEntryCount({"o.index.hidden": true, "o2.indexOptions_old.hidden": true}, 0);

// Un-hiding an hidden index will generate the oplog entry with a 'hidden_old: true'.
result = assert.commandWorked(primaryColl.unhideIndex('a_1'));
validateResultForCollMod(result, {hidden_old: true, hidden_new: false});
validateCollModOplogEntryCount({"o.index.hidden": false, "o2.indexOptions_old.hidden": true}, 1);

// Un-hiding a non-hidden index won't generate both 'hidden' and 'hidden_old' field as it's a no-op.
// The result for no-op 'collMod' command shouldn't contain 'hidden' field.
result = assert.commandWorked(primaryColl.unhideIndex('a_1'));
validateResultForCollMod(result, {});
validateCollModOplogEntryCount({"o.index.hidden": false, "o2.indexOptions_old.hidden": false}, 0);

// Validate that if both 'expireAfterSeconds' and 'hidden' options are specified but the 'hidden'
// option is a no-op, the operation as a whole will NOT be a no-op - instead, it will generate an
// oplog entry with only 'expireAfterSeconds'. Ditto for the command result returned to the user.
result = assert.commandWorked(primaryDB.runCommand({
    "collMod": primaryColl.getName(),
    "index": {"name": "b_1", "expireAfterSeconds": 10, "hidden": false},
}));
validateResultForCollMod(result, {expireAfterSeconds_old: 5, expireAfterSeconds_new: 10});
validateCollModOplogEntryCount({
    "o.index.expireAfterSeconds": 10,
    "o2.indexOptions_old.expireAfterSeconds": 5,
},
                               1);

// Test that the index was successfully modified.
let idxSpec = IndexCatalogHelpers.findByName(primaryColl.getIndexes(), "b_1");
assert.eq(idxSpec.hidden, undefined);
assert.eq(idxSpec.expireAfterSeconds, 10);

// Validate that if both 'unique' and 'hidden' options are specified but the 'hidden'
// option is a no-op, the operation as a whole will NOT be a no-op - instead, it will generate an
// oplog entry with only 'unique'. Ditto for the command result returned to the user.
if (collModIndexUniqueEnabled) {
    assert.commandWorked(primaryDB.runCommand(
        {collMod: collName, index: {keyPattern: {c: 1}, prepareUnique: true}}));
    assert.commandFailedWithCode(primaryDB.runCommand({
        "collMod": primaryColl.getName(),
        "index": {"name": "c_1", "unique": false, "hidden": false},
    }),
                                 ErrorCodes.BadValue);
    result = assert.commandWorked(primaryDB.runCommand({
        "collMod": primaryColl.getName(),
        "index": {"name": "c_1", "unique": true, "hidden": false},
    }));
    validateResultForCollMod(result, {unique_new: true});
    validateCollModOplogEntryCount({
        "o.index.unique": true,
    },
                                   1);

    // Test that the index was successfully modified.
    idxSpec = IndexCatalogHelpers.findByName(primaryColl.getIndexes(), "c_1");
    assert.eq(idxSpec.hidden, undefined);
    assert.eq(idxSpec.expireAfterSeconds, undefined);
    assert(idxSpec.unique, tojson(idxSpec));

    // Validate that if the 'unique' option is specified but is a no-op, the operation as a whole
    // will be a no-op.
    result = assert.commandWorked(primaryDB.runCommand({
        "collMod": primaryColl.getName(),
        "index": {"name": "d_1", "unique": true},
    }));
    validateResultForCollMod(result, {});
    validateCollModOplogEntryCount({
        "o.index.name": "d_1",
    },
                                   0);

    // Test that the index was unchanged.
    idxSpec = IndexCatalogHelpers.findByName(primaryColl.getIndexes(), "d_1");
    assert.eq(idxSpec.hidden, undefined);
    assert.eq(idxSpec.expireAfterSeconds, undefined);
    assert(idxSpec.unique, tojson(idxSpec));

    // Validate that if both the 'hidden' and 'unique' options are specified but the
    // 'hidden' and 'unique' options are no-ops, the operation as a whole will be a no-op.
    result = assert.commandWorked(primaryDB.runCommand({
        "collMod": primaryColl.getName(),
        "index": {"name": "e_1", "hidden": true, "unique": true},
    }));
    validateResultForCollMod(result, {});
    validateCollModOplogEntryCount({
        "o.index.name": "e_1",
    },
                                   0);

    // Test that the index was unchanged.
    idxSpec = IndexCatalogHelpers.findByName(primaryColl.getIndexes(), "e_1");
    assert(idxSpec.hidden, tojson(idxSpec));
    assert.eq(idxSpec.expireAfterSeconds, undefined);
    assert(idxSpec.unique, tojson(idxSpec));

    // Validate that if both 'expireAfterSeconds' and 'unique' options are specified but the
    // 'unique' option is a no-op, the operation as a whole will NOT be a no-op - instead, it will
    // generate an oplog entry with only 'expireAfterSeconds'. Ditto for the command result returned
    // to the user.
    result = assert.commandWorked(primaryDB.runCommand({
        "collMod": primaryColl.getName(),
        "index": {"name": "f_1", "expireAfterSeconds": 20, "unique": true},
    }));
    validateResultForCollMod(result, {expireAfterSeconds_old: 15, expireAfterSeconds_new: 20});
    validateCollModOplogEntryCount({
        "o.index.name": "f_1",
        "o.index.expireAfterSeconds": 20,
        "o2.indexOptions_old.expireAfterSeconds": 15,
        "o.index.unique": {$exists: false},
    },
                                   1);

    // Test that the index was successfully modified.
    idxSpec = IndexCatalogHelpers.findByName(primaryColl.getIndexes(), "f_1");
    assert.eq(idxSpec.hidden, undefined);
    assert.eq(idxSpec.expireAfterSeconds, 20);
    assert(idxSpec.unique, tojson(idxSpec));

    // Validate that if 'expireAfterSeconds', 'hidden', and 'unique' options are specified but the
    // 'hidden' and 'unique' options are no-ops, the operation as a whole will NOT be a no-op -
    // instead, it will generate an oplog entry with only 'expireAfterSeconds'. Ditto for the
    // command result returned to the user.
    result = assert.commandWorked(primaryDB.runCommand({
        "collMod": primaryColl.getName(),
        "index": {"name": "g_1", "expireAfterSeconds": 30, "hidden": true, "unique": true},
    }));
    validateResultForCollMod(result, {expireAfterSeconds_old: 25, expireAfterSeconds_new: 30});
    validateCollModOplogEntryCount({
        "o.index.name": "g_1",
        "o.index.expireAfterSeconds": 30,
        "o2.indexOptions_old.expireAfterSeconds": 25,
        "o.index.hidden": {$exists: false},
        "o.index.unique": {$exists: false},
    },
                                   1);

    // Test that the index was successfully modified.
    idxSpec = IndexCatalogHelpers.findByName(primaryColl.getIndexes(), "g_1");
    assert(idxSpec.hidden, tojson(idxSpec));
    assert.eq(idxSpec.expireAfterSeconds, 30);
    assert(idxSpec.unique, tojson(idxSpec));

    // Validate that if the 'prepareUnique' option is specified but is a no-op, the
    // operation as a whole will be a no-op.
    result = assert.commandWorked(primaryDB.runCommand({
        "collMod": primaryColl.getName(),
        "index": {"name": "h_1", "prepareUnique": true},
    }));
    validateResultForCollMod(result, {});
    validateCollModOplogEntryCount({
        "o.index.name": "h_1",
    },
                                   0);

    // Test that the index was unchanged.
    idxSpec = IndexCatalogHelpers.findByName(primaryColl.getIndexes(), "h_1");
    assert.eq(idxSpec.hidden, undefined);
    assert.eq(idxSpec.expireAfterSeconds, undefined);
    assert(idxSpec.prepareUnique, tojson(idxSpec));
}

rst.stopSet();
})();
