/**
 * Tests the "requireApiVersion" mongod/mongos parameter.
 *
 * @tags: [
 *   assumes_superuser_permissions,
 *   requires_fcv_47,
 *   # Cannot change primary or read from secondary after setting primary parameter.
 *   assumes_read_preference_unchanged,
 *   does_not_support_stepdowns,
 * ]
 */

// Background commands will fail while requireApiVersion is true.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckDBHashes = true;
TestData.skipCheckOrphans = true;

(function() {
"use strict";

assert.commandWorked(db.adminCommand({setParameter: 1, requireApiVersion: true}));
assert.commandFailedWithCode(db.adminCommand({ping: 1}), 498870, "command without apiVersion");
assert.commandWorked(db.adminCommand({ping: 1, apiVersion: "1"}));
assert.commandFailed(db.adminCommand({ping: 1, apiVersion: "not a real API version"}));
assert.commandWorked(db.adminCommand({setParameter: 1, requireApiVersion: false, apiVersion: "1"}));
assert.commandWorked(db.adminCommand({ping: 1}));
}());
