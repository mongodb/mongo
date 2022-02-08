/**
 * Tests that large clustered keys can be serialized properly.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   requires_fcv_53
 * ]
 */

// Use hybrid_geo_index.js to exercise RecordId::serializeToken
(function testRecordSerializationForSkippedRecordTracker() {
    'use strict';

    load('jstests/noPassthrough/libs/hybrid_geo_index.js');

    const rsOptions = {
        wiredTigerEngineConfigString: 'eviction_dirty_trigger=80'  // needed for larger recordIds
    };
    const createOptions = {clusteredIndex: {key: {'_id': 1}, unique: true}};

    const largeKey = '0'.repeat(8 * 1024 * 1024 - 3);  // 8 MB keys
    const invalidKey = largeKey + '0';
    const validKey = largeKey + '1';

    HybridGeoIndexTest.run(rsOptions, createOptions, invalidKey, validKey, Operation.REMOVE);
})();

(function testDuplicateKeyErrorsForLargeKeys() {
    'use strict';

    const rst = new ReplSetTest({name: 'testName', nodes: 1, nodeOptions: {}});
    const nodes = rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const testDB = primary.getDB('test');
    const coll = testDB.getCollection('test');

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {clusteredIndex: {key: {'_id': 1}, unique: true}}));

    const largeKey = '0'.repeat(8 * 1024 * 1024 - 2);  // 8 MB key

    assert.commandWorked(coll.insert({_id: largeKey, b: {}}));
    assert.commandFailedWithCode(coll.insert({_id: largeKey, c: {}}), 11000);

    rst.stopSet();
})();
