/*
 * This test exercises interactions between upgrade/downgrade, retryable writes and $v: 2 "delta"
 * oplog entries. We check that updates which use the $v: 2 oplog entries can be retried
 * across different values of the FCV flag and across binaries of different versions.
 *
 * The test has two parts:
 * 1) Checks that an update which used the new style of oplog entry can be retried across a
 * downgrade. While retrying the update requires the node to inspect the associated oplog entry, it
 * should make no attempt to parse the contents of the 'o' field. This is important, because 4.4
 * mongod binaries cannot parse $v: 2 oplog entries.
 *
 * 2) Checks that an update can be retried across an upgrade. This is a less interesting case, but
 * is a good sanity check to have.
 */
(function() {
const kCollName = "downgrade_coll";
const kGiantStr = "x".repeat(100);

const kTests = [
    // findAndModify which will return the pre image of the modification (new=false).
    {
        initialDoc: {_id: "pipelineFAM0", a: 0, padding: kGiantStr},
        cmd: {
            findAndModify: kCollName,
            query: {_id: "pipelineFAM0"},
            update: [{$set: {b: 4, a: {$add: ["$a", 1]}}}],
            "new": false,
            txnNumber: NumberLong(0),
            lsid: {id: UUID()}
        },
        checkPostImage: function(doc) {
            assert.eq(doc, {_id: "pipelineFAM0", a: 1, padding: kGiantStr, b: 4});
        },
        checkCmdResponse: function(response) {
            assert.eq(response.value, {_id: "pipelineFAM0", a: 0, padding: kGiantStr});
        },
        oplogEntryVersion: 2,
    },

    // findAndModify which will return the post image of the update (new=true).
    {
        initialDoc: {_id: "pipelineFAM1", a: 0, padding: kGiantStr},
        cmd: {
            findAndModify: kCollName,
            query: {_id: "pipelineFAM1"},
            update: [{$set: {b: 4, a: {$add: ["$a", 1]}}}],
            "new": true,
            txnNumber: NumberLong(0),
            lsid: {id: UUID()}
        },
        checkPostImage: function(doc) {
            assert.eq(doc, {_id: "pipelineFAM1", a: 1, padding: kGiantStr, b: 4});
        },
        checkCmdResponse: function(response) {
            assert.eq(response.value, {_id: "pipelineFAM1", a: 1, padding: kGiantStr, b: 4});
        },
        oplogEntryVersion: 2,
    },
    {
        initialDoc: {_id: "pipelineUpdate", a: 0, padding: kGiantStr},
        cmd: {
            update: kCollName,
            updates: [{q: {_id: "pipelineUpdate"}, u: [{$set: {a: {$add: ["$a", 2]}}}]}],
            txnNumber: NumberLong(0),
            lsid: {id: UUID()}
        },
        checkPostImage: function(doc) {
            assert.eq(doc, {_id: "pipelineUpdate", a: 2, padding: kGiantStr});
        },
        oplogEntryVersion: 2,
    },

    {
        initialDoc: {_id: "pipelineUpdateWhichForcesReplacement", a: 0, padding: kGiantStr},
        cmd: {
            update: kCollName,
            updates: [{
                q: {_id: "pipelineUpdateWhichForcesReplacement"},
                u: [{$replaceWith: {a: 1, b: 1, str: kGiantStr}}]
            }],
            txnNumber: NumberLong(0),
            lsid: {id: UUID()}
        },
        checkPostImage: function(doc) {
            assert.eq(doc,
                      {_id: "pipelineUpdateWhichForcesReplacement", a: 1, b: 1, str: kGiantStr});
        },
        oplogEntryVersion: null
    },

    // Test some modifier-style updates as well.
    {
        initialDoc: {_id: "modifierUpdate0", a: 0},
        cmd: {
            findAndModify: kCollName,
            query: {_id: "modifierUpdate0"},
            update: {$set: {b: 4}, $inc: {a: 1}},
            "new": false,
            txnNumber: NumberLong(0),
            lsid: {id: UUID()}
        },
        checkPostImage: function(doc) {
            assert.eq(doc, {_id: "modifierUpdate0", a: 1, b: 4});
        },
        oplogEntryVersion: 1
    },
    {
        initialDoc: {_id: "modifierUpdate1", x: 0},
        cmd: {
            findAndModify: kCollName,
            query: {_id: "modifierUpdate1"},
            update: {$set: {"b.c": 4}, $unset: {x: false}},
            "new": false,
            txnNumber: NumberLong(0),
            lsid: {id: UUID()}
        },
        checkPostImage: function(doc) {
            assert.eq(doc, {_id: "modifierUpdate1", b: {c: 4}});
        },
        oplogEntryVersion: 1
    },
];

// Given a handle to the test database on the primary, ensure that re-running the test
// operations succeeds, and that the documents associated with each operation have the correct
// values.
function testRetriesSucceed(primaryDB) {
    for (let test of kTests) {
        const response = assert.commandWorked(primaryDB.runCommand(test.cmd));

        if (test.checkCmdResponse) {
            test.checkCmdResponse(response);
        }

        // Check that the post image is correct.
        const postImage = primaryDB[kCollName].findOne({_id: test.initialDoc._id});
        test.checkPostImage(postImage);
    }
}

/**
 * Given a two node ReplSetTest, sets the feature flag which enables V2 oplog entries on the
 * primary and secondary.
 */
function enableV2OplogEntries(rst) {
    const cmd = {setParameter: 1, internalQueryEnableLoggingV2OplogEntries: true};
    assert.commandWorked(rst.getPrimary().adminCommand(cmd));
    assert.commandWorked(rst.getSecondary().adminCommand(cmd));
}

const rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: "latest", noCleanData: true}});

