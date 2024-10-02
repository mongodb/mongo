//
// Basic $changeStream tests for operations that perform queryable encryption.
//
// @tags: [
// change_stream_does_not_expect_txns,
// assumes_unsharded_collection,
// requires_fcv_71
// ]
//
import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";
import {canonicalizeEventForTesting, ChangeStreamTest} from "jstests/libs/change_stream_util.js";

if (!isEnterpriseShell()) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

const dbName = "qetestdb";
const collName = "qetestcoll";
const initialConn = db.getMongo();
const testDb = db.getSiblingDB(dbName);
const placeholderBinData0 = BinData(0, "WMdGo/tcDkE4UL6bgGYTN6oKFitgLXvhyhB9sbKxprk=");
const placeholderBinData6 = BinData(6, "WMdGo/tcDkE4UL6bgGYTN6oKFitgLXvhyhB9sbKxprk=");
const placeholderOID = ObjectId();

function replaceRandomDataWithPlaceholders(event) {
    for (let field in event) {
        if (!Object.prototype.hasOwnProperty.call(event, field)) {
            continue;
        }
        if (event[field] instanceof BinData) {
            if (event[field].subtype() === 6) {
                event[field] = placeholderBinData6;
            } else if (event[field].subtype() === 0) {
                event[field] = placeholderBinData0;
            }
        } else if (event[field] instanceof ObjectId) {
            event[field] = placeholderOID;
        } else if (typeof event[field] === "object") {
            replaceRandomDataWithPlaceholders(event[field]);
        }
    }
}
const eventModifier = function(event, expected) {
    if (event.hasOwnProperty("fullDocument") || event.hasOwnProperty("documentKey")) {
        replaceRandomDataWithPlaceholders(event);
    }
    return canonicalizeEventForTesting(event, expected);
};

testDb.dropDatabase();

let encryptedClient = new EncryptedClient(initialConn, dbName);
assert.commandWorked(encryptedClient.createEncryptionCollection(collName, {
    encryptedFields: {
        "fields": [
            {
                "path": "first",
                "bsonType": "string",
                /* contention: 0 is required for the cleanup tests to work */
                "queries": {"queryType": "equality", "contention": 0}
            },
        ]
    }
}));

const cst = new ChangeStreamTest(testDb, {eventModifier});
const ecoll = encryptedClient.getDB()[collName];
const [escName, ecocName] = (() => {
    let names = encryptedClient.getStateCollectionNamespaces(collName);
    return [names.esc, names.ecoc];
})();

const escInsertChange = {
    documentKey: {_id: placeholderBinData0},
    fullDocument: {_id: placeholderBinData0},
    ns: {db: dbName, coll: escName},
    operationType: "insert",
};
const ecocInsertChange = {
    documentKey: {_id: placeholderOID},
    fullDocument: {_id: placeholderOID, fieldName: "first", value: placeholderBinData0},
    ns: {db: dbName, coll: ecocName},
    operationType: "insert",
};
function expectedEDCInsertChange(id, last, implicitShardKey = undefined) {
    let expected = {
        documentKey: {_id: id},
        fullDocument: {
            _id: id,
            first: placeholderBinData6,
            last: last,
            "__safeContent__": [placeholderBinData0]
        },
        ns: {db: dbName, coll: collName},
        operationType: "insert",
    };
    if (encryptedClient.useImplicitSharding && implicitShardKey) {
        expected.documentKey = Object.assign(expected.documentKey, implicitShardKey);
    }
    return expected;
}

let expectedChange = undefined;
const testValues = [
    ["frodo", "baggins"],
    ["merry", "brandybuck"],
    ["pippin", "took"],
    ["sam", "gamgee"],
    ["rosie", "gamgee"],
    ["paladin", "took"],
];

let cursor =
    cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: testDb[collName]});
let cursordb = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

// Test that if there are no changes, we return an empty batch.
assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));
assert.eq(0, cursordb.firstBatch.length, "Cursor had changes: " + tojson(cursordb));

jsTestLog("Testing single insert");
{
    assert.commandWorked(ecoll.einsert({_id: 0, first: "frodo", last: "baggins"}));
    expectedChange = expectedEDCInsertChange(0, "baggins", {last: "baggins"});

    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expectedChange]});
    cst.assertNextChangesEqualUnordered(
        {cursor: cursordb, expectedChanges: [expectedChange, escInsertChange, ecocInsertChange]});
    cst.assertNoChange(cursor);
    cst.assertNoChange(cursordb);
}

