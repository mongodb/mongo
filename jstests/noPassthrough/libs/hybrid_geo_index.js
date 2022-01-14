// Helper functions to test hybrid geo index builds

const Operation = Object.freeze({REMOVE: 0, UPDATE: 1});

var HybridGeoIndexTest = class {
    static run(rsOptions, createOptions, invalidKey, validKey, op) {
        'use strict';

        load('jstests/noPassthrough/libs/index_build.js');

        const rst = new ReplSetTest({
            nodes: [
                rsOptions,
                Object.extend({
                    // Disallow elections on secondary.
                    rsConfig: {
                        priority: 0,
                        votes: 0,
                    }
                },
                              rsOptions)
            ]
        });
        const nodes = rst.startSet();
        rst.initiate();

        const primary = rst.getPrimary();
        const testDB = primary.getDB('test');
        const coll = testDB.getCollection('test');

        assert.commandWorked(testDB.createCollection(coll.getName(), createOptions));

        // Insert an invalid geo document that will be removed before the indexer starts a collecton
        // scan.
        assert.commandWorked(coll.insert({
            _id: invalidKey,
            b: {type: 'invalid_geo_json_type', coordinates: [100, 100]},
        }));

        // We are using this fail point to pause the index build before it starts the collection
        // scan. This is important for this test because we are mutating the collection state before
        // the index builder is able to observe the invalid geo document. By comparison,
        // IndexBuildTest.pauseIndexBuilds() stalls the index build in the middle of the collection
        // scan.
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'alwaysOn'}));

        const createIdx =
            IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {b: '2dsphere'});
        IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'b_2dsphere');

        switch (op) {
            case Operation.REMOVE:
                // Insert a valid geo document to initialize the hybrid index builder's side table
                // state.
                assert.commandWorked(coll.insert({
                    _id: validKey,
                    b: {type: 'Polygon', coordinates: [[[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]]},
                }));

                // Removing the invalid geo document should not cause any issues for the side table
                // accounting.
                assert.commandWorked(coll.remove({_id: invalidKey}));
                break;
            case Operation.UPDATE:
                // Fixing the invalid geo document should not cause any issues for the side table
                // accounting.
                assert.commandWorked(coll.update({_id: invalidKey}, {
                    b: {type: 'Polygon', coordinates: [[[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]]}
                }));
                break;
        }

        assert.commandWorked(
            testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'off'}));

        // Wait for the index build to finish. Since the invalid geo document is removed before the
        // index build scans the collection, the index should be built successfully.
        createIdx();
        IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'b_2dsphere']);

        let res = assert.commandWorked(coll.validate({full: true}));
        assert(res.valid, 'validation failed on primary: ' + tojson(res));

        rst.stopSet();
    }
};
