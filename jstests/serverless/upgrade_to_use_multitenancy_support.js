/**
 * This test checks that tenants can access their data before, during and after enabling
 * multitenancySupport in a rolling fashion in a replica set.
 */

import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {waitForState} from "jstests/replsets/rslib.js";

// In production, we will upgrade to start using multitenancySupport before enabling this feature
// flag, and this test is meant to exercise that upgrade behavior, so don't run if the feature flag
// is enabled.
const featureFlagRequireTenantId = TestData.setParameters.featureFlagRequireTenantID;
if (featureFlagRequireTenantId) {
    quit();
}

/*
 * Runs a find using a prefixed db, and asserts the find returns 'expectedDocsReturned'. Also
 * checks that the "ns" returned in the cursor result is serialized as expected, including the
 * tenantId.
 */
function runFindOnPrefixedDb(conn, prefixedDb, collName, expectedDocsReturned) {
    const res =
        assert.commandWorked(conn.getDB(prefixedDb).runCommand({find: collName, filter: {}}));
    assert(arrayEq(expectedDocsReturned, res.cursor.firstBatch), tojson(res));
    const prefixedNamespace = prefixedDb + "." + collName;
    assert.eq(res.cursor.ns, prefixedNamespace);
}

/*
 * Runs a findAndModify using a prefixed db.
 */
function runFindAndModOnPrefixedDb(conn, prefixedDb, collName, query, update, expectedDocReturned) {
    const res = assert.commandWorked(
        conn.getDB(prefixedDb).runCommand({findAndModify: collName, query: query, update: update}));
    assert.eq(res.value, expectedDocReturned);
}

/*
 * Runs a find using $tenant, and asserts the find returns 'expectedDocsReturned'. Also
 * checks that the "ns" returned in the cursor result is serialized as expected, without the
 * tenantId.
 */
function runFindUsingDollarTenant(conn, db, collName, tenantId, expectedDocsReturned) {
    const res = assert.commandWorked(
        conn.getDB(db).runCommand({find: collName, filter: {}, $tenant: tenantId}));
    assert(arrayEq(expectedDocsReturned, res.cursor.firstBatch), tojson(res));
    const namespace = db + "." + collName;
    assert.eq(res.cursor.ns, namespace);
}

/*
 * Runs a find using $tenant and prefixed db, and asserts the find returns
 * 'expectedDocsReturned'. Also checks that the "ns" returned in the cursor result is serialized
 * as expected, including the tenantId.
 */
function runFindUsingDollarTenantAndPrefix(
    conn, prefixedDb, collName, tenantId, expectedDocsReturned) {
    const res = assert.commandWorked(
        conn.getDB(prefixedDb)
            .runCommand({find: collName, filter: {}, $tenant: tenantId, expectPrefix: true}));
    assert(arrayEq(expectedDocsReturned, res.cursor.firstBatch), tojson(res));
    const prefixedNamespace = prefixedDb + "." + collName;
    assert.eq(res.cursor.ns, prefixedNamespace);
}

/*
 * Runs a find for both tenants using a prefixed db, and asserts the find returns
 * 'expectedDocsReturned'.
 */
function assertFindBothTenantsPrefixedDb(
    conn, tenant1DbPrefixed, tenant2DbPrefixed, kCollName, tenant1Docs, tenant2Docs) {
    runFindOnPrefixedDb(conn, tenant1DbPrefixed, kCollName, tenant1Docs);
    runFindOnPrefixedDb(conn, tenant2DbPrefixed, kCollName, tenant2Docs);
}

/*
 * Runs a find for both tenants using a prefixed db, and asserts the find returns
 * 'expectedDocsReturned'.
 */
function assertFindBothTenantsUsingDollarTenant(conn,
                                                db,
                                                collName,
                                                tenantId1,
                                                tenantId2,
                                                expectedDocsReturnedTenant1,
                                                expectedDocsReturnedTenant2) {
    runFindUsingDollarTenant(conn, db, collName, tenantId1, expectedDocsReturnedTenant1);
    runFindUsingDollarTenant(conn, db, collName, tenantId2, expectedDocsReturnedTenant2);
}

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        auth: '',
    }
});
rst.startSet({keyFile: 'jstests/libs/key1'});
rst.initiate();

let originalPrimary = rst.getPrimary();
let originalSecondary = rst.getSecondary();

