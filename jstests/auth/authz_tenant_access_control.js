// Test that the privilege system works to separate tenants, but allows a privileged-enough
// non-tenanted user to access tenant collections.
// @tags: [requires_replication, serverless, featureFlagSecurityToken]

import {runCommandWithSecurityToken} from "jstests/libs/multitenancy_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const tenantId1 = ObjectId();
const tenantId2 = ObjectId();
const unsignedToken1 = _createTenantToken({tenant: tenantId1});
const kVTSKey = 'secret';

// List of tests. Each test specifies a role which the created user should have, whether we
// should be testing with a tenant ID, and the list of authorization checks to run and their
// expected result. isTenantedUser tests have a shorter list of authz checks to run because we can't
// pass unsigned security token and signed security token on the same request, so we are limited to
// tests which pass no tenant ID.
const testsToRun = [
    // Role specifying a specific namespace should allow user to write to that exact namespace
    // with their own tenant ID and nothing else
    {
        role: 'readWriteTenantTestFooRole',
        isTenantedUser: true,
        collectionTests: {
            normalNoTenant: true,
            systemNoTenant: false,
        }
    },
    {
        role: 'readWriteTestFooRole',
        isTenantedUser: false,
        collectionTests: {
            normalNoTenant: true,
            normalWithTenant: false,
            normalOtherTenant: false,
            systemNoTenant: false,
            systemWithTenant: false,
            systemOtherTenant: false
        }
    },

    // Role specifying a specific DB should allow user to write to that exact DB with their own
    // tenant ID and nothing else
    {
        role: 'readWriteTenantTestDBRole',
        isTenantedUser: true,
        collectionTests: {
            normalNoTenant: true,
            systemNoTenant: false,
        }
    },
    {
        role: 'readWriteTestDBRole',
        isTenantedUser: false,
        collectionTests: {
            normalNoTenant: true,
            normalWithTenant: false,
            normalOtherTenant: false,
            systemNoTenant: false,
            systemWithTenant: false,
            systemOtherTenant: false
        }
    },

    // readWriteAny, which confers read/write on anyNormalResource, should allow a tenanted user
    // to write to any normal collections with their own tenant ID, but should allow a
    // non-tenanted user to write to any normal collections, even those with tenant IDs.
    {
        role: 'readWriteAnyDatabase',
        isTenantedUser: true,
        collectionTests: {
            normalNoTenant: true,
            systemNoTenant: false,
        }
    },
    {
        role: 'readWriteAnyDatabase',
        isTenantedUser: false,
        collectionTests: {
            normalNoTenant: true,
            normalWithTenant: true,
            normalOtherTenant: true,
            systemNoTenant: false,
            systemWithTenant: false,
            systemOtherTenant: false
        }
    },
    // If we skip adding useTenant, we should now only be authorized for the no-tenant collections.
    {
        role: 'readWriteAnyDatabase',
        isTenantedUser: false,
        skipUseTenant: true,
        collectionTests: {
            normalNoTenant: true,
            normalWithTenant: false,
            normalOtherTenant: false,
            systemNoTenant: false,
            systemWithTenant: false,
            systemOtherTenant: false
        }
    },

    // read/write on anyResource should allow a tenanted user to write to any collections with
    // their own tenant ID, and should allow a non-tenanted user to write to any collection
    // regardless of tenant ID.
    {
        role: 'readWriteTenantAnyRole',
        isTenantedUser: true,
        collectionTests: {
            normalNoTenant: true,
            systemNoTenant: true,
        }
    },
    {
        role: 'readWriteAnyRole',
        isTenantedUser: false,
        collectionTests: {
            normalNoTenant: true,
            normalWithTenant: true,
            normalOtherTenant: true,
            systemNoTenant: true,
            systemWithTenant: true,
            systemOtherTenant: true
        }
    },
    // If we skip adding useTenant, we should now be able to read/write any collection with no
    // tenant, and no others.
    {
        role: 'readWriteAnyRole',
        isTenantedUser: false,
        skipUseTenant: true,
        collectionTests: {
            normalNoTenant: true,
            normalWithTenant: false,
            normalOtherTenant: false,
            systemNoTenant: true,
            systemWithTenant: false,
            systemOtherTenant: false
        }
    }
];

// We use the admin DB for normal collection tests, and the local DB for non-normal collection
// tests, mainly because the admin and local DBs are allowed to be read/written to even when
// featureFlagRequireTenantID is enabled.
const testToNamespaceMap = {
    normalNoTenant: {tenant: undefined, db: 'admin', coll: 'foo'},
    normalWithTenant: {tenant: tenantId1, db: 'admin', coll: 'foo'},
    normalOtherTenant: {tenant: tenantId2, db: 'admin', coll: 'foo'},
    systemNoTenant: {tenant: undefined, db: 'local', coll: 'foo'},
    systemWithTenant: {tenant: tenantId1, db: 'local', coll: 'foo'},
    systemOtherTenant: {tenant: tenantId2, db: 'local', coll: 'foo'},
};