jsTestLog("Testing second insert");
{
    cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: testDb[collName]});
    cursordb = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    assert.commandWorked(ecoll.einsert({_id: 1, first: "merry", last: "brandybuck"}));
    expectedChange = expectedEDCInsertChange(1, "brandybuck", {last: "brandybuck"});

    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expectedChange]});
    cst.assertNextChangesEqualUnordered(
        {cursor: cursordb, expectedChanges: [expectedChange, escInsertChange, ecocInsertChange]});
    cst.assertNoChange(cursor);
    cst.assertNoChange(cursordb);
}

jsTestLog("Testing replacement update");
{
    cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: testDb[collName]});
    cursordb = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    assert.commandWorked(
        ecoll.ereplaceOne({last: "baggins"}, {first: "pippin", last: "took", location: "shire"}));
    expectedChange = expectedEDCInsertChange(0, "took", {last: "baggins"});
    expectedChange.operationType = "replace";
    expectedChange.fullDocument.location = "shire";

    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expectedChange]});
    cst.assertNextChangesEqualUnordered(
        {cursor: cursordb, expectedChanges: [expectedChange, escInsertChange, ecocInsertChange]});
}

jsTestLog("Testing upsert");
{
    cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: testDb[collName]});
    cursordb = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    assert.commandWorked(ecoll.ereplaceOne(
        {last: "gamgee"}, {_id: 2, first: "sam", last: "gamgee"}, {upsert: true}));

    expectedChange = expectedEDCInsertChange(2, "gamgee", {last: "gamgee"});
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expectedChange]});
    cst.assertNextChangesEqualUnordered(
        {cursor: cursordb, expectedChanges: [expectedChange, escInsertChange, ecocInsertChange]});
    cst.assertNoChange(cursor);
    cst.assertNoChange(cursordb);
}

jsTestLog("Testing modification update");
{
    cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: testDb[collName]});
    cursordb = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    assert.commandWorked(ecoll.eupdateOne({last: "gamgee"}, {$set: {first: "rosie"}}));
    expectedChange = {
        documentKey: {_id: 2},
        ns: {db: dbName, coll: collName},
        operationType: "update",
        updateDescription: {
            removedFields: [],
            updatedFields: {first: placeholderBinData6, "__safeContent__.1": placeholderBinData0},
            truncatedArrays: []
        },
    };
    let safeContentPullChange = {
        documentKey: {_id: 2},
        ns: {db: dbName, coll: collName},
        operationType: "update",
        updateDescription: {
            removedFields: [],
            updatedFields: {"__safeContent__": [placeholderBinData0]},
            truncatedArrays: []
        },
    };
    if (encryptedClient.useImplicitSharding) {
        expectedChange.documentKey.last = "gamgee";
        safeContentPullChange.documentKey.last = "gamgee";
    }

    cst.assertNextChangesEqual(
        {cursor: cursor, expectedChanges: [expectedChange, safeContentPullChange]});
    cst.assertNextChangesEqualUnordered({
        cursor: cursordb,
        expectedChanges: [expectedChange, escInsertChange, ecocInsertChange, safeContentPullChange]
    });
    cst.assertNoChange(cursor);
    cst.assertNoChange(cursordb);
}

jsTestLog("Testing findAndModify");
{
    cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: testDb[collName]});
    cursordb = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    assert.commandWorked(ecoll.erunCommand({
        findAndModify: ecoll.getName(),
        query: {last: "took"},
        update: {$set: {first: "paladin"}, $unset: {location: ""}},
    }));
    expectedChange = {
        documentKey: {_id: 0},
        ns: {db: dbName, coll: collName},
        operationType: "update",
        updateDescription: {
            removedFields: ["location"],
            updatedFields: {first: placeholderBinData6, "__safeContent__.1": placeholderBinData0},
            truncatedArrays: []
        },
    };
    let safeContentPullChange = {
        documentKey: {_id: 0},
        ns: {db: dbName, coll: collName},
        operationType: "update",
        updateDescription: {
            removedFields: [],
            updatedFields: {"__safeContent__": [placeholderBinData0]},
            truncatedArrays: []
        },
    };
    if (encryptedClient.useImplicitSharding) {
        expectedChange.documentKey.last = "took";
        safeContentPullChange.documentKey.last = "took";
    }

    cst.assertNextChangesEqual(
        {cursor: cursor, expectedChanges: [expectedChange, safeContentPullChange]});
    cst.assertNextChangesEqualUnordered({
        cursor: cursordb,
        expectedChanges: [expectedChange, escInsertChange, ecocInsertChange, safeContentPullChange]
    });
    cst.assertNoChange(cursor);
    cst.assertNoChange(cursordb);
}

