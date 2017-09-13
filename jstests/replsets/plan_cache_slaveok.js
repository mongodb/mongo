// Verify that the plan cache and index filter commands can be run on secondaries, but only
// if slave ok is explicitly set.

var name = "plan_cache_slaveok";

function assertPlanCacheCommandsSucceed(db) {
    // .listQueryShapes()
    assert.commandWorked(db.runCommand({planCacheListQueryShapes: name}));

    // .getPlansByQuery()
    assert.commandWorked(db.runCommand({planCacheListPlans: name, query: {a: 1}}));

    // .clear()
    assert.commandWorked(db.runCommand({planCacheClear: name, query: {a: 1}}));

    // setFilter
    assert.commandWorked(
        db.runCommand({planCacheSetFilter: name, query: {a: 1}, indexes: [{a: 1}]}));

    // listFilters
    assert.commandWorked(db.runCommand({planCacheListFilters: name}));

    // clearFilters
    assert.commandWorked(db.runCommand({planCacheClearFilters: name, query: {a: 1}}));
}

function assertPlanCacheCommandsFail(db) {
    // .listQueryShapes()
    assert.commandFailed(db.runCommand({planCacheListQueryShapes: name}));

    // .getPlansByQuery()
    assert.commandFailed(db.runCommand({planCacheListPlans: name, query: {a: 1}}));

    // .clear()
    assert.commandFailed(db.runCommand({planCacheClear: name, query: {a: 1}}));

    // setFilter
    assert.commandFailed(
        db.runCommand({planCacheSetFilter: name, query: {a: 1}, indexes: [{a: 1}]}));

    // listFilters
    assert.commandFailed(db.runCommand({planCacheListFilters: name}));

    // clearFilters
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

// With slave ok false, the commands should fail on the secondary.
var secondary = replTest.getSecondary();
secondary.getDB("test").getMongo().setSlaveOk(false);
assertPlanCacheCommandsFail(secondary.getDB("test"));

// With slave ok true, the commands should succeed on the secondary.
secondary.getDB("test").getMongo().setSlaveOk(true);
assertPlanCacheCommandsSucceed(secondary.getDB("test"));