const kTenant1 = ObjectId();
const kTenant2 = ObjectId();
const kDbName = "test";
const kCollName = "foo";

// Create a root user and login on both the primary and secondary.
const primaryAdminDb = originalPrimary.getDB('admin');
let secondaryAdminDb = originalSecondary.getDB('admin');
assert.commandWorked(primaryAdminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(primaryAdminDb.auth('admin', 'pwd'));
assert(secondaryAdminDb.auth('admin', 'pwd'));

// Insert data for two different tenants - multitenancySupport is not yet enabled, so we use a
// prefixed db. Then, check that we find the correct docs for both tenants, reading from both
// the primary and secondary.
const tenant1DbPrefixed = kTenant1 + "_" + kDbName;
const tenant1Docs = [{_id: 0, x: 1, y: 1}, {_id: 1, x: 2, y: 3}];
assert.commandWorked(originalPrimary.getDB(tenant1DbPrefixed)
                         .runCommand({insert: kCollName, documents: tenant1Docs}));

const tenant2DbPrefixed = kTenant2 + "_" + kDbName;
const tenant2Docs = [{_id: 10, a: 10, b: 10}, {_id: 11, a: 20, b: 30}];
assert.commandWorked(originalPrimary.getDB(tenant2DbPrefixed)
                         .runCommand({insert: kCollName, documents: tenant2Docs}));

assertFindBothTenantsPrefixedDb(
    originalPrimary, tenant1DbPrefixed, tenant2DbPrefixed, kCollName, tenant1Docs, tenant2Docs);
assertFindBothTenantsPrefixedDb(
    originalSecondary, tenant1DbPrefixed, tenant2DbPrefixed, kCollName, tenant1Docs, tenant2Docs);

// Now, restart the secondary and enable multitenancySupport. The primary still does not have
// multitenancySupport enabled.
originalSecondary = rst.restart(originalSecondary,
                                {startClean: false, setParameter: {'multitenancySupport': true}});

originalSecondary.setSecondaryOk();
assert(originalSecondary.getDB("admin").auth('admin', 'pwd'));

// Check that we can still find the docs when using a prefixed db on both the primary and
// secondary.
assertFindBothTenantsPrefixedDb(
    originalPrimary, tenant1DbPrefixed, tenant2DbPrefixed, kCollName, tenant1Docs, tenant2Docs);
assertFindBothTenantsPrefixedDb(
    originalSecondary, tenant1DbPrefixed, tenant2DbPrefixed, kCollName, tenant1Docs, tenant2Docs);

// Now check that we find the docs for both tenants when reading from the secondary using
// $tenant and a security token. The primary does not yet support $tenant or a security token
// since it does not have multitenancySupport enabled.
assertFindBothTenantsUsingDollarTenant(
    originalSecondary, kDbName, kCollName, kTenant1, kTenant2, tenant1Docs, tenant2Docs);

// Also assert both tenants find the new doc on the secondary using $tenant and a prefixed db.
runFindUsingDollarTenantAndPrefix(
    originalSecondary, tenant1DbPrefixed, kCollName, kTenant1, tenant1Docs);
runFindUsingDollarTenantAndPrefix(
    originalSecondary, tenant2DbPrefixed, kCollName, kTenant2, tenant2Docs);

// Now insert a new doc for both tenants using the prefixed db, and assert that we can find it
// on both the primary and secondary.
const newTenant1Doc = [{_id: 2, x: 3}];
const newTenant2Doc = [{_id: 12, a: 30}];
assert.commandWorked(originalPrimary.getDB(tenant1DbPrefixed)
                         .runCommand({insert: kCollName, documents: newTenant1Doc}));
assert.commandWorked(originalPrimary.getDB(tenant2DbPrefixed)
                         .runCommand({insert: kCollName, documents: newTenant2Doc}));

const allTenant1Docs = tenant1Docs.concat(newTenant1Doc);
const allTenant2Docs = tenant2Docs.concat(newTenant2Doc);

// Assert both tenants find the new doc on both the primary and secondary when using the
// prefixed db.
assertFindBothTenantsPrefixedDb(originalPrimary,
                                tenant1DbPrefixed,
                                tenant2DbPrefixed,
                                kCollName,
                                allTenant1Docs,
                                allTenant2Docs);
assertFindBothTenantsPrefixedDb(originalSecondary,
                                tenant1DbPrefixed,
                                tenant2DbPrefixed,
                                kCollName,
                                allTenant1Docs,
                                allTenant2Docs);

// Assert both tenants find the new doc on the secondary using $tenant.
assertFindBothTenantsUsingDollarTenant(
    originalSecondary, kDbName, kCollName, kTenant1, kTenant2, allTenant1Docs, allTenant2Docs);

// Assert both tenants find the new doc on the secondary using $tenant and a prefixed db.
runFindUsingDollarTenantAndPrefix(
    originalSecondary, tenant1DbPrefixed, kCollName, kTenant1, allTenant1Docs);
runFindUsingDollarTenantAndPrefix(
    originalSecondary, tenant2DbPrefixed, kCollName, kTenant2, allTenant2Docs);

// Now run findAndModify on one doc using a prefixed db and check that we can read from the
// secondary using just $tenant and $tenant and a prefix.
runFindAndModOnPrefixedDb(originalPrimary,
                          tenant1DbPrefixed,
                          kCollName,
                          newTenant1Doc[0],
                          {$set: {x: 4}},
                          newTenant1Doc[0]);
runFindAndModOnPrefixedDb(originalPrimary,
                          tenant2DbPrefixed,
                          kCollName,
                          newTenant2Doc[0],
                          {$set: {a: 40}},
                          newTenant2Doc[0]);

const modifiedTenant1Docs = tenant1Docs.concat([{_id: 2, x: 4}]);
const modifiedTenant2Docs = tenant2Docs.concat([{_id: 12, a: 40}]);
assertFindBothTenantsUsingDollarTenant(originalSecondary,
                                       kDbName,
                                       kCollName,
                                       kTenant1,
                                       kTenant2,
                                       modifiedTenant1Docs,
                                       modifiedTenant2Docs);

runFindUsingDollarTenantAndPrefix(
    originalSecondary, tenant1DbPrefixed, kCollName, kTenant1, modifiedTenant1Docs);
runFindUsingDollarTenantAndPrefix(
    originalSecondary, tenant2DbPrefixed, kCollName, kTenant2, modifiedTenant2Docs);

// Now, restart the primary and enable multitenancySupport. The secondary will step up to
// become primary.
originalPrimary =
    rst.restart(originalPrimary, {startClean: false, setParameter: {'multitenancySupport': true}});
assert(originalPrimary.getDB("admin").auth('admin', 'pwd'));
waitForState(originalSecondary, ReplSetTest.State.PRIMARY);
waitForState(originalPrimary, ReplSetTest.State.SECONDARY);
originalPrimary.setSecondaryOk();

// Check that we can still find the docs when using a prefixed db on both the primary and
// secondary.
assertFindBothTenantsPrefixedDb(originalPrimary,
                                tenant1DbPrefixed,
                                tenant2DbPrefixed,
                                kCollName,
                                modifiedTenant1Docs,
                                modifiedTenant2Docs);
assertFindBothTenantsPrefixedDb(originalSecondary,
                                tenant1DbPrefixed,
                                tenant2DbPrefixed,
                                kCollName,
                                modifiedTenant1Docs,
                                modifiedTenant2Docs);

// Now check that we find the docs for both tenants when reading from both the primary and
// secondary using $tenant.
assertFindBothTenantsUsingDollarTenant(originalPrimary,
                                       kDbName,
                                       kCollName,
                                       kTenant1,
                                       kTenant2,
                                       modifiedTenant1Docs,
                                       modifiedTenant2Docs);
assertFindBothTenantsUsingDollarTenant(originalSecondary,
                                       kDbName,
                                       kCollName,
                                       kTenant1,
                                       kTenant2,
                                       modifiedTenant1Docs,
                                       modifiedTenant2Docs);

// Also check that both tenants find the new doc on the primary and secondary using $tenant and
// a prefixed db.
runFindUsingDollarTenantAndPrefix(
    originalPrimary, tenant1DbPrefixed, kCollName, kTenant1, modifiedTenant1Docs);
runFindUsingDollarTenantAndPrefix(
    originalSecondary, tenant2DbPrefixed, kCollName, kTenant2, modifiedTenant2Docs);
runFindUsingDollarTenantAndPrefix(
    originalPrimary, tenant1DbPrefixed, kCollName, kTenant1, modifiedTenant1Docs);
runFindUsingDollarTenantAndPrefix(
    originalSecondary, tenant2DbPrefixed, kCollName, kTenant2, modifiedTenant2Docs);

rst.stopSet();