jsTestLog("Running downgrade test");

// Check that an operation which resulted in a $v: 2 oplog entry being logged can be retried
// across a downgrade of FCV and a downgrade of binary.
(function runDowngradeTest() {
    // Start a latest replica set, run some inserts, run some retryable operations, downgrade the
    // FCV to 4.4, and then retry them.
    (function startLatestRSAndInsertData() {
        rst.startSet();
        rst.initiate();

        enableV2OplogEntries(rst);

        const primaryDB = rst.getPrimary().getDB("test");
        const coll = primaryDB[kCollName];

        // Seed the collection.
        for (let test of kTests) {
            assert.commandWorked(coll.insert(test.initialDoc));
        }

        // Run the retryable operation.
        for (let test of kTests) {
            const response = assert.commandWorked(primaryDB.runCommand(test.cmd));

            // Check that the oplog entry uses the correct format.
            const oplogRes = rst.getPrimary()
                                 .getDB("local")["oplog.rs"]
                                 .find({"o2._id": test.initialDoc._id})
                                 .hint({$natural: -1})
                                 .limit(1)
                                 .toArray();
            assert.eq(oplogRes.length, 1);
            if (test.oplogEntryVersion) {
                assert.eq(oplogRes[0].o.$v, test.oplogEntryVersion, oplogRes);
            }

            if (test.checkCmdResponse) {
                test.checkCmdResponse(response);
            }

            // Check that the post image is correct.
            const postImage = coll.findOne({_id: test.initialDoc._id});
            test.checkPostImage(postImage);
        }

        // Downgrade FCV.
        assert.commandWorked(primaryDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
        checkFCV(primaryDB.getSiblingDB("admin"), lastLTSFCV);
        // Retry the operations, ensure they succeed and check that the writes are not performed
        // again.
        testRetriesSucceed(primaryDB);

        rst.awaitReplication();
        rst.stopSet(
            null,  // signal
            true   // for restart
        );
    })();

    // This will start a "last-lts" replica set using the same data files as the "latest" replica
    // set started above. It will then retry the operations in each test case again. The operations
    // should indicate success even if the last-lts binaries are not be able to parse the 'o'
    // field in the oplog entries associated with each operation.
    //
    // Then, this will add a new node to the set and check that it can initial sync without issue.
    (function startLastStableRSAndRetryOperations() {
        rst.startSet({restart: true, binVersion: "last-lts"});

        const primaryDB = rst.getPrimary().getDB("test");
        testRetriesSucceed(primaryDB);
        rst.awaitReplication();

        rst.stopSet();
    })();
})();

jsTestLog("Running upgrade test");

// Test that pipeline updates which would use $v: 2 oplog entries can be retried across an
// upgrade.
// NOTE: This will restart the repl set with fresh data files.
(function runUpgradeTest() {
    (function startLastStableRSAndRunOperations() {
        rst.startSet({binVersion: "last-lts"});
        rst.initiate();

        const primaryDB = rst.getPrimary().getDB("test");
        const coll = primaryDB[kCollName];

        for (let test of kTests) {
            assert.commandWorked(coll.insert(test.initialDoc));
        }

        // Run the retryable operations twice.
        testRetriesSucceed(primaryDB);
        testRetriesSucceed(primaryDB);

        rst.stopSet(
            null,  // signal
            true   // for restart
        );
    })();

    (function startLatestRSAndRetryOperations() {
        rst.startSet({restart: true, binVersion: "latest"});

        const primaryTestDB = rst.getPrimary().getDB("test");
        const primaryAdminDB = primaryTestDB.getSiblingDB("admin");

        // Retry the operations under the old FCV.
        checkFCV(primaryAdminDB, lastLTSFCV);
        testRetriesSucceed(primaryTestDB);

        // Upgrade to the new FCV and retry the operations again.
        assert.commandWorked(
            primaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(primaryTestDB.getSiblingDB("admin"), latestFCV);
        testRetriesSucceed(primaryTestDB);

        // Make sure we can add a secondary to the set without issue.
        const newSecondary = rst.add({binVersion: "latest"});
        rst.reInitiate();

        // As a sanity check, enable V2 oplog entries and retry the operations again.
        enableV2OplogEntries(rst);
        testRetriesSucceed(primaryTestDB);

        rst.stopSet();
    })();
})();
})();
