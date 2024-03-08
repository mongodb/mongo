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
    conn._setSecurityToken(undefined);
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
    conn._setSecurityToken(undefined);
    const res = assert.commandWorked(
        conn.getDB(prefixedDb).runCommand({findAndModify: collName, query: query, update: update}));
    assert.eq(res.value, expectedDocReturned);
}

/*
 * Runs a find using unsigned security token, and asserts the find returns 'expectedDocsReturned'.
 * Also checks that the "ns" returned in the cursor result is serialized as expected, without the
 * tenantId.
 */
function runFindUsingSecurityToken(conn, db, collName, token, expectedDocsReturned) {
    conn._setSecurityToken(token);
    const res = assert.commandWorked(conn.getDB(db).runCommand({find: collName, filter: {}}));
    assert(arrayEq(expectedDocsReturned, res.cursor.firstBatch), tojson(res));
    const namespace = db + "." + collName;
    assert.eq(res.cursor.ns, namespace);
}

/*
 * Runs a find using unsigned security token and prefixed db, and asserts the find returns
 * 'expectedDocsReturned'. Also checks that the "ns" returned in the cursor result is serialized
 * as expected, including the tenantId.
 */
function runFindUsingSecurityTokenAndPrefix(
    conn, prefixedDb, collName, token, expectedDocsReturned) {
    conn._setSecurityToken(token);
    const res =
        assert.commandWorked(conn.getDB(prefixedDb).runCommand({find: collName, filter: {}}));
    assert(arrayEq(expectedDocsReturned, res.cursor.firstBatch), tojson(res));
    const prefixedNamespace = prefixedDb + "." + collName;
    assert.eq(res.cursor.ns, prefixedNamespace);
}

