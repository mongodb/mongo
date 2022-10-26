// Test that currentOp works as expected in a multitenant environment.
// @tags: [requires_fcv_62, featureFlagRequireTenantID]
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');     // For arrayEq()
load("jstests/libs/fail_point_util.js");         // For configureFailPoint()
load("jstests/libs/parallel_shell_helpers.js");  // For funWithArgs()

const kTenant = ObjectId();
const kOtherTenant = ObjectId();
const kDbName = 'myDb';
const kNewCollectionName = "currOpColl";

function assertCurrentOpAggOutput(
    tokenConn, rootConn, tenantId, dbName, expectedBatchSize, featureFlagRequireTenantId) {
    // Check for the 'insert' op in the currOp output for 'tenantId' when issuing '$currentOp' in
    // aggregation pipeline with a security token.
    const aggTokenTenant = tokenConn.getDB("admin").runCommand({
        aggregate: 1,
        pipeline: [{$currentOp: {allUsers: false}}, {$match: {op: "insert"}}],
        cursor: {}
    });
    assert.eq(aggTokenTenant.cursor.firstBatch.length, expectedBatchSize, tojson(aggTokenTenant));
    if (expectedBatchSize > 0) {
        checkNsSerializedCorrectly(featureFlagRequireTenantId,
                                   tenantId,
                                   dbName,
                                   kNewCollectionName,
                                   aggTokenTenant.cursor.firstBatch[0]);
    }

    // Check for the 'insert' op in the currOp output for 'tenantId' when issuing '$currentOp' in
    // aggregation pipeline and passing '$tenant' to it.
    const aggDollarTenant = rootConn.runCommand({
        aggregate: 1,
        pipeline: [{$currentOp: {allUsers: true}}, {$match: {op: "insert"}}],
        cursor: {},
        '$tenant': tenantId
    });
    assert.eq(aggDollarTenant.cursor.firstBatch.length, expectedBatchSize, tojson(aggDollarTenant));
    if (expectedBatchSize > 0) {
        checkNsSerializedCorrectly(featureFlagRequireTenantId,
                                   tenantId,
                                   dbName,
                                   kNewCollectionName,
                                   aggDollarTenant.cursor.firstBatch[0]);
    }
}

function assertCurrentOpCommandOutput(
    tokenConn, rootConn, tenantId, dbName, expectedBatchSize, featureFlagRequireTenantId) {
    // Check for the 'insert' op in the currOp output for 'tenantId' when issuing the currentOp
    // command with a security token.
    const cmdTokenTenant = tokenConn.getDB("admin").runCommand(
        {currentOp: 1, $ownOps: true, $all: true, op: "insert"});
    assert.eq(cmdTokenTenant.inprog.length, expectedBatchSize, tojson(cmdTokenTenant));
    if (expectedBatchSize > 0) {
        const cmdOp = cmdTokenTenant.inprog[0];
        assert.eq(cmdOp.command.insert, kNewCollectionName);
        checkNsSerializedCorrectly(
            featureFlagRequireTenantId, tenantId, dbName, kNewCollectionName, cmdOp);
    }

    // Check for the 'insert' op in the currOp output for 'tenantId' when issuing the currentOp
    // command with $tenant.
    const cmdDollarTenant = rootConn.runCommand(
        {currentOp: 1, $ownOps: false, $all: true, op: "insert", '$tenant': tenantId});
    assert.eq(cmdDollarTenant.inprog.length, expectedBatchSize, tojson(cmdDollarTenant));
    if (expectedBatchSize > 0) {
        const cmdOp = cmdDollarTenant.inprog[0];
        assert.eq(cmdOp.command.insert, kNewCollectionName);
        checkNsSerializedCorrectly(
            featureFlagRequireTenantId, tenantId, dbName, kNewCollectionName, cmdOp);
    }
}

function checkNsSerializedCorrectly(
    featureFlagRequireTenantId, kTenantId, dbName, collectionName, op) {
    if (featureFlagRequireTenantId) {
        // This case represents the upgraded state where we will not include the tenantId as the
        // db prefix.
        assert.eq(op.ns, dbName + "." + collectionName);
        assert.eq(op.command.$db, dbName);
    } else {
        // This case represents the downgraded state where we will continue to prefix namespaces.
        const prefixedDb = kTenant + "_" + kDbName;
        assert.eq(op.ns, prefixedDb + "." + collectionName);
        // TODO SERVER-70053 Uncomment this line.
        // assert.eq(op.command.$db, kTenantId + "_" + dbName);
    }
}