function runTests(conn) {
    const admin = conn.getDB('admin');

    assert.commandWorked(admin.runCommand({createUser: 'system', pwd: 'pwd', roles: ['__system']}));
    assert(admin.auth('system', 'pwd'));

    // Make specific roles for testing.
    assert.commandWorked(admin.runCommand({
        createRole: 'readWriteTestFooRole',
        roles: [],
        privileges: [{resource: {db: "admin", collection: "foo"}, actions: ['insert', 'find']}]
    }));
    assert.commandWorked(admin.runCommand({
        createRole: 'readWriteTestDBRole',
        roles: [],
        privileges: [{resource: {db: "admin", collection: ""}, actions: ['insert', 'find']}]
    }));
    assert.commandWorked(runCommandWithSecurityToken(unsignedToken1, admin, {
        createRole: 'readWriteTenantTestFooRole',
        roles: [],
        privileges: [{resource: {db: "admin", collection: "foo"}, actions: ['insert', 'find']}],
    }));
    assert.commandWorked(runCommandWithSecurityToken(unsignedToken1, admin, {
        createRole: 'readWriteTenantTestDBRole',
        roles: [],
        privileges: [{resource: {db: "admin", collection: ""}, actions: ['insert', 'find']}],
    }));
    assert.commandWorked(admin.runCommand({
        createRole: 'readWriteAnyRole',
        roles: [],
        privileges: [{resource: {anyResource: true}, actions: ['insert', 'find']}],
    }));

    assert.commandWorked(runCommandWithSecurityToken(unsignedToken1, admin, {
        createRole: 'readWriteTenantAnyRole',
        roles: [],
        privileges: [{resource: {anyResource: true}, actions: ['insert', 'find']}],
    }));

    for (let i = 0; i < testsToRun.length; i++) {
        const test = testsToRun[i];
        jsTest.log("TESTING: role = " + test.role +
                   ", tenant = " + (test.isTenantedUser ? "true" : "false"));
        const username = 'testUser' + i.toString();
        let role = test.role;
        // Unless skipUseTenant is set, we want to always allow non-tenanted users to pass unsigned
        // token, so for non-tenanted users, give them a special subrole with useTenant
        if (!test.isTenantedUser && !test.skipUseTenant) {
            jsTest.log("Creating role with useTenant for this test");
            role = username + "-role";
            assert.commandWorked(admin.runCommand({
                createRole: role,
                roles: [test.role],
                privileges: [{resource: {cluster: true}, actions: ['useTenant']}]
            }));
        }

        // Create a user for this test, with the given role and tenant ID.
        let createUserCmd = {createUser: username, pwd: 'pwd', roles: [role]};
        if (test.isTenantedUser) {
            assert.commandWorked(runCommandWithSecurityToken(unsignedToken1, admin, createUserCmd));
        } else {
            assert.commandWorked(admin.runCommand(createUserCmd));
        }

        admin.logout();

        // We use security token to auth tenant users, and the mongo.auth method to auth non-tenant
        // users.
        if (test.isTenantedUser) {
            conn._setSecurityToken(
                _createSecurityToken({user: username, db: 'admin', tenant: tenantId1}, kVTSKey));
        } else {
            assert(admin.auth(username, 'pwd'));
        }

        for (const [testName, shouldWork] of Object.entries(test.collectionTests)) {
            jsTest.log("Running collection test: " + testName);
            const nss = testToNamespaceMap[testName];
            const db = conn.getDB(nss.db);
            // Test a find and insert operation on each collection; whether they work should match
            // our expectation.
            if (shouldWork) {
                assert.commandWorked(db.runCommand({find: nss.coll}, nss.tenant));
                assert.commandWorked(db.runCommand({insert: nss.coll, documents: [{a: username}]}));
            } else {
                const unsignedToken =
                    nss.tenant ? _createTenantToken({tenant: nss.tenant}) : undefined;
                assert.commandFailedWithCode(
                    runCommandWithSecurityToken(unsignedToken, db, {find: nss.coll}),
                    [ErrorCodes.Unauthorized]);
                assert.commandFailedWithCode(
                    runCommandWithSecurityToken(
                        unsignedToken, db, {insert: nss.coll, documents: [{a: username}]}),
                    [ErrorCodes.Unauthorized]);
            }
        }

        if (test.isTenantedUser) {
            conn._setSecurityToken(undefined);
        } else {
            admin.logout();
        }

        assert(admin.auth('system', 'pwd'));
    }
}

const opts = {
    auth: '',
    setParameter: {
        multitenancySupport: true,
        testOnlyValidatedTenancyScopeKey: kVTSKey,
    },
};

// Test on standalone and replset.
{
    const standalone = MongoRunner.runMongod(opts);
    runTests(standalone);
    MongoRunner.stopMongod(standalone);
}

{
    const rst = new ReplSetTest({nodes: 2, nodeOptions: opts});
    rst.startSet({keyFile: 'jstests/libs/key1'});
    rst.initiate();
    runTests(rst.getPrimary());
    rst.stopSet();
}
// Do not test sharding since mongos must have an authenticated connection to
// all mongod nodes, and this conflicts with proxying tokens which we'll be
// performing in mongoq.
