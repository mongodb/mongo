// Test that we can create and auth a tenant (user) using a security token.
// Then create an op (insert) for that tenant, find it using `currentOp` and kill it using `killOp`.

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

function killCurrentOpTest() {
    function operationToKillFunc(securityToken, dbName, colName) {
        db.getMongo()._setSecurityToken(securityToken);
        const insertCmdObj = {insert: colName, documents: [{_id: 0}]};
        assert.commandWorked(db.getSiblingDB(dbName).runCommand(insertCmdObj));
    }

    const kVTSKey = 'secret';
    const rst = new ReplSetTest({
        nodes: 3,
        nodeOptions: {
            auth: '',
            setParameter: {
                multitenancySupport: true,
                featureFlagSecurityToken: true,
                testOnlyValidatedTenancyScopeKey: kVTSKey
            }
        }
    });
    rst.startSet({keyFile: 'jstests/libs/key1'});
    rst.initiate();

    const primary = rst.getPrimary();
    const adminDb = primary.getDB('admin');

    // Prepare an authenticated user for testing.
    // Must be authenticated as a user with ActionType::useTenant in order to use security token
    assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
    assert(adminDb.auth('admin', 'pwd'));

    const kTenant = ObjectId();
    const kOtherTenant = ObjectId();
    const kDbName = 'myDb';
    const kCollName = "currOpColl";

    // Create a user for kTenant and its security token.
    const securityToken =
        _createSecurityToken({user: "userTenant1", db: '$external', tenant: kTenant}, kVTSKey);

    primary._setSecurityToken(_createTenantToken({tenant: kTenant}));
    assert.commandWorked(primary.getDB('$external').runCommand({
        createUser: "userTenant1",
        roles:
            [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
    }));

    // Create a different tenant to test that one tenant can't see or kill other tenant's op.
    const securityTokenOtherTenant =
        _createSecurityToken({user: "userTenant2", db: '$external', tenant: kOtherTenant}, kVTSKey);

    primary._setSecurityToken(_createTenantToken({tenant: kOtherTenant}));
    assert.commandWorked(primary.getDB('$external').runCommand({
        createUser: "userTenant2",
        roles:
            [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
    }));
    primary._setSecurityToken(undefined);

    const tokenConn = new Mongo(primary.host);
    tokenConn._setSecurityToken(securityToken);

    // test that the current tenant can see and kill his current op.
    {
        const findCurrentOpCmd = {
            aggregate: 1,
            pipeline: [{$currentOp: {allUsers: false}}, {$match: {op: "insert"}}],
            cursor: {}
        };

        const createCollFP = configureFailPoint(primary, "hangBeforeLoggingCreateCollection");

        const parallelShell = startParallelShell(
            funWithArgs(operationToKillFunc, securityToken, kDbName, kCollName), primary.port);

        createCollFP.wait();

        // find the current insert op that's being blocked due to the failpoint.
        let findCurrentOpRes = tokenConn.getDB("admin").runCommand(findCurrentOpCmd);
        assert.eq(findCurrentOpRes.cursor.firstBatch.length, 1, tojson(findCurrentOpRes));
        const opIdToKill = findCurrentOpRes.cursor.firstBatch[0].opid;

        // Try to kill the op with a different tenant / security token. Fails due to Unauthorized.
        tokenConn._setSecurityToken(securityTokenOtherTenant);
        assert.commandFailedWithCode(
            tokenConn.getDB("admin").runCommand({killOp: 1, op: opIdToKill}),
            ErrorCodes.Unauthorized);

        // Try to kill the op with a the same tenant / security token. Succeeds.
        tokenConn._setSecurityToken(securityToken);
        assert.commandWorked(tokenConn.getDB("admin").runCommand({killOp: 1, op: opIdToKill}));

        createCollFP.off();

        // the current op was killed therefore the thread will throw an exception and wil return
        // code 252.
        const exitCode = parallelShell({checkExitSuccess: false});
        assert.neq(0, exitCode, "Expected shell to exit with failure due to operation kill");

        // we should no longer have an operation for that tenant.
        findCurrentOpRes = tokenConn.getDB("admin").runCommand(findCurrentOpCmd);
        assert.eq(findCurrentOpRes.cursor.firstBatch.length, 0, tojson(findCurrentOpRes));
    }

    rst.stopSet();
}
killCurrentOpTest();
