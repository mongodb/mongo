// Test that currentOp works as expected in a multitenant environment.
// @tags: [requires_fcv_62]
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const kTenant = ObjectId();
const kOtherTenant = ObjectId();
const kDbName = 'myDb';
const kNewCollectionName = "currOpColl";
const kVTSKey = 'secret';

// Check for the 'insert' op(s) in the currOp output for 'tenantId' when issuing '$currentOp' in
// aggregation pipeline with a security token.
function assertCurrentOpAggOutputToken(tokenConn, dbName, expectedBatchSize) {
    // Security token users are not allowed to pass "allUsers: true" because it requires having the
    // "inprog" action type, which is only available to the "clusterMonitor" role. Security token
    // users should not be allowed this role.
    const res = tokenConn.getDB("admin").runCommand({
        aggregate: 1,
        pipeline: [{$currentOp: {allUsers: false}}, {$match: {op: "insert"}}],
        cursor: {}
    });
    assert.eq(res.cursor.firstBatch.length, expectedBatchSize, tojson(res));
    checkNsSerializedCorrectly(dbName, kNewCollectionName, res.cursor.firstBatch);
}

// Check for the 'insert' op(s) in the currOp output for 'tenantId' when issuing '$currentOp' in
// aggregation pipeline and passing '$tenant' to it.
function assertCurrentOpAggOutputDollarTenant(rootConn, tenantId, dbName, expectedBatchSize) {
    // We pass "allUsers: true" in order to see ops run by other users, including the security token
    // user. Passing $tenant will filter for only ops which belong to this tenant.
    const res = rootConn.runCommand({
        aggregate: 1,
        pipeline: [{$currentOp: {allUsers: true}}, {$match: {op: "insert"}}],
        cursor: {},
        '$tenant': tenantId
    });
    assert.eq(res.cursor.firstBatch.length, expectedBatchSize, tojson(res));
    checkNsSerializedCorrectly(dbName, kNewCollectionName, res.cursor.firstBatch);
}

// Check for the 'insert' op(s) in the currOp output for 'tenantId' when issuing the currentOp
// command with a security token.
function assertCurrentOpCommandOutputToken(tokenConn, dbName, expectedBatchSize) {
    const res = tokenConn.getDB("admin").runCommand(
        {currentOp: 1, $ownOps: true, $all: true, op: "insert"});
    assert.eq(res.inprog.length, expectedBatchSize, tojson(res));
    checkNsSerializedCorrectly(dbName, kNewCollectionName, res.inprog);
    res.inprog.forEach(op => {
        assert.eq(op.command.insert, kNewCollectionName);
    });
}

// Check for the 'insert' op in the currOp output for 'tenantId' when issuing the currentOp
// command with $tenant.
function assertCurrentOpCommandOutputDollarTenant(rootConn, tenantId, dbName, expectedBatchSize) {
    const res = rootConn.runCommand(
        {currentOp: 1, $ownOps: false, $all: true, op: "insert", '$tenant': tenantId});
    assert.eq(res.inprog.length, expectedBatchSize, tojson(res));
    checkNsSerializedCorrectly(dbName, kNewCollectionName, res.inprog);
    res.inprog.forEach(op => {
        assert.eq(op.command.insert, kNewCollectionName);
    });
}

