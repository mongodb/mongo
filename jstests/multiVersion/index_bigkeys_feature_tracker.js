/**
 * Test feature tracker bit indicating the existence of long TypeBits.
 * Intentionally don't test unique index because 4.2 and 4.0 have different unique index formats and
 * we already have other mechanism to make 4.0 fail to start up if there are 4.2 unique indexes on
 * disk.
 * TODO SERVER-36385: Remove this test in the master branch once we have created a 4.2 branch.
 */
(function() {
"use strict";

const collName = "index_bigkeys";

function insertIndexKey(createIndexFirst, db, collName, docToInsert, backgroundIndexBuild, update) {
    if (createIndexFirst) {
        assert.commandWorked(db.runCommand({
            createIndexes: collName,
            indexes: [{key: {x: 1}, name: "x_1", background: backgroundIndexBuild}]
        }));
        if (update) {
            assert.commandWorked(
                db.runCommand({insert: collName, documents: [{_id: docToInsert._id, x: 1}]}));
            assert.commandWorked(db.runCommand(
                {update: collName, updates: [{q: {_id: docToInsert._id}, u: {x: docToInsert}}]}));
        } else {
            // This will insert a feature tracker bit on disk.
            assert.commandWorked(db.runCommand({insert: collName, documents: [docToInsert]}));
        }
    } else {
        if (update) {
            assert.commandWorked(
                db.runCommand({insert: collName, documents: [{_id: docToInsert._id, x: 1}]}));
            assert.commandWorked(db.runCommand(
                {update: collName, updates: [{q: {_id: docToInsert._id}, u: {x: docToInsert}}]}));
        } else {
            // This will insert a feature tracker bit on disk.
            assert.commandWorked(db.runCommand({insert: collName, documents: [docToInsert]}));
        }
        // This will insert a feature tracker bit on disk.
        assert.commandWorked(db.runCommand({
            createIndexes: collName,
            indexes: [{key: {x: 1}, name: "x_1", background: backgroundIndexBuild}]
        }));
    }
}

function logTestParameters(
    docToInsert, shouldFailOnStartup, createIndexFirst, backgroundIndexBuild, update) {
    let output = {
        "docToInsert._id": docToInsert._id,
        shouldFailOnStartup: shouldFailOnStartup,
        createIndexFirst: createIndexFirst,
        backgroundIndexBuild: backgroundIndexBuild,
        update: update
    };
    jsTestLog("Testing with parameters: " + tojson(output));
}

const dbpath = MongoRunner.dataPath + "index_bigkeys_feature_tracker";

function testInsertIndexKeyAndDowngradeStandalone(
    docToInsert, shouldFailOnStartup, createIndexFirst, backgroundIndexBuild, update) {
    logTestParameters(
        docToInsert, shouldFailOnStartup, createIndexFirst, backgroundIndexBuild, update);
    const conn = MongoRunner.runMongod({binVersion: "latest", dbpath: dbpath});

    insertIndexKey(
        createIndexFirst, conn.getDB("test"), collName, docToInsert, backgroundIndexBuild, update);

    // Downgrade the FCV to 4.0
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

    // Index validation would fail because validation code assumes big index keys are not
    // indexed in FCV 4.0.
    MongoRunner.stopMongod(conn, null, {skipValidation: true});

    if (shouldFailOnStartup) {
        //  4.0 binary should fail on start up due to the new feature tracker bit.
        assert.eq(null,
                  MongoRunner.runMongod({binVersion: "4.0", noCleanData: true, dbpath: dbpath}));
    } else {
        const conn = MongoRunner.runMongod({binVersion: "4.0", noCleanData: true, dbpath: dbpath});
        assert.neq(null, conn);
        MongoRunner.stopMongod(conn, null, {skipValidation: true});
    }
}

function testInsertIndexKeyAndDowngradeReplset(
    docToInsert, shouldFailOnStartup, createIndexFirst, backgroundIndexBuild, update) {
    logTestParameters(
        docToInsert, shouldFailOnStartup, createIndexFirst, backgroundIndexBuild, update);
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();

    insertIndexKey(createIndexFirst,
                   primary.getDB("test"),
                   collName,
                   docToInsert,
                   backgroundIndexBuild,
                   update);

    // Downgrade the FCV to 4.0
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

    // Index validation would fail because validation code assumes big index keys are not
    // indexed in FCV 4.0.
    rst.stopSet(undefined, undefined, {noCleanData: true, skipValidation: true});

    if (shouldFailOnStartup) {
        //  4.0 binary should fail on start up due to the new feature tracker bit.
        assert.throws(function() {
            rst.start(0, {binVersion: "4.0", noCleanData: true}, true);
        });
    } else {
        const conn = rst.start(0, {binVersion: "4.0", noCleanData: true}, true);
        assert.neq(null, conn);
        rst.stopSet(undefined, undefined, {skipValidation: true});
    }
}

const largeKeyWithShortTypeBits = 's'.repeat(12345);
const largeKeyWithLongTypeBits = (() => {
    // {a : [0,1,2, ... ,9999] }
    return {a: Array.from({length: 10000}, (value, i) => i)};
})();

// Tests for standalone
jsTestLog("Test for standalone");
[true, false].forEach(function(backgroundIndexBuild) {
    [true, false].forEach(function(createIndexFirst) {
        [true, false].forEach(function(update) {
            testInsertIndexKeyAndDowngradeStandalone(
                {_id: "shortTypeBits", x: largeKeyWithShortTypeBits},
                false,
                createIndexFirst,
                backgroundIndexBuild,
                update);
            testInsertIndexKeyAndDowngradeStandalone(
                {_id: "longTypeBits", x: largeKeyWithLongTypeBits},
                true,
                createIndexFirst,
                backgroundIndexBuild,
                update);
        });
    });
});

// Tests for replset
jsTestLog("Test for replset");
[true, false].forEach(function(backgroundIndexBuild) {
    [true, false].forEach(function(createIndexFirst) {
        [true, false].forEach(function(update) {
            testInsertIndexKeyAndDowngradeReplset(
                {_id: "shortTypeBits", x: largeKeyWithShortTypeBits},
                false,
                createIndexFirst,
                backgroundIndexBuild,
                update);
            testInsertIndexKeyAndDowngradeReplset(
                {_id: "longTypeBits", x: largeKeyWithLongTypeBits},
                true,
                createIndexFirst,
                backgroundIndexBuild,
                update);
        });
    });
});
}());
