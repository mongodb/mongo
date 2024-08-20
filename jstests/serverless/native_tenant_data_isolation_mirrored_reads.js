/**
 * Test mirrored reads in a multi-tenant environment.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        auth: '',
        setParameter: {
            multitenancySupport: true,
            logComponentVerbosity: tojson({command: 1}),
            mirrorReads: tojsononeline({samplingRate: 1.0}),
            "failpoint.mirrorMaestroExpectsResponse": tojson({mode: "alwaysOn"}),
        }
    }
});
rst.startSet({keyFile: 'jstests/libs/key1'});
rst.initiate();

const primary = rst.getPrimary();
const adminDb = primary.getDB('admin');
const secondary = rst.getSecondary();

// Prepare an authenticated user for testing.
// Must be authenticated as a user with ActionType::useTenant in order to use security token
assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));
rst.awaitReplication();
assert(secondary.getDB('admin').auth('admin', 'pwd'));

const kTenant = ObjectId();
const kOtherTenant = ObjectId();
const kDbName = 'myDb';
const kCollName = 'myColl';
const testDb = primary.getDB(kDbName);

const securityToken = _createTenantToken({tenant: kTenant});
const otherSecurityToken = _createTenantToken({tenant: kOtherTenant});

function getMirroredReadsStats(node) {
    return node.getDB(kDbName).serverStatus({mirroredReads: 1}).mirroredReads;
}

function assertSecondaryStats(initialSecondaryStats, numSentSince) {
    const currentSecondaryStats = getMirroredReadsStats(secondary);
    jsTestLog("Current secondary stats: " + tojson(currentSecondaryStats));
    const numProcessed = currentSecondaryStats.processedAsSecondary;
    return initialSecondaryStats.processedAsSecondary + numSentSince == numProcessed;
}

const kTenantDocs = [{w: 0}, {x: 1}, {y: 2}, {z: 3}];
const kOtherTenantDocs = [{i: 1}, {j: 2}, {k: 3}];

primary._setSecurityToken(securityToken);
assert.commandWorked(testDb.runCommand({insert: kCollName, documents: kTenantDocs}));
primary._setSecurityToken(otherSecurityToken);
assert.commandWorked(testDb.runCommand({insert: kCollName, documents: kOtherTenantDocs}));

function verifyMirroredReadStats(cmd, token) {
    primary._setSecurityToken(token);
    const initialPrimaryStats = getMirroredReadsStats(primary);
    const initialSecondaryStats = getMirroredReadsStats(secondary);
    jsTestLog("Verifying mirrored reads for cmd: " + tojson(cmd));
    jsTestLog("Initial primary stats: " + tojson(initialPrimaryStats));
    jsTestLog("Initial secondary stats: " + tojson(initialSecondaryStats));

    // Check that the mirrored operation is observable through the metrics.
    assert.commandWorked(testDb.runCommand(cmd));
    let currentPrimaryStats;
    assert.soon(() => {
        currentPrimaryStats = getMirroredReadsStats(primary);
        jsTestLog("Current primary stats: " + tojson(currentPrimaryStats));
        let resolved = currentPrimaryStats.resolved;
        let succeeded = currentPrimaryStats.succeeded;
        return (initialPrimaryStats.resolved + 1 == resolved) &&
            (initialPrimaryStats.succeeded + 1 == succeeded);
    });
    assert.eq(initialPrimaryStats.seen + 1, currentPrimaryStats.seen, currentPrimaryStats);
    assertSecondaryStats(initialSecondaryStats, 1);
}

// Verify that mirrored reads are successful for mirrored operations with security token
verifyMirroredReadStats({find: kCollName, projection: {_id: 0}}, securityToken);
verifyMirroredReadStats({find: kCollName, projection: {_id: 0}}, otherSecurityToken);

verifyMirroredReadStats({count: kCollName, query: {x: 1}}, securityToken);
verifyMirroredReadStats({count: kCollName, query: {i: 1}}, otherSecurityToken);

verifyMirroredReadStats({distinct: kCollName, key: 'x'}, securityToken);
verifyMirroredReadStats({distinct: kCollName, key: 'i'}, otherSecurityToken);

verifyMirroredReadStats({findAndModify: kCollName, query: {x: 1}, update: {$inc: {x: 10}}},
                        securityToken);
verifyMirroredReadStats({findAndModify: kCollName, query: {i: 1}, update: {$inc: {i: 10}}},
                        otherSecurityToken);

verifyMirroredReadStats(
    {update: kCollName, updates: [{q: {x: 1}, u: {'inc': {x: 1}}}], ordered: false}, securityToken);
verifyMirroredReadStats(
    {update: kCollName, updates: [{q: {i: 1}, u: {'inc': {i: 1}}}], ordered: false},
    otherSecurityToken);

primary._setSecurityToken(undefined);
rst.stopSet();
