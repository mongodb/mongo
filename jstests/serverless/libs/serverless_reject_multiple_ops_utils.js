/**
 * Utility functions for serverless_reject_multiple_ops tests
 *
 * @tags: [
 *   serverless,
 *   requires_fcv_52,
 *   featureFlagShardSplit,
 *   featureFlagShardMerge
 * ]
 */

load("jstests/replsets/rslib.js");
load("jstests/libs/parallelTester.js");

function waitForMergeToComplete(migrationOpts, migrationId, test) {
    // Assert that the migration has already been started.
    assert(test.getDonorPrimary().getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({
        _id: migrationId
    }));

    const donorStartReply = test.runDonorStartMigration(
        migrationOpts, {waitForMigrationToComplete: true, retryOnRetryableErrors: false});

    return donorStartReply;
}

function commitSplitAsync(rst, tenantIds, recipientTagName, recipientSetName, migrationId) {
    jsTestLog("Running commitAsync command");

    const rstArgs = createRstArgs(rst);
    const migrationIdString = extractUUIDFromObject(migrationId);

    const thread = new Thread(runCommitSplitThreadWrapper,
                              rstArgs,
                              migrationIdString,
                              tenantIds,
                              recipientTagName,
                              recipientSetName,
                              false /* enableDonorStartMigrationFsync */);
    thread.start();

    return thread;
}

function addRecipientNodes(rst, recipientTagName) {
    const numNodes = 3;  // default to three nodes
    let recipientNodes = [];
    const options = TenantMigrationUtil.makeX509OptionsForTest();
    jsTestLog(`Adding ${numNodes} non-voting recipient nodes to donor`);
    for (let i = 0; i < numNodes; ++i) {
        recipientNodes.push(rst.add(options.donor));
    }

    const primary = rst.getPrimary();
    const admin = primary.getDB('admin');
    const config = rst.getReplSetConfigFromNode();
    config.version++;

    // ensure recipient nodes are added as non-voting members
    recipientNodes.forEach(node => {
        config.members.push({
            host: node.host,
            votes: 0,
            priority: 0,
            hidden: true,
            tags: {[recipientTagName]: ObjectId().valueOf()}
        });
    });

    // reindex all members from 0
    config.members = config.members.map((member, idx) => {
        member._id = idx;
        return member;
    });

    assert.commandWorked(admin.runCommand({replSetReconfig: config}));
    recipientNodes.forEach(node => rst.waitForState(node, ReplSetTest.State.SECONDARY));

    return recipientNodes;
}
