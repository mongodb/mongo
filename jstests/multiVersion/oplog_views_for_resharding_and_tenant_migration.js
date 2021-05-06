/**
 * Tests that oplog views for resharding and tenant migration are created when a replica set is
 * upgraded from 4.4.
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");  // For 'upgradeSet()'

const kReshardingOplogViewName = "system.resharding.slimOplogForGraphLookup";
const kTenantMigrationOplogViewName = "system.tenantMigration.oplogView";

function assertOplogViewsNotExist(node) {
    const localDb = node.getDB("local");
    const reshardingViewRes = localDb.runCommand(
        {listCollections: 1, filter: {type: "view", name: kReshardingOplogViewName}});
    assert.eq(0, reshardingViewRes.cursor.firstBatch.length);
    const tenantMigrationRes = localDb.runCommand(
        {listCollections: 1, filter: {type: "view", name: kTenantMigrationOplogViewName}});
    assert.eq(0, tenantMigrationRes.cursor.firstBatch.length);
}

function assertOplogViewsExist(node) {
    const localDb = node.getDB("local");
    const reshardingViewRes = localDb.runCommand(
        {listCollections: 1, filter: {type: "view", name: kReshardingOplogViewName}});
    assert.eq(1, reshardingViewRes.cursor.firstBatch.length);
    const tenantMigrationRes = localDb.runCommand(
        {listCollections: 1, filter: {type: "view", name: kTenantMigrationOplogViewName}});
    assert.eq(1, tenantMigrationRes.cursor.firstBatch.length);
}

// Set up a replica set in v4.4.
const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: "last-lts"}});
rst.startSet();
rst.initiate();
rst.nodes.forEach(node => assertOplogViewsNotExist(node));

// Upgrade the replica set.
rst.upgradeSet({binVersion: "latest"});
rst.nodes.forEach(node => assertOplogViewsExist(node));

rst.stopSet();
})();