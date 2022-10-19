// Test that the collection catalog is restored correctly after a restart in a multitenant
// environment.

(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For arrayEq()

function runTest(featureFlagRequireTenantId) {
    const rst = new ReplSetTest({
        nodes: 3,
        nodeOptions: {
            auth: '',
            setParameter: {
                multitenancySupport: true,
                featureFlagMongoStore: true,
                featureFlagRequireTenantID: featureFlagRequireTenantId
            }
        }
    });
    rst.startSet({keyFile: 'jstests/libs/key1'});
    rst.initiate();

    let primary = rst.getPrimary();
    let adminDb = primary.getDB('admin');

    // Must be authenticated as a user with ActionType::useTenant in order to use $tenant.
    assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
    assert(adminDb.auth('admin', 'pwd'));

    {
        const kTenant = ObjectId();
        let testDb = primary.getDB('myDb0');

        // Create a collection by inserting a document to it.
        assert.commandWorked(testDb.runCommand(
            {insert: 'myColl0', documents: [{_id: 0, a: 1, b: 1}], '$tenant': kTenant}));

        // Run findAndModify on the document.
        let fad = assert.commandWorked(testDb.runCommand({
            findAndModify: "myColl0",
            query: {a: 1},
            update: {$inc: {a: 10}},
            '$tenant': kTenant
        }));
        assert.eq({_id: 0, a: 1, b: 1}, fad.value);

        // Create a view on the collection.
        assert.commandWorked(testDb.runCommand(
            {"create": "view1", "viewOn": "myColl0", pipeline: [], '$tenant': kTenant}));

        // Stop the rs and restart it.
        rst.stopSet(null /* signal */, true /* forRestart */, {noCleanData: true});
        rst.startSet({restart: true});
        primary = rst.getPrimary();

        adminDb = primary.getDB('admin');
        assert(adminDb.auth('admin', 'pwd'));
        testDb = primary.getDB('myDb0');

        // Assert we see 3 collections in the tenant's db 'myDb0' - the original collection we
        // created, the view on it, and the system.views collection.
        const colls = assert.commandWorked(
            testDb.runCommand({listCollections: 1, nameOnly: true, '$tenant': kTenant}));
        assert.eq(3, colls.cursor.firstBatch.length, tojson(colls.cursor.firstBatch));
        const expectedColls = [
            {"name": "myColl0", "type": "collection"},
            {"name": "system.views", "type": "collection"},
            {"name": "view1", "type": "view"}
        ];
        assert(arrayEq(expectedColls, colls.cursor.firstBatch), tojson(colls.cursor.firstBatch));

        // Assert we can still run findAndModify on the doc.
        fad = assert.commandWorked(testDb.runCommand({
            findAndModify: "myColl0",
            query: {a: 11},
            update: {$inc: {a: 10}},
            '$tenant': kTenant
        }));
        assert.eq({_id: 0, a: 11, b: 1}, fad.value);

        // Check that we will cannot run findAndModify on the doc when the tenantId is passed as the
        // prefix.
        fad = assert.commandWorked(
            primary.getDB(kTenant + '_myDb0')
                .runCommand({findAndModify: "myColl0", query: {b: 1}, update: {$inc: {b: 10}}}));
        assert.eq(null, fad.value);
    }

    rst.stopSet();
}
runTest(true);
runTest(false);
})();