function runTest(featureFlagRequireTenantId) {
    const rst = new ReplSetTest({
        nodes: 1,
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

    const primary = rst.getPrimary();
    const adminDb = primary.getDB('admin');

    // Must be authenticated as a user with ActionType::useTenant in order to use $tenant.
    assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
    assert(adminDb.auth('admin', 'pwd'));

    // Create a non-privileged user for later use.
    assert.commandWorked(
        adminDb.runCommand({createUser: 'dbAdmin', pwd: 'pwd', roles: ['dbAdminAnyDatabase']}));

    const securityToken =
        _createSecurityToken({user: "userTenant1", db: '$external', tenant: kTenant});
    assert.commandWorked(primary.getDB('$external').runCommand({
        createUser: "userTenant1",
        '$tenant': kTenant,
        roles:
            [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
    }));

    const securityTokenOtherTenant =
        _createSecurityToken({user: "userTenant2", db: '$external', tenant: kOtherTenant});
    assert.commandWorked(primary.getDB('$external').runCommand({
        createUser: "userTenant2",
        '$tenant': kOtherTenant,
        roles:
            [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
    }));

    // Set the security token for kTenant on the connection to start.
    const tokenConn = new Mongo(primary.host);
    tokenConn._setSecurityToken(securityToken);

    // Run an insert for kTenant that will implicitly create a collection, and force the collection
    // creation to hang so that we'll capture it in the currentOp output. Turn on a failpoint to
    // force the create to hang, and wait to hit the failpoint.
    const createCollFP = configureFailPoint(primary, "hangBeforeLoggingCreateCollection");
    function runCreate(securityToken, dbName, kNewCollectionName) {
        db.getMongo()._setSecurityToken(securityToken);
        assert.commandWorked(db.getSiblingDB(dbName).runCommand(
            {insert: kNewCollectionName, documents: [{_id: 0}]}));
    }
    const createShell = startParallelShell(
        funWithArgs(runCreate, securityToken, kDbName, kNewCollectionName), primary.port);
    createCollFP.wait();

    // Check that the 'insert' op shows up in the currOp output for 'kTenant' when issuing
    // '$currentOp' in aggregation pipeline.
    assertCurrentOpAggOutput(tokenConn,
                             adminDb,
                             kTenant,
                             kDbName,
                             1 /* expectedBatchSize */,
                             featureFlagRequireTenantId);

    // Check that the 'insert' op shows up in the currOp output for 'kTenant' when issuing
    // the currentOp command.
    assertCurrentOpCommandOutput(tokenConn,
                                 adminDb,
                                 kTenant,
                                 kDbName,
                                 1 /* expectedBatchSize */,
                                 featureFlagRequireTenantId);

    // Check that the other tenant does not see the op in currentOp output.
    tokenConn._setSecurityToken(securityTokenOtherTenant);
    assertCurrentOpAggOutput(tokenConn,
                             adminDb,
                             kOtherTenant,
                             kDbName,
                             0 /* expectedBatchSize */,
                             featureFlagRequireTenantId);
    assertCurrentOpCommandOutput(tokenConn,
                                 adminDb,
                                 kOtherTenant,
                                 kDbName,
                                 0 /* expectedBatchSize */,
                                 featureFlagRequireTenantId);

    // Now check that a privileged user can see this op using both $currentOp and the currentOp
    // command when no tenantId is provided.
    const currOpAggResPrivilegedUser = adminDb.runCommand({
        aggregate: 1,
        pipeline: [{$currentOp: {allUsers: true}}, {$match: {op: "insert"}}],
        cursor: {}
    });
    assert.eq(
        currOpAggResPrivilegedUser.cursor.firstBatch.length, 1, tojson(currOpAggResPrivilegedUser));

    const currOpCmdResPrivilegedUser =
        adminDb.runCommand({currentOp: 1, $ownOps: false, $all: true, op: "insert"});
    assert.eq(currOpCmdResPrivilegedUser.inprog.length, 1, tojson(currOpCmdResPrivilegedUser));

    // Check that a user without required privileges, i.e. not associated with kTenant, is not
    // authorized to issue '$currentOp' for other users.
    const nonPrivilegedConn = new Mongo(primary.host);
    assert(nonPrivilegedConn.getDB("admin").auth('dbAdmin', 'pwd'));

    assert.commandFailedWithCode(nonPrivilegedConn.getDB("admin").runCommand({
        aggregate: 1,
        pipeline: [{$currentOp: {allUsers: true}}, {$match: {op: "insert"}}],
        cursor: {}
    }),
                                 ErrorCodes.Unauthorized);

    // Check that a user without required privileges, i.e. not associated with kTenant, is not
    // authorized to issue the currentOp command for other users.
    assert.commandFailedWithCode(nonPrivilegedConn.getDB("admin").runCommand(
                                     {currentOp: 1, $ownOps: false, $all: true, op: "insert"}),
                                 ErrorCodes.Unauthorized);

    // Turn the failpoint off and wait for the create to finish.
    createCollFP.off();
    createShell();

    rst.stopSet();
}

runTest(true);
runTest(false);
})();
