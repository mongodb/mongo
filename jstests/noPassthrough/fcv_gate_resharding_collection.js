/**
 * Test that `system.resharding.*` namespaces can only be created with `create`Collection in
 * FCV 4.9+.
 *
 * TODO SERVER-55470: This test should be removed when 5.0 becomes the lastLTS.
 *
 * @tags: [multiversion_incompatible, requires_replication]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "4.4"}));
assert.commandFailedWithCode(primary.getDB("foo").createCollection("system.resharding.123"),
                             ErrorCodes.InvalidNamespace);
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(primary.getDB("foo").createCollection("system.resharding.123"));
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "4.4"}));
assert.commandWorked(primary.getDB("foo").runCommand({"drop": "system.resharding.123"}));

rst.stopSet();
})();
