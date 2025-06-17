/**
 * Tests that serverStatus contains a catalogStats section.
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
const replSet = new ReplSetTest({nodes: 2});
replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();
let db1 = primary.getDB('db1');
let db2 = primary.getDB('db2');

const assertCatalogStats = (db, assertFn) => {
    assertFn(db.serverStatus().catalogStats);
};

let internalCollectionsAtStart;
let internalViewsAtStart;
assertCatalogStats(db1, (stats) => {
    assert.eq(0, stats.capped);
    assert.eq(0, stats.clustered);
    assert.eq(0, stats.collections);
    assert.eq(0, stats.systemProfile);
    assert.eq(0, stats.timeseries);
    assert.eq(0, stats.views);
    internalCollectionsAtStart = stats.internalCollections;
    internalViewsAtStart = stats.internalViews;
});

assert.commandWorked(db1.coll.insert({a: 1}));
assert.commandWorked(db1.createCollection('capped', {capped: true, size: 1024}));
assert.commandWorked(
    db1.createCollection('clustered', {clusteredIndex: {unique: true, key: {_id: 1}}}));
assert.commandWorked(db1.createCollection('view', {viewOn: 'coll', pipeline: []}));
assert.commandWorked(db1.createCollection('ts', {timeseries: {timeField: 't'}}));

// A system.views and system.buckets collection should have been created.
let internalCollectionsCreated = 2;

// Create the profile collection.
assert.commandWorked(db1.setProfilingLevel(2, 0));
assert.eq(1, db1.coll.find({}).itcount());
internalCollectionsCreated += 1;

// Turn off profiler to avoid creating extra collections.
assert.commandWorked(db1.setProfilingLevel(0, 100));

assertCatalogStats(db1, (stats) => {
    assert.eq(1, stats.capped);
    assert.eq(1, stats.clustered);
    assert.eq(3, stats.collections);
    assert.eq(1, stats.systemProfile);
    assert.eq(1, stats.timeseries);
    assert.eq(1, stats.views);
    assert.eq(internalCollectionsAtStart + internalCollectionsCreated, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

// Ensure the stats stay accurate in the view catalog with a collMod.
assert.commandWorked(db1.runCommand({collMod: 'view', pipeline: [{$match: {a: 1}}]}));
assertCatalogStats(db1, (stats) => {
    assert.eq(1, stats.capped);
    assert.eq(1, stats.clustered);
    assert.eq(3, stats.collections);
    assert.eq(1, stats.systemProfile);
    assert.eq(1, stats.timeseries);
    assert.eq(1, stats.views);
    // An system.views and system.buckets collection should have been created.
    assert.eq(internalCollectionsAtStart + internalCollectionsCreated, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

assert.commandWorked(db2.coll.insert({a: 1}));
assert.commandWorked(db2.createCollection('capped', {capped: true, size: 1024}));
assert.commandWorked(
    db2.createCollection('clustered', {clusteredIndex: {unique: true, key: {_id: 1}}}));
assert.commandWorked(db2.createCollection('view', {viewOn: 'coll', pipeline: []}));
assert.commandWorked(db2.createCollection('ts', {timeseries: {timeField: 't'}}));

// An system.views and system.buckets collection should have been created.
internalCollectionsCreated += 2;

assertCatalogStats(db1, (stats) => {
    assert.eq(2, stats.capped);
    assert.eq(2, stats.clustered);
    assert.eq(6, stats.collections);
    assert.eq(1, stats.systemProfile);
    assert.eq(2, stats.timeseries);
    assert.eq(2, stats.views);
    assert.eq(internalCollectionsAtStart + internalCollectionsCreated, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

replSet.stopSet(undefined, /* restart */ true);
replSet.startSet({}, /* restart */ true);
primary = replSet.getPrimary();
db1 = primary.getDB('db1');
db2 = primary.getDB('db2');

// Ensure stats are the same after restart.
assertCatalogStats(db1, (stats) => {
    assert.eq(2, stats.capped);
    assert.eq(2, stats.clustered);
    assert.eq(6, stats.collections);
    assert.eq(1, stats.systemProfile);
    assert.eq(2, stats.timeseries);
    assert.eq(2, stats.views);
    assert.eq(internalCollectionsAtStart + internalCollectionsCreated, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

assert(db1.coll.drop());
assert(db1.capped.drop());
assert(db1.clustered.drop());
assert(db1.view.drop());
assert(db1.ts.drop());

// The system.buckets collection will be dropped, but not system.views.
internalCollectionsCreated -= 1;

assertCatalogStats(db1, (stats) => {
    assert.eq(1, stats.capped);
    assert.eq(1, stats.clustered);
    assert.eq(3, stats.collections);
    assert.eq(1, stats.systemProfile);
    assert.eq(1, stats.timeseries);
    assert.eq(1, stats.views);
    assert.eq(internalCollectionsAtStart + internalCollectionsCreated, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

db1.dropDatabase();

// The system.views and system.profile collections should be dropped.
internalCollectionsCreated -= 2;

assertCatalogStats(db1, (stats) => {
    assert.eq(1, stats.capped);
    assert.eq(3, stats.collections);
    // The system.profile collection should be dropped.
    assert.eq(0, stats.systemProfile);
    assert.eq(1, stats.timeseries);
    assert.eq(1, stats.views);
    assert.eq(internalCollectionsAtStart + internalCollectionsCreated, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

db2.dropDatabase();

// The system.views and system.buckets collections should be dropped.
internalCollectionsCreated -= 2;

assertCatalogStats(db1, (stats) => {
    assert.eq(0, stats.capped);
    assert.eq(0, stats.clustered);
    assert.eq(0, stats.collections);
    assert.eq(0, stats.systemProfile);
    assert.eq(0, stats.timeseries);
    assert.eq(0, stats.views);
    assert.eq(internalCollectionsAtStart + internalCollectionsCreated, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

replSet.stopSet();
