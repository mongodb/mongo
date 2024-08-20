// Test that currentOp works as expected in a multitenant environment.
// @tags: [requires_fcv_62]
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

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

assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));

const featureFlagRequireTenantId = FeatureFlagUtil.isEnabled(adminDb, "RequireTenantID");

// Create a non-privileged user for later use.
assert.commandWorked(
    adminDb.runCommand({createUser: 'dbAdmin', pwd: 'pwd', roles: ['dbAdminAnyDatabase']}));

const securityToken =
    _createSecurityToken({user: "userTenant1", db: '$external', tenant: kTenant}, kVTSKey);

// Set a temporary token to create a user
primary._setSecurityToken(_createTenantToken({tenant: kTenant}));
assert.commandWorked(primary.getDB('$external').runCommand({
    createUser: "userTenant1",
    roles: [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
}));

const securityTokenOtherTenant =
    _createSecurityToken({user: "userTenant2", db: '$external', tenant: kOtherTenant}, kVTSKey);

// Set a temporary token to create a user
primary._setSecurityToken(_createTenantToken({tenant: kOtherTenant}));
assert.commandWorked(primary.getDB('$external').runCommand({
    createUser: "userTenant2",
    roles: [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
}));

// Reset the token on primary
primary._setSecurityToken(undefined);

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
    // '$currentOp' in aggregation pipeline using a security token.
    assertCurrentOpAggOutputToken(tokenConn, kDbName, 1 /* expectedBatchSize */);

    // Check that the 'insert' op shows up in the currOp output for 'kTenant' when issuing
    // the currentOp command using a security token.
    assertCurrentOpCommandOutputToken(tokenConn, kDbName, 1 /* expectedBatchSize */);

    // Check that the other tenant does not see the op in any currentOp output.
    tokenConn._setSecurityToken(securityTokenOtherTenant);
    assertCurrentOpAggOutputToken(tokenConn, kDbName, 0 /* expectedBatchSize */);

    assertCurrentOpCommandOutputToken(tokenConn, kDbName, 0 /* expectedBatchSize */);

    createCollFP.off();
    createShell();

    // Drop the collection so we can re-create it below.
    tokenConn._setSecurityToken(securityToken);
    assert.commandWorked(tokenConn.getDB(kDbName).runCommand({drop: kNewCollectionName}));
}

rst.stopSet();
