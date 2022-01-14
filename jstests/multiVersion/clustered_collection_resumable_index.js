/**
 * Validates that after upgrading we can still resume an index build that used the old RecordID
 * serialization format.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication
 * ]
 */

(function testResumableIndex() {
    'use strict';
    const defaultOptions = {noCleanData: true};

    let mongodOptions5dot1 = Object.extend(
        {binVersion: '5.2', setParameter: 'featureFlagClusteredIndexes=true'}, defaultOptions);
    let mongodOptionsLatest = Object.extend(
        {binVersion: 'latest', setParameter: 'featureFlagClusteredIndexes=true'}, defaultOptions);

    load("jstests/noPassthrough/libs/index_build.js");

    const dbName = "test";

    const rst = new ReplSetTest({nodes: 1, nodeOptions: mongodOptions5dot1});
    rst.startSet();
    rst.initiate();

    const upg = new ReplSetTest(
        {nodes: 1, nodeOptions: Object.assign({port: rst.getPort(0)}, mongodOptionsLatest)});

    const runTests = function(docs, indexSpecsFlat, collNameSuffix) {
        const conn = rst.getPrimary();
        const testDB = conn.getDB(dbName);
        const coll = testDB.getCollection(jsTestName() + collNameSuffix);

        assert.commandWorked(testDB.createCollection(
            coll.getName(), {clusteredIndex: {key: {'_id': 1}, unique: true}}));

        assert.commandWorked(coll.insert(docs));

        const runTest = function(indexSpecs, iteration) {
            ResumableIndexBuildTest.runAndUpgrade(
                rst,
                upg,
                dbName,
                coll.getName(),
                indexSpecs,
                [{
                    name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
                    logIdWithBuildUUID: 20386
                }],
                iteration,
                ["collection scan"],
                [{numScannedAfterResume: 2 - iteration}]);
            upg.stop(conn);
            rst.start(conn);
        };

        runTest([[indexSpecsFlat[0]]], 0);
        runTest([[indexSpecsFlat[0]]], 1);
        runTest([[indexSpecsFlat[0]], [indexSpecsFlat[1]]], 0);
        runTest([[indexSpecsFlat[0]], [indexSpecsFlat[1]]], 1);
        runTest([indexSpecsFlat], 0);
        runTest([indexSpecsFlat], 1);
    };

    const largeKey = '0'.repeat(100);
    const k0 = largeKey + '0';
    const k1 = largeKey + '1';
    runTests([{_id: k0, a: 1, b: 1}, {_id: k1, a: 2, b: 2}], [{a: 1}, {b: 1}], "");
    runTests([{_id: k0, a: [1, 2], b: [1, 2]}, {_id: k1, a: 2, b: 2}],
             [{a: 1}, {b: 1}],
             "_multikey_first");
    runTests([{_id: k0, a: 1, b: 1}, {_id: k1, a: [1, 2], b: [1, 2]}],
             [{a: 1}, {b: 1}],
             "_multikey_last");
    runTests([{_id: k0, a: [1, 2], b: 1}, {_id: k1, a: 2, b: [1, 2]}],
             [{a: 1}, {b: 1}],
             "_multikey_mixed");
    runTests(
        [{_id: k0, a: [1, 2], b: {c: [3, 4]}, d: ""}, {_id: k1, e: "", f: [[]], g: null, h: 8}],
        [{"$**": 1}, {h: 1}],
        "_wildcard");

    rst.stopSet();
    upg.stopSet();
})();
