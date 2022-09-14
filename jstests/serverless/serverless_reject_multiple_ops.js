/**
 * Tests that shard split operations cannot be started simultaneously with a tenant migration or
 * shard merge.
 *
 * @tags: [
 *   serverless,
 *   requires_fcv_52,
 *   featureFlagShardSplit,
 *   featureFlagShardMerge
 * ]
 */

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/replsets/rslib.js");
load("jstests/serverless/libs/basic_serverless_test.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");

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

function cannotStartShardSplitWithMigrationInProgress({protocol, runOnRecipient}) {
    // Test that we cannot start a shard split while a migration is in progress.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    const shardSplitRst = runOnRecipient ? test.getRecipientRst() : test.getDonorRst();

    let recipientNodes = addRecipientNodes(shardSplitRst, recipientTagName);

    let fp = configureFailPoint(test.getDonorRst().getPrimary(),
                                "pauseTenantMigrationBeforeLeavingDataSyncState");
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0];
    }
    jsTestLog("Starting tenant migration");
    assert.commandWorked(test.startMigration(migrationOpts));

    fp.wait();

    const commitThread = commitSplitAsync(
        shardSplitRst, tenantIds, recipientTagName, recipientSetName, splitMigrationId);
    assert.commandFailed(commitThread.returnData());

    fp.off();

    TenantMigrationTest.assertCommitted(
        waitForMergeToComplete(migrationOpts, tenantMigrationId, test));
    assert.commandWorked(test.forgetMigration(migrationOpts.migrationIdString));

    recipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    test.stop();
    jsTestLog("cannotStartShardSplitWithMigrationInProgress test completed");
}

function canStartShardSplitWithAbortedMigration({protocol, runOnRecipient}) {
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: false, sharedOptions});

    const shardSplitRst = runOnRecipient ? test.getRecipientRst() : test.getDonorRst();

    let recipientNodes = addRecipientNodes(shardSplitRst, recipientTagName);

    let fp = configureFailPoint(test.getDonorRst().getPrimary(),
                                "abortTenantMigrationBeforeLeavingBlockingState");
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0];
    }
    jsTestLog("Starting tenant migration");
    assert.commandWorked(test.startMigration(migrationOpts));

    TenantMigrationTest.assertAborted(
        waitForMergeToComplete(migrationOpts, tenantMigrationId, test));
    assert.commandWorked(test.forgetMigration(migrationOpts.migrationIdString));

    const commitThread = commitSplitAsync(
        shardSplitRst, tenantIds, recipientTagName, recipientSetName, splitMigrationId);
    assert.commandWorked(commitThread.returnData());

    recipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    test.stop();
    jsTestLog("cannotStartShardSplitWithMigrationInProgress test completed");
}

function cannotStartMigrationWhileShardSplitIsInProgress(protocol) {
    // Test that we cannot start a migration while a shard split is in progress.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    const splitRst = test.getDonorRst();

    let splitRecipientNodes = addRecipientNodes(splitRst, recipientTagName);

    let fp = configureFailPoint(splitRst.getPrimary(), "pauseShardSplitBeforeBlockingState");

    const commitThread =
        commitSplitAsync(splitRst, tenantIds, recipientTagName, recipientSetName, splitMigrationId);
    fp.wait();

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0];
    }
    jsTestLog("Starting tenant migration");
    assert.commandFailedWithCode(test.startMigration(migrationOpts),
                                 ErrorCodes.ConflictingServerlessOperation);

    fp.off();

    assert.commandWorked(commitThread.returnData());

    splitRst.nodes = splitRst.nodes.filter(node => !splitRecipientNodes.includes(node));
    splitRst.ports =
        splitRst.ports.filter(port => !splitRecipientNodes.some(node => node.port === port));

    assert.commandWorked(
        splitRst.getPrimary().adminCommand({forgetShardSplit: 1, migrationId: splitMigrationId}));

    splitRecipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    waitForGarbageCollectionForSplit(splitRst.nodes, splitMigrationId, tenantIds);

    test.stop();
    jsTestLog("cannotStartMigrationWhileShardSplitIsInProgress test completed");
}

function cannotStartMigrationWhileShardSplitIsInProgressOnRecipient(protocol) {
    // Test that we cannot start a migration while a shard split is in progress.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    const splitRst = test.getRecipientRst();

    let splitRecipientNodes = addRecipientNodes(splitRst, recipientTagName);

    let fp = configureFailPoint(splitRst.getPrimary(), "pauseShardSplitBeforeBlockingState");

    const commitThread =
        commitSplitAsync(splitRst, tenantIds, recipientTagName, recipientSetName, splitMigrationId);
    fp.wait();

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0];
    }
    jsTestLog("Starting tenant migration");
    assert.commandWorked(test.startMigration(migrationOpts));

    const result = assert.commandWorked(test.waitForMigrationToComplete(migrationOpts));
    assert.eq(result.state, "aborted");
    assert.eq(result.abortReason.code, ErrorCodes.ConflictingServerlessOperation);

    fp.off();

    assert.commandWorked(commitThread.returnData());

    splitRst.nodes = splitRst.nodes.filter(node => !splitRecipientNodes.includes(node));
    splitRst.ports =
        splitRst.ports.filter(port => !splitRecipientNodes.some(node => node.port === port));

    assert.commandWorked(
        splitRst.getPrimary().adminCommand({forgetShardSplit: 1, migrationId: splitMigrationId}));

    splitRecipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    waitForGarbageCollectionForSplit(splitRst.nodes, splitMigrationId, tenantIds);

    test.stop();
    jsTestLog("cannotStartMigrationWhileShardSplitIsInProgress test completed");
}

