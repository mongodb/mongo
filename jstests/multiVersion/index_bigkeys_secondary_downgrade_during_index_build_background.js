/**
 * If a secondary is downgraded to FCV 4.0, it should still accept long index
 * keys because the long index keys must already exist on the primary.
 * TODO SERVER-36385: remove this test in 4.4.
 */
(function() {
    'use strict';

    load("jstests/libs/feature_compatibility_version.js");
    load('jstests/noPassthrough/libs/index_build.js');

    TestData.replSetFeatureCompatibilityVersion = "4.2";
    const rst = new ReplSetTest({nodes: [{binVersion: 'latest'}, {binVersion: 'latest'}]});
    rst.startSet();
    rst.initiate();
    rst.awaitReplication();

    const dbName = "test";
    const collName = "index_bigkeys_downgrade_during_index_build";

    const largeKey = 's'.repeat(12345);
    const documentWithLargeKey = {x: largeKey};

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const primaryDB = primary.getDB(dbName);
    const secondaryDB = secondary.getDB(dbName);
    const testColl = primaryDB[collName];

    testColl.drop({writeConcern: {w: 2}});

    // Both primary and secondary have documents with large keys.
    let documents = [];
    for (let i = 0; i < 10; i++) {
        documents.push(documentWithLargeKey);
    }
    assert.commandWorked(
        primaryDB.runCommand({insert: collName, documents: documents, writeConcern: {w: 2}}));

    assert.commandWorked(secondaryDB.adminCommand(
        {configureFailPoint: "hangAfterStartingIndexBuild", mode: "alwaysOn"}));

    // Start the index build on the primary.
    assert.commandWorked(primaryDB.runCommand(
        {createIndexes: collName, indexes: [{key: {x: 1}, name: "x_1", background: true}]}));

    // Make sure index build starts on the secondary.
    IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

    // Downgrade the FCV to 4.0
    assert.commandWorked(primaryDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

    // Make sure the secondary has FCV 4.0
    assert.soon(() => {
        let res = secondaryDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.commandWorked(res);
        return res.featureCompatibilityVersion.version == "4.0";
    });

    // Continue index build on the secondary. There should be no KeyTooLong error.
    assert.commandWorked(
        secondaryDB.adminCommand({configureFailPoint: "hangAfterStartingIndexBuild", mode: "off"}));

    // Make sure the index is successfully created.
    assert.soon(() => {
        return secondaryDB[collName].getIndexes().length == 2;
    });

    const signal = true;  // Use default kill signal.
    const forRestart = false;
    rst.stopSet(signal, forRestart, {skipValidation: true});
}());
