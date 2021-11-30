// Tests that running validate in read-only mode doesn't try to fix metadata
// @tags: [assumes_against_mongod_not_mongos]

load("jstests/readonly/lib/read_only_test.js");

// We skip doing the data consistency checks while terminating the cluster because we leave data in
// an inconsitent state on purpose.
TestData.skipCollectionAndIndexValidation = true;

runReadOnlyTest(function() {
    'use strict';
    return {
        name: 'validate',
        load: function(writableCollection) {
            const db = writableCollection.getDB();
            assert.commandWorked(
                db.adminCommand({configureFailPoint: "skipUpdateIndexMultikey", mode: "alwaysOn"}));
            assert.commandWorked(writableCollection.createIndex({a: 1, b: 1}, {name: 'idx'}));
            assert.commandWorked(writableCollection.insert({_id: 0, a: [0, 1]}));
            assert.commandWorked(
                db.adminCommand({configureFailPoint: "skipUpdateIndexMultikey", mode: "off"}));
        },
        exec: function(readableCollection) {
            // Make sure we don't try to adjust multikey indices in read-only mode
            const res = readableCollection.validate({full: true});
            assert.eq(1, res.ok, "Expected success of validate on read-only mode");
            assert.eq(false, res.valid);
            assert.eq(1, res.errors.length);

            const idx = res.indexDetails.idx;
            assert.eq(false, idx.valid);
            assert.eq(1, idx.errors.length);

            // Test that validate { repair: true } fails in read-only mode.
            assert.commandFailedWithCode(
                readableCollection.validate({full: true, repair: true}),
                ErrorCodes.InvalidOptions,
                "Expected validate to fail because repairing a read-only database is not possible");
        }
    };
}());
