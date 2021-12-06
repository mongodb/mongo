/**
 * Test that the collMod command allows concurrent writes while converting regular indexes to
 * unique indexes.
 *
 * @tags: [
 *  # TODO(SERVER-61181): Fix validation errors under ephemeralForTest.
 *  incompatible_with_eft,
 *  # TODO(SERVER-61182): Fix WiredTigerKVEngine::alterIdentMetadata() under inMemory.
 *  requires_persistence,
 *  # Replication requires journaling support so this tag also implies exclusion from
 *  # --nojournal test configurations.
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
const collPrefix = 'collmod_convert_to_unique_side_writes_';

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
 * While the 'collMod' command in paused, runs 'doCrudOpsFunc' before resuming the
 * conversion process. Confirms expected 'collMod' behavior.
 */
const testCollModConvertUniqueWithSideWrites = function(performCrudOpsFunc, expectSuccess) {
    const testDB = primary.getDB('test');
    const collName = collPrefix + collCount++;
    const coll = testDB.getCollection(collName);

    jsTestLog('Starting test on collection: ' + coll.getFullName());
    assert.commandWorked(testDB.createCollection(collName));

    // Creates a regular index and use collMod to convert it to a unique index.
    assert.commandWorked(coll.createIndex({a: 1}));

    // Initial documents. If the conversion is expected to be successful, we
    // can check the uniquenes contraint using the values 'a' in these seed
    // documents.
    const docs = [
        {_id: 1, a: 100},
        {_id: 2, a: 200},
        {_id: 3, a: 300},
    ];
    assert.commandWorked(coll.insert(docs));

    let awaitCollMod = () => {};
    const failPoint = configureFailPoint(
        primary, 'hangAfterCollModIndexUniqueSideWriteTracker', {nss: coll.getFullName()});
    try {
        // Start collMod unique index conversion.
        if (expectSuccess) {
            awaitCollMod = assertCommandWorkedInParallelShell(
                primary, testDB, {collMod: collName, index: {keyPattern: {a: 1}, unique: true}});
        } else {
            awaitCollMod = assertCommandFailedWithCodeInParallelShell(
                primary,
                testDB,
                {collMod: collName, index: {keyPattern: {a: 1}, unique: true}},
                ErrorCodes.CannotEnableIndexConstraint);
        }
        failPoint.wait();

        // Check locks held by collMod while waiting on fail point.
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
                                                    'locks.Collection': 'r'
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
                  'r',
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

    if (expectSuccess) {
        assert.eq(countUnique(coll, {a: 1}),
                  1,
                  'index should be unique now: ' + tojson(coll.getIndexes()));

        // Test uniqueness constraint.
        assert.commandFailedWithCode(coll.insert({_id: 100, a: 100}), ErrorCodes.DuplicateKey);
    } else {
        assert.eq(
            countUnique(coll, {a: 1}), 0, 'index should not unique: ' + tojson(coll.getIndexes()));

        // Check that uniquenesss constraint is not enforceed.
        assert.commandWorked(coll.insert({_id: 100, a: 100}));
    }
    jsTestLog('Successsfully completed test on collection: ' + coll.getFullName());
};

// Checks successful conversion with non-conflicting documents inserted during collMod.
testCollModConvertUniqueWithSideWrites((coll) => {
    const docs = [
        {_id: 4, a: 400},
        {_id: 5, a: 500},
        {_id: 6, a: 600},
    ];
    jsTestLog('Inserting additional documents after collMod completed index scan: ' + tojson(docs));
    assert.commandWorked(coll.insert(docs));
    jsTestLog('Successfully inserted documents. Resuming collMod index conversion: ' +
              tojson(docs));
}, true /* expectSuccess */);

// Confirms that conversion fails with a conflicting document inserted during collMod.
testCollModConvertUniqueWithSideWrites((coll) => {
    const docs = [
        {_id: 1000, a: 100},
    ];
    jsTestLog('Inserting additional documents after collMod completed index scan: ' + tojson(docs));
    assert.commandWorked(coll.insert(docs));
    jsTestLog('Successfully inserted documents. Resuming collMod index conversion: ' +
              tojson(docs));
}, false /* expectSuccess */);

// Confirms that conversion fails if an update during collMod leads to a conflict.
testCollModConvertUniqueWithSideWrites((coll) => {
    jsTestLog('Updating single document after collMod completed index scan.');
    assert.commandWorked(coll.update({_id: 1}, {a: 200}));
    jsTestLog('Successfully updated document. Resuming collMod index conversion.');
}, false /* expectSuccess */);

// Inserting and deleting a conflicting document before collMod obtains exclusive access to the
// collection to complete the conversion should result in a successful conversion.
testCollModConvertUniqueWithSideWrites((coll) => {
    jsTestLog('Inserting and removing a conflicting document after collMod completed index scan.');
    assert.commandWorked(coll.insert({_id: 101, a: 100}));
    assert.commandWorked(coll.remove({_id: 101}));
    jsTestLog('Successfully inserted and removed document. Resuming collMod index conversion.');
}, true /* expectSuccess */);

// Inserting a non-conflicting document containing an unindexed field should not affect conversion.
testCollModConvertUniqueWithSideWrites((coll) => {
    jsTestLog('Inserting a non-conflicting document containing an unindexed field.');
    assert.commandWorked(coll.insert({_id: 7, a: 700, b: 2222}));
    jsTestLog('Successfully inserted a non-conflicting document containing an unindexed field. ' +
              'Resuming collMod index conversion.');
}, true /* expectSuccess */);

// Removing the last entry in the index should not throw off the index scan.
testCollModConvertUniqueWithSideWrites((coll) => {
    jsTestLog('Removing the last index entry');
    assert.commandWorked(coll.remove({_id: 3}));
    jsTestLog('Successfully the last index entry. Resuming collMod index conversion.');
}, true /* expectSuccess */);

// Make the index multikey with a non-conflicting document.
testCollModConvertUniqueWithSideWrites((coll) => {
    jsTestLog('Converting the index to multikey with non-conflicting document');
    assert.commandWorked(coll.insert({_id: 8, a: [400, 500]}));
    jsTestLog('Successfully converted the index to multikey with non-conflicting document');
}, true /* expectSuccess */);

// Make the index multikey with a conflicting document.
testCollModConvertUniqueWithSideWrites((coll) => {
    jsTestLog('Converting the index to multikey with conflicting document');
    assert.commandWorked(coll.insert({_id: 9, a: [900, 100]}));
    jsTestLog('Successfully converted the index to multikey with conflicting document');
}, false /* expectSuccess */);

rst.stopSet();
})();
