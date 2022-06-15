/**
 * Tests that the collMod command disallows concurrent writes that introduce new duplicate keys
 * while converting regular indexes to unique indexes.
 *
 * @tags: [
 *  # TODO(SERVER-61182): Fix WiredTigerKVEngine::alterIdentMetadata() under inMemory.
 *  requires_persistence,
 *  requires_replication,
 * ]
 */

(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const collModIndexUniqueEnabled =
    assert.commandWorked(primary.adminCommand({getParameter: 1, featureFlagCollModIndexUnique: 1}))
        .featureFlagCollModIndexUnique.value;

if (!collModIndexUniqueEnabled) {
    jsTestLog('Skipping test because the collMod unique index feature flag is disabled');
    rst.stopSet();
    return;
}

let collCount = 0;
const collPrefix = 'collmod_convert_to_unique_disallow_duplicates_';

/**
 * Returns the number of unique indexes with the given key pattern.
 */
const countUnique = function(coll, key) {
    const all = coll.getIndexes().filter(function(z) {
        return z.unique && friendlyEqual(z.key, key);
    });
    return all.length;
};

/**
 * Starts and pauses a unique index conversion in the collection.
 * While the 'collMod' command in paused, runs 'performCrudOpsFunc' before resuming the
 * conversion process. Confirms expected 'collMod' behavior.
 */
const testCollModConvertUniqueWithSideWrites = function(initialDocs,
                                                        performCrudOpsFunc,
                                                        duplicateDoc = {
                                                            _id: 100,
                                                            a: 100
                                                        },
                                                        expectedViolations = undefined) {
    const testDB = primary.getDB('test');
    const collName = collPrefix + collCount++;
    const coll = testDB.getCollection(collName);

    jsTestLog('Starting test on collection: ' + coll.getFullName());
    assert.commandWorked(testDB.createCollection(collName));

    // Creates a regular index and use collMod to convert it to a unique index.
    assert.commandWorked(coll.createIndex({a: 1}));

    // Initial documents.
    assert.commandWorked(coll.insert(initialDocs));

    // Disallows new duplicate keys on the index.
    assert.commandWorked(
        testDB.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: true}}));

    let awaitCollMod = () => {};
    const failPoint = configureFailPoint(
        primary, 'hangAfterCollModIndexUniqueFullIndexScan', {nss: coll.getFullName()});
    try {
        // Starts collMod unique index conversion.
        if (!expectedViolations) {
            awaitCollMod = assertCommandWorkedInParallelShell(
                primary, testDB, {collMod: collName, index: {keyPattern: {a: 1}, unique: true}});
        } else {
            const assertViolations = function(result, expectedViolations) {
                const compareIds = function(lhs, rhs) {
                    try {
                        assert.sameMembers(lhs.ids, rhs.ids);
                    } catch (e) {
                        return false;
                    }
                    return true;
                };
                assert.sameMembers(result.violations, expectedViolations, '', compareIds);
            };
            awaitCollMod = assertCommandFailedWithCodeInParallelShell(
                primary,
                testDB,
                {collMod: collName, index: {keyPattern: {a: 1}, unique: true}},
                ErrorCodes.CannotConvertIndexToUnique,
                assertViolations,
                expectedViolations);
        }
        failPoint.wait();

        // Checks locks held by collMod while waiting on fail point.
        const currentOpResult = testDB.getSiblingDB("admin")
                                    .aggregate(
                                        [
                                            {$currentOp: {allUsers: true, idleConnections: true}},
                                            {
                                                $match: {
                                                    type: 'op',
                                                    op: 'command',
                                                    connectionId: {$exists: true},
                                                    ns: `${coll.getDB().$cmd.getFullName()}`,
                                                    'command.collMod': coll.getName(),
                                                    'locks.Collection': 'w'
                                                }
                                            },
                                        ],
                                        {readConcern: {level: "local"}})
                                    .toArray();
        assert.eq(
            currentOpResult.length,
            1,
            'unable to find collMod command in db.currentOp() result: ' + tojson(currentOpResult));
        const collModOp = currentOpResult[0];
        assert(collModOp.hasOwnProperty('locks'),
               'no lock info in collMod op from db.currentOp(): ' + tojson(collModOp));
        assert.eq(collModOp.locks.Collection,
                  'w',
                  'collMod is not holding collection lock in read mode: ' + tojson(collModOp));

        jsTestLog('Performing CRUD ops on collection while collMod is paused: ' +
                  performCrudOpsFunc);
        try {
            performCrudOpsFunc(coll);
        } catch (ex) {
            jsTestLog('CRUD ops failed: ' + ex);
            doassert('CRUD ops failed: ' + ex + ': ' + performCrudOpsFunc);
        }
    } finally {
        failPoint.off();
        awaitCollMod();
    }

    if (!expectedViolations) {
        assert.eq(countUnique(coll, {a: 1}),
                  1,
                  'index should be unique now: ' + tojson(coll.getIndexes()));

        // Tests uniqueness constraint.
        assert.commandFailedWithCode(coll.insert(duplicateDoc), ErrorCodes.DuplicateKey);
    } else {
        assert.eq(
            countUnique(coll, {a: 1}), 0, 'index should not unique: ' + tojson(coll.getIndexes()));

        // Resets to allow duplicates on the regular index.
        assert.commandWorked(testDB.runCommand(
            {collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: false}}));

        // Checks that uniqueness constraint is not enforced.
        assert.commandWorked(coll.insert(duplicateDoc));
    }
    jsTestLog('Successfully completed test on collection: ' + coll.getFullName());
};