function checkNsSerializedCorrectly(dbName, collectionName, cursorRes) {
    cursorRes.forEach(op => {
        assert.eq(op.ns, dbName + "." + collectionName);
        assert.eq(op.command.$db, dbName);
    });
}

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        auth: '',
        setParameter: {
            multitenancySupport: true,
            featureFlagSecurityToken: true,
            testOnlyValidatedTenancyScopeKey: kVTSKey,
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

const featureFlagRequireTenantId = FeatureFlagUtil.isEnabled(adminDb, "RequireTenantID");

// Create a non-privileged user for later use.
assert.commandWorked(
    adminDb.runCommand({createUser: 'dbAdmin', pwd: 'pwd', roles: ['dbAdminAnyDatabase']}));

const securityToken =
    _createSecurityToken({user: "userTenant1", db: '$external', tenant: kTenant}, kVTSKey);
assert.commandWorked(primary.getDB('$external').runCommand({
    createUser: "userTenant1",
    '$tenant': kTenant,
    roles: [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
}));

const securityTokenOtherTenant =
    _createSecurityToken({user: "userTenant2", db: '$external', tenant: kOtherTenant}, kVTSKey);
assert.commandWorked(primary.getDB('$external').runCommand({
    createUser: "userTenant2",
    '$tenant': kOtherTenant,
    roles: [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
}));

// Set the security token for kTenant on the connection to start.
const tokenConn = new Mongo(primary.host);
tokenConn._setSecurityToken(securityToken);

// Run an insert for kTenant using a security token that will implicitly create a collection. We
// force the collection creation to hang so that we'll capture the insert in the currentOp output.
{
    const createCollFP = configureFailPoint(primary, "hangBeforeLoggingCreateCollection");
    function runCreateToken(securityToken, dbName, kNewCollectionName) {
        db.getMongo()._setSecurityToken(securityToken);
        assert.commandWorked(db.getSiblingDB(dbName).runCommand(
            {insert: kNewCollectionName, documents: [{_id: 0}]}));
    }
    const createShell = startParallelShell(
        funWithArgs(runCreateToken, securityToken, kDbName, kNewCollectionName), primary.port);
    createCollFP.wait();

    // Check that the 'insert' op shows up in the currOp output for 'kTenant' when issuing
    // '$currentOp' in aggregation pipeline using both a security token and $tenant.
    assertCurrentOpAggOutputToken(tokenConn, kDbName, 1 /* expectedBatchSize */);
    assertCurrentOpAggOutputDollarTenant(adminDb, kTenant, kDbName, 1 /* expectedBatchSize */);

    // Check that the 'insert' op shows up in the currOp output for 'kTenant' when issuing
    // the currentOp command using both a security token and $tenant.
    assertCurrentOpCommandOutputToken(tokenConn, kDbName, 1 /* expectedBatchSize */);
    assertCurrentOpCommandOutputDollarTenant(adminDb, kTenant, kDbName, 1 /* expectedBatchSize */);

    // Check that the other tenant does not see the op in any currentOp output.
    tokenConn._setSecurityToken(securityTokenOtherTenant);
    assertCurrentOpAggOutputToken(tokenConn, kDbName, 0 /* expectedBatchSize */);
    assertCurrentOpAggOutputDollarTenant(adminDb, kOtherTenant, kDbName, 0 /* expectedBatchSize */);

    assertCurrentOpCommandOutputToken(tokenConn, kDbName, 0 /* expectedBatchSize */);
    assertCurrentOpCommandOutputDollarTenant(
        adminDb, kOtherTenant, kDbName, 0 /* expectedBatchSize */);

    createCollFP.off();
    createShell();

    // Drop the collection so we can re-create it below.
    assert.commandWorked(
        adminDb.getSiblingDB(kDbName).runCommand({drop: kNewCollectionName, $tenant: kTenant}));
}

// Now, run an insert for kTenant using $tenant. It will also implicitly create a collection, and we
// similarly force the collection creation to hang so that we'll capture the insert in the currentOp
// output.
{
    const createCollFP = configureFailPoint(primary, "hangBeforeLoggingCreateCollection");

    function runCreateDollarTenant(tenantId, dbName, kNewCollectionName) {
        assert(db.getSiblingDB('admin').auth({user: 'admin', pwd: 'pwd'}));
        assert.commandWorked(db.getSiblingDB(dbName).runCommand(
            {insert: kNewCollectionName, documents: [{_id: 0}], $tenant: tenantId}));
    }

    const createShell = startParallelShell(
        funWithArgs(runCreateDollarTenant, kTenant, kDbName, kNewCollectionName), primary.port);
    createCollFP.wait();

    // Check that the 'insert' op does NOT show up in the currOp output for 'kTenant' when issuing
    // '$currentOp' in aggregation pipeline using a security token. A security token user is not
    // authorized to pass ""allUsers: true", so it can only see ops that it has actually run itself.
    // In this case, the insert was issued by the "admin" user.
    assertCurrentOpAggOutputToken(tokenConn, kDbName, 0 /* expectedBatchSize */);

    // Check that the 'insert' op shows up in the currOp output for 'kTenant' when issuing
    // '$currentOp' in aggregation pipeline using $tenant.
    assertCurrentOpAggOutputDollarTenant(adminDb, kTenant, kDbName, 1 /* expectedBatchSize */);

    // Check that the 'insert' op also does NOT show up in the currOp output for 'kTenant' when
    // issuing the currentOp command, for the same reason as above.
    assertCurrentOpCommandOutputToken(tokenConn, kDbName, 0 /* expectedBatchSize */);

    // Check that the 'insert' op shows up in the currOp output for 'kTenant' when issuing
    // the currentOp command using $tenant.
    assertCurrentOpCommandOutputDollarTenant(adminDb, kTenant, kDbName, 1 /* expectedBatchSize */);

    // Now, check that the other tenant does not see the op in any currentOp output.
    tokenConn._setSecurityToken(securityTokenOtherTenant);
    assertCurrentOpAggOutputToken(tokenConn, kDbName, 0 /* expectedBatchSize */);
    assertCurrentOpAggOutputDollarTenant(adminDb, kOtherTenant, kDbName, 0 /* expectedBatchSize */);

    assertCurrentOpCommandOutputToken(tokenConn, kDbName, 0 /* expectedBatchSize */);
    assertCurrentOpCommandOutputDollarTenant(
        adminDb, kOtherTenant, kDbName, 0 /* expectedBatchSize */);

    // Now check that a privileged user can see this op using both $currentOp and the currentOp
    // command when no tenantId is provided. The user currently authenticated on the adminDb
    // connection has the "root" role.
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
}

rst.stopSet();