function assertTokenMustBeUsed(conn, tenant1DbPrefixed, tenant2DbPrefixed, kCollName) {
    conn._setSecurityToken(undefined);
    assert.commandFailedWithCode(
        conn.getDB(tenant1DbPrefixed).runCommand({find: kCollName, filter: {}}), 8233503);
    assert.commandFailedWithCode(
        conn.getDB(tenant2DbPrefixed).runCommand({find: kCollName, filter: {}}), 8233503);
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
function assertFindBothTenantsUsingSecurityToken(
    conn, db, collName, token1, token2, expectedDocsReturnedTenant1, expectedDocsReturnedTenant2) {
    runFindUsingSecurityToken(conn, db, collName, token1, expectedDocsReturnedTenant1);
    runFindUsingSecurityToken(conn, db, collName, token2, expectedDocsReturnedTenant2);
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

const kToken1 = _createTenantToken({tenant: kTenant1});
const kToken2 = _createTenantToken({tenant: kTenant2});

const kExpectPrefixToken1 = _createTenantToken({tenant: kTenant1, expectPrefix: true});
const kExpectPrefixToken2 = _createTenantToken({tenant: kTenant2, expectPrefix: true});

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

// Get another connecton of secondary as connection protocol cannot change once set.
let prefixedOriginalSecondary = new Mongo(originalSecondary.host);
prefixedOriginalSecondary.setSecondaryOk();
assert(prefixedOriginalSecondary.getDB("admin").auth('admin', 'pwd'));

// Check that we can still find the docs when using a prefixed db on both the primary and
// secondary.
assertFindBothTenantsPrefixedDb(
    originalPrimary, tenant1DbPrefixed, tenant2DbPrefixed, kCollName, tenant1Docs, tenant2Docs);

// Once multitenancy is set we must use a token.
assertTokenMustBeUsed(originalSecondary, tenant1DbPrefixed, tenant2DbPrefixed, kCollName);

// Now check that we find the docs for both tenants when reading from the secondary using
// a security token. The primary does not yet support a security token
// since it does not have multitenancySupport enabled.
assertFindBothTenantsUsingSecurityToken(
    originalSecondary, kDbName, kCollName, kToken1, kToken2, tenant1Docs, tenant2Docs);

// Also assert both tenants find the new doc on the secondary using token and a prefixed db.
runFindUsingSecurityTokenAndPrefix(
    prefixedOriginalSecondary, tenant1DbPrefixed, kCollName, kExpectPrefixToken1, tenant1Docs);
runFindUsingSecurityTokenAndPrefix(
    prefixedOriginalSecondary, tenant2DbPrefixed, kCollName, kExpectPrefixToken2, tenant2Docs);

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

// Assert both tenants find the new doc on both the primary.
assertFindBothTenantsPrefixedDb(originalPrimary,
                                tenant1DbPrefixed,
                                tenant2DbPrefixed,
                                kCollName,
                                allTenant1Docs,
                                allTenant2Docs);
// The token must be used on the secondary.
assertTokenMustBeUsed(originalSecondary, tenant1DbPrefixed, tenant2DbPrefixed, kCollName);

// Assert both tenants find the new doc on the secondary using token.
assertFindBothTenantsUsingSecurityToken(
    originalSecondary, kDbName, kCollName, kToken1, kToken2, allTenant1Docs, allTenant2Docs);

// Assert both tenants find the new doc on the secondary using token and a prefixed db.
runFindUsingSecurityTokenAndPrefix(
    prefixedOriginalSecondary, tenant1DbPrefixed, kCollName, kExpectPrefixToken1, allTenant1Docs);
runFindUsingSecurityTokenAndPrefix(
    prefixedOriginalSecondary, tenant2DbPrefixed, kCollName, kExpectPrefixToken2, allTenant2Docs);

// Now run findAndModify on one doc using a prefixed db and check that we can read from the
// secondary using just token and a prefix.
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
assertFindBothTenantsUsingSecurityToken(originalSecondary,
                                        kDbName,
                                        kCollName,
                                        kToken1,
                                        kToken2,
                                        modifiedTenant1Docs,
                                        modifiedTenant2Docs);

runFindUsingSecurityTokenAndPrefix(prefixedOriginalSecondary,
                                   tenant1DbPrefixed,
                                   kCollName,
                                   kExpectPrefixToken1,
                                   modifiedTenant1Docs);
runFindUsingSecurityTokenAndPrefix(prefixedOriginalSecondary,
                                   tenant2DbPrefixed,
                                   kCollName,
                                   kExpectPrefixToken2,
                                   modifiedTenant2Docs);

// Now, restart the primary and enable multitenancySupport. The secondary will step up to
// become primary.
originalPrimary =
    rst.restart(originalPrimary, {startClean: false, setParameter: {'multitenancySupport': true}});
assert(originalPrimary.getDB("admin").auth('admin', 'pwd'));
waitForState(originalSecondary, ReplSetTest.State.PRIMARY);
waitForState(originalPrimary, ReplSetTest.State.SECONDARY);
originalPrimary.setSecondaryOk();

// Get another connecton of original primary as connection protocol cannot change once set.
let prefixedOriginalPrimary = new Mongo(originalPrimary.host);
prefixedOriginalPrimary.setSecondaryOk();
assert(prefixedOriginalPrimary.getDB("admin").auth('admin', 'pwd'));

// The token must now be used on both primary and secocndary.
assertTokenMustBeUsed(originalPrimary, tenant1DbPrefixed, tenant2DbPrefixed, kCollName);
assertTokenMustBeUsed(originalSecondary, tenant1DbPrefixed, tenant2DbPrefixed, kCollName);

// Now check that we find the docs for both tenants when reading from both the primary and
// secondary using token.
assertFindBothTenantsUsingSecurityToken(originalPrimary,
                                        kDbName,
                                        kCollName,
                                        kToken1,
                                        kToken2,
                                        modifiedTenant1Docs,
                                        modifiedTenant2Docs);
assertFindBothTenantsUsingSecurityToken(originalSecondary,
                                        kDbName,
                                        kCollName,
                                        kToken1,
                                        kToken2,
                                        modifiedTenant1Docs,
                                        modifiedTenant2Docs);

// Also check that both tenants find the new doc on the primary and secondary using token and a
// prefixed db.
runFindUsingSecurityTokenAndPrefix(prefixedOriginalPrimary,
                                   tenant1DbPrefixed,
                                   kCollName,
                                   kExpectPrefixToken1,
                                   modifiedTenant1Docs);
runFindUsingSecurityTokenAndPrefix(prefixedOriginalSecondary,
                                   tenant2DbPrefixed,
                                   kCollName,
                                   kExpectPrefixToken2,
                                   modifiedTenant2Docs);
runFindUsingSecurityTokenAndPrefix(prefixedOriginalPrimary,
                                   tenant1DbPrefixed,
                                   kCollName,
                                   kExpectPrefixToken1,
                                   modifiedTenant1Docs);
runFindUsingSecurityTokenAndPrefix(prefixedOriginalSecondary,
                                   tenant2DbPrefixed,
                                   kCollName,
                                   kExpectPrefixToken2,
                                   modifiedTenant2Docs);

originalPrimary._setSecurityToken(undefined);
originalSecondary._setSecurityToken(undefined);
prefixedOriginalPrimary._setSecurityToken(undefined);
prefixedOriginalSecondary._setSecurityToken(undefined);
rst.stopSet();