const initialDocsUnique = [
    {_id: 1, a: 100},
    {_id: 2, a: 200},
    {_id: 3, a: 300},
];

const initialDocsDuplicate = [
    {_id: 1, a: 100},
    {_id: 2, a: 100},
    {_id: 3, a: 200},
    {_id: 4, a: 200},
];

// Checks successful conversion with non-conflicting documents inserted during collMod.
testCollModConvertUniqueWithSideWrites(initialDocsUnique, (coll) => {
    const docs = [
        {_id: 4, a: 400},
        {_id: 5, a: 500},
        {_id: 6, a: 600},
    ];
    jsTestLog('Inserting additional documents after collMod completed index scan: ' + tojson(docs));
    assert.commandWorked(coll.insert(docs));
    jsTestLog('Successfully inserted documents. Resuming collMod index conversion: ' +
              tojson(docs));
});

// Checks successful conversion with a conflicting document rejected during collMod.
testCollModConvertUniqueWithSideWrites(initialDocsUnique, (coll) => {
    jsTestLog('Inserting additional documents after collMod completed index scan.');
    assert.commandFailedWithCode(coll.insert({_id: 1000, a: 100}), ErrorCodes.DuplicateKey);
    jsTestLog('Failed to insert documents. Resuming collMod index conversion.');
});

// Checks successful conversion with a conflicting update rejected during collMod.
testCollModConvertUniqueWithSideWrites(initialDocsUnique, (coll) => {
    jsTestLog('Updating single document after collMod completed index scan.');
    assert.commandFailedWithCode(coll.update({_id: 1}, {a: 200}), ErrorCodes.DuplicateKey);
    jsTestLog('Failed to update document. Resuming collMod index conversion.');
});

// Inserts a non-conflicting document containing an unindexed field should not affect conversion.
testCollModConvertUniqueWithSideWrites(initialDocsUnique, (coll) => {
    jsTestLog('Inserting a non-conflicting document containing an unindexed field.');
    assert.commandWorked(coll.insert({_id: 7, a: 700, b: 2222}));
    jsTestLog('Successfully inserted a non-conflicting document containing an unindexed field. ' +
              'Resuming collMod index conversion.');
});

// Removes the last entry in the index should not throw off the index scan.
testCollModConvertUniqueWithSideWrites(initialDocsUnique, (coll) => {
    jsTestLog('Removing the last index entry.');
    assert.commandWorked(coll.remove({_id: 3}));
    jsTestLog('Successfully removed the last index entry. Resuming collMod index conversion.');
});

// Makes the index multikey with a non-conflicting document.
testCollModConvertUniqueWithSideWrites(initialDocsUnique, (coll) => {
    jsTestLog('Converting the index to multikey with non-conflicting document.');
    assert.commandWorked(coll.insert({_id: 8, a: [400, 500]}));
    jsTestLog('Successfully converted the index to multikey with non-conflicting document.');
});

// Makes the index multikey with a conflicting document.
testCollModConvertUniqueWithSideWrites(initialDocsUnique, (coll) => {
    jsTestLog('Converting the index to multikey with conflicting document.');
    assert.commandFailedWithCode(coll.insert({_id: 9, a: [900, 100]}), ErrorCodes.DuplicateKey);
    jsTestLog('Failed to convert the index to multikey with a conflicting document.');
});

// All duplicates will be rejected during collMod. The conversion still succeeds eventually.
testCollModConvertUniqueWithSideWrites(initialDocsUnique, (coll) => {
    jsTestLog('Inserting additional documents after collMod completed index scan.');
    assert.commandFailedWithCode(coll.insert({_id: 1000, a: 100}), ErrorCodes.DuplicateKey);
    assert.commandFailedWithCode(coll.insert({_id: 1001, a: 100}), ErrorCodes.DuplicateKey);
    assert.commandFailedWithCode(coll.insert({_id: 1002, a: 200}), ErrorCodes.DuplicateKey);
    assert.commandFailedWithCode(coll.insert({_id: 1003, a: 200}), ErrorCodes.DuplicateKey);
    jsTestLog('Failed to insert documents. Resuming collMod index conversion.');
});

// Checks unsuccessful conversion due to duplicates in the initial collection as well as rejects a
// conflicting document during collMod.
testCollModConvertUniqueWithSideWrites(initialDocsDuplicate, (coll) => {
    jsTestLog('Inserting additional documents after collMod completed index scan.');
    assert.commandFailedWithCode(coll.insert({_id: 1000, a: 100}), ErrorCodes.DuplicateKey);
    jsTestLog('Failed to insert documents. Resuming collMod index conversion.');
}, {_id: 1000, a: 100} /* duplicateDoc */, [{ids: [1, 2]}, {ids: [3, 4]}] /* expectedViolations */);

rst.stopSet();
})();
