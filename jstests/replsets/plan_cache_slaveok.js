// Verify that the plan cache and index filter commands can be run on secondaries, but only
// if secondaryOk is explicitly set.

var name = "plan_cache_secondaryok";

function assertPlanCacheCommandsSucceed(db) {
    assert.commandWorked(db.runCommand({planCacheClear: name, query: {a: 1}}));

    // Using aggregate to list the contents of the plan cache.
    assert.commandWorked(
        db.runCommand({aggregate: name, pipeline: [{$planCacheStats: {}}], cursor: {}}));

    assert.commandWorked(
        db.runCommand({planCacheSetFilter: name, query: {a: 1}, indexes: [{a: 1}]}));

    assert.commandWorked(db.runCommand({planCacheListFilters: name}));

    assert.commandWorked(db.runCommand({planCacheClearFilters: name, query: {a: 1}}));
}

function assertPlanCacheCommandsFail(db) {
    assert.commandFailed(db.runCommand({planCacheClear: name, query: {a: 1}}));

    // Using aggregate to list the contents of the plan cache.
    assert.commandFailed(
        db.runCommand({aggregate: name, pipeline: [{$planCacheStats: {}}], cursor: {}}));

    assert.commandFailed(
        db.runCommand({planCacheSetFilter: name, query: {a: 1}, indexes: [{a: 1}]}));

    assert.commandFailed(db.runCommand({planCacheListFilters: name}));

    assert.commandFailed(db.runCommand({planCacheClearFilters: name, query: {a: 1}}));
}

print("Start replica set with two nodes");
var replTest = new ReplSetTest({name: name, nodes: 2});
var nodes = replTest.startSet();
replTest.initiate();
var primary = replTest.getPrimary();

// Insert a document and let it sync to the secondary.
print("Initial sync");
primary.getDB("test")[name].insert({a: 1});
replTest.awaitReplication();

// Check that the document is present on the primary.
assert.eq(1, primary.getDB("test")[name].findOne({a: 1})["a"]);

// Make sure the plan cache commands succeed on the primary.
assertPlanCacheCommandsSucceed(primary.getDB("test"));

// With secondaryOk false, the commands should fail on the secondary.
var secondary = replTest.getSecondary();
secondary.getDB("test").getMongo().setSecondaryOk(false);
assertPlanCacheCommandsFail(secondary.getDB("test"));

// With secondaryOk true, the commands should succeed on the secondary.
secondary.getDB("test").getMongo().setSecondaryOk();
assertPlanCacheCommandsSucceed(secondary.getDB("test"));

replTest.stopSet();