jsTestLog("Testing delete");
{
    cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: testDb[collName]});
    cursordb = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    assert.commandWorked(ecoll.edeleteOne({last: "gamgee"}));
    expectedChange = {
        documentKey: {_id: 2},
        ns: {db: dbName, coll: collName},
        operationType: "delete",
    };
    if (encryptedClient.useImplicitSharding) {
        expectedChange.documentKey.last = "gamgee";
    }
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expectedChange]});
    cst.assertNextChangesEqual({cursor: cursordb, expectedChanges: [expectedChange]});
    cst.assertNoChange(cursor);
    cst.assertNoChange(cursordb);
}

const ecocRenameChange = {
    operationType: "rename",
    ns: {db: dbName, coll: ecocName},
    to: {db: dbName, coll: ecocName + ".compact"},
};
const escDeleteChange = {
    operationType: "delete",
    ns: {db: dbName, coll: escName},
    documentKey: {_id: placeholderBinData0},
};
const ecocCompactDropChange = {
    operationType: "drop",
    ns: {db: dbName, coll: ecocName + ".compact"},
};

jsTestLog("Testing compact");
{
    // all non-anchors will be deleted by compact
    const deleteCount = testDb[escName].countDocuments({value: {$exists: false}});
    const numUniqueValues = testValues.length;

    encryptedClient.assertEncryptedCollectionCounts(collName, 2, numUniqueValues, numUniqueValues);

    cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: testDb[collName]});
    cursordb = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    encryptedClient.runEncryptionOperation(() => {
        assert.commandWorked(ecoll.compact());
    });
    encryptedClient.assertEncryptedCollectionCounts(collName, 2, numUniqueValues, 0);
    const anchorCount = testDb[escName].countDocuments({value: {$exists: true}});
    const nonAnchorCount = testDb[escName].countDocuments({value: {$exists: false}});
    assert.eq(anchorCount, numUniqueValues);
    assert.eq(nonAnchorCount, 0);

    cst.assertNoChange(cursor);

    escInsertChange.fullDocument.value = placeholderBinData0;
    // temp ecoc rename
    cst.assertNextChangesEqual({cursor: cursordb, expectedChanges: [ecocRenameChange]});
    // normal anchor inserts
    for (let i = 0; i < numUniqueValues; i++) {
        cst.assertNextChangesEqual({cursor: cursordb, expectedChanges: [escInsertChange]});
    }
    // non-anchor deletes
    for (let i = 0; i < deleteCount; i++) {
        cst.assertNextChangesEqual({cursor: cursordb, expectedChanges: [escDeleteChange]});
    }
    // temp ecoc drop
    cst.assertNextChangesEqual({cursor: cursordb, expectedChanges: [ecocCompactDropChange]});
    cst.assertNoChange(cursordb);
}

jsTestLog("Testing cleanup");
{
    // insert new documents for each test value, so the ECOC & ESC have documents to clean up
    for (let val of testValues) {
        assert.commandWorked(ecoll.einsert({first: val[0], last: val[1]}));
    }
    // ESC doesn't have null anchors yet, so the total delete count == ESC count before cleanup
    const deleteCount = testDb[escName].countDocuments({});
    const nonAnchorCount = testDb[escName].countDocuments({value: {$exists: false}});
    const anchorCount = deleteCount - nonAnchorCount;
    const numUniqueValues = testValues.length;

    encryptedClient.assertEncryptedCollectionCounts(
        collName, 2 + numUniqueValues, numUniqueValues * 2, numUniqueValues);
    assert.eq(anchorCount, numUniqueValues);
    assert.eq(nonAnchorCount, numUniqueValues);

    cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: testDb[collName]});
    cursordb = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    encryptedClient.runEncryptionOperation(() => {
        assert.commandWorked(ecoll.cleanup());
    });
    encryptedClient.assertEncryptedCollectionCounts(
        collName, 2 + numUniqueValues, numUniqueValues, 0);
    encryptedClient.assertESCNonAnchorCount(collName, 0);

    cst.assertNoChange(cursor);

    // temp ecoc rename
    cst.assertNextChangesEqual({cursor: cursordb, expectedChanges: [ecocRenameChange]});
    // null anchor inserts
    escInsertChange.fullDocument.value = placeholderBinData0;
    for (let i = 0; i < anchorCount; i++) {
        cst.assertNextChangesEqual({cursor: cursordb, expectedChanges: [escInsertChange]});
    }
    // non-anchors and regular anchors are deleted from ESC
    for (let i = 0; i < deleteCount; i++) {
        cst.assertNextChangesEqual({cursor: cursordb, expectedChanges: [escDeleteChange]});
    }
    // temp ecoc.compact drop
    cst.assertNextChangesEqual({cursor: cursordb, expectedChanges: [ecocCompactDropChange]});
    cst.assertNoChange(cursordb);
}

cst.cleanUp();