function cannotStartMigrationWithDifferentTenantWhileShardSplitIsInProgress(protocol) {
    // Test that we cannot start a tenant migration while a shard split is in progress. Use a
    // tenantId uninvolved in the split.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    let recipientNodes = addRecipientNodes(test.getDonorRst(), recipientTagName);

    let fp =
        configureFailPoint(test.getDonorRst().getPrimary(), "pauseShardSplitBeforeBlockingState");

    const commitThread = commitSplitAsync(
        test.getDonorRst(), tenantIds, recipientTagName, recipientSetName, splitMigrationId);
    fp.wait();

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = "otherTenantToMove";
    }
    jsTestLog("Starting tenant migration");
    assert.commandFailedWithCode(test.startMigration(migrationOpts),
                                 ErrorCodes.ConflictingServerlessOperation);

    fp.off();

    assert.commandWorked(commitThread.returnData());

    test.getDonorRst().nodes =
        test.getDonorRst().nodes.filter(node => !recipientNodes.includes(node));
    test.getDonorRst().ports =
        test.getDonorRst().ports.filter(port => !recipientNodes.some(node => node.port === port));

    assert.commandWorked(test.getDonorRst().getPrimary().adminCommand(
        {forgetShardSplit: 1, migrationId: splitMigrationId}));

    recipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    waitForGarbageCollectionForSplit(test.getDonorRst().nodes, splitMigrationId, tenantIds);

    test.stop();
    jsTestLog("cannotStartMigrationWithDifferentTenantWhileShardSplitIsInProgress test completed");
}

function cannotStartMigrationWhenThereIsAnExistingAccessBlocker(protocol) {
    // Test that we cannot start a tenant migration for a tenant that already has an access blocker.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    // Ensure a high enough delay so the shard split document is not deleted before tenant migration
    // is started.
    sharedOptions = {};
    sharedOptions["setParameter"] = {
        shardSplitGarbageCollectionDelayMS: 36000000,
        ttlMonitorSleepSecs: 1
    };

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    let recipientNodes = addRecipientNodes(test.getDonorRst(), recipientTagName);

    const commitThread = commitSplitAsync(
        test.getDonorRst(), tenantIds, recipientTagName, recipientSetName, splitMigrationId);
    assert.commandWorked(commitThread.returnData());

    // Remove recipient nodes
    test.getDonorRst().nodes =
        test.getDonorRst().nodes.filter(node => !recipientNodes.includes(node));
    test.getDonorRst().ports =
        test.getDonorRst().ports.filter(port => !recipientNodes.some(node => node.port === port));

    assert.commandWorked(test.getDonorRst().getPrimary().adminCommand(
        {forgetShardSplit: 1, migrationId: splitMigrationId}));

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0];
    }
    assert.commandFailed(test.startMigration(migrationOpts));

    recipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    test.stop();
    jsTestLog("cannotStartMigrationWhenThereIsAnExistingAccessBlocker test completed");
}

function canStartMigrationAfterSplitGarbageCollection(protocol) {
    // Test that we can start a migration after a shard split has been garbage collected.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    let recipientNodes = addRecipientNodes(test.getDonorRst(), recipientTagName);

    const commitThread = commitSplitAsync(
        test.getDonorRst(), tenantIds, recipientTagName, recipientSetName, splitMigrationId);
    assert.commandWorked(commitThread.returnData());

    // Remove recipient nodes
    test.getDonorRst().nodes =
        test.getDonorRst().nodes.filter(node => !recipientNodes.includes(node));
    test.getDonorRst().ports =
        test.getDonorRst().ports.filter(port => !recipientNodes.some(node => node.port === port));

    assert.commandWorked(test.getDonorRst().getPrimary().adminCommand(
        {forgetShardSplit: 1, migrationId: splitMigrationId}));

    waitForGarbageCollectionForSplit(test.getDonorRst().nodes, splitMigrationId, tenantIds);

    jsTestLog("Starting tenant migration");
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0];
    }
    assert.commandWorked(test.startMigration(migrationOpts));

    TenantMigrationTest.assertCommitted(test.waitForMigrationToComplete(migrationOpts));
    assert.commandWorked(test.forgetMigration(migrationOpts.migrationIdString));

    recipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    test.stop();
    jsTestLog("canStartMigrationAfterSplitGarbageCollection test completed");
}

canStartShardSplitWithAbortedMigration({protocol: "multitenant migrations", runOnRecipient: false});
canStartShardSplitWithAbortedMigration({protocol: "shard merge", runOnRecipient: false});

cannotStartShardSplitWithMigrationInProgress(
    {protocol: "multitenant migrations", runOnRecipient: false});
cannotStartShardSplitWithMigrationInProgress({protocol: "shard merge", runOnRecipient: false});

cannotStartShardSplitWithMigrationInProgress(
    {protocol: "multitenant migrations", runOnRecipient: true});
cannotStartShardSplitWithMigrationInProgress({protocol: "shard merge", runOnRecipient: true});

cannotStartMigrationWhileShardSplitIsInProgress("multitenant migrations");
cannotStartMigrationWhileShardSplitIsInProgress("shard merge");

cannotStartMigrationWhileShardSplitIsInProgressOnRecipient("multitenant migrations");
cannotStartMigrationWhileShardSplitIsInProgressOnRecipient("shard merge");

cannotStartMigrationWithDifferentTenantWhileShardSplitIsInProgress("multitenant migrations");
cannotStartMigrationWithDifferentTenantWhileShardSplitIsInProgress("shard merge");

cannotStartMigrationWhenThereIsAnExistingAccessBlocker("multitenant migrations");
cannotStartMigrationWhenThereIsAnExistingAccessBlocker("shard merge");

canStartMigrationAfterSplitGarbageCollection("multitenant migrations");
canStartMigrationAfterSplitGarbageCollection("shard merge");
