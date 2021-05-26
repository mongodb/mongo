/**
 * Tests that serverStatus contains a catalogStats section.
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

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
    assert.eq(0, stats.collections);
    assert.eq(0, stats.timeseries);
    assert.eq(0, stats.views);
    internalCollectionsAtStart = stats.internalCollections;
    internalViewsAtStart = stats.internalViews;
});

assert.commandWorked(db1.coll.insert({a: 1}));
assert.commandWorked(db1.createCollection('capped', {capped: true, size: 1024}));
assert.commandWorked(db1.createCollection('view', {viewOn: 'coll', pipeline: []}));
assert.commandWorked(db1.createCollection('ts', {timeseries: {timeField: 't'}}));

assertCatalogStats(db1, (stats) => {
    assert.eq(1, stats.capped);
    assert.eq(2, stats.collections);
    assert.eq(1, stats.timeseries);
    assert.eq(1, stats.views);
    // An system.views and system.buckets collection should have been created.
    assert.eq(internalCollectionsAtStart + 2, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

// Ensure the stats stay accurate in the view catalog with a collMod.
assert.commandWorked(db1.runCommand({collMod: 'view', pipeline: [{$match: {a: 1}}]}));
assertCatalogStats(db1, (stats) => {
    assert.eq(1, stats.capped);
    assert.eq(2, stats.collections);
    assert.eq(1, stats.timeseries);
    assert.eq(1, stats.views);
    // An system.views and system.buckets collection should have been created.
    assert.eq(internalCollectionsAtStart + 2, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

assert.commandWorked(db2.coll.insert({a: 1}));
assert.commandWorked(db2.createCollection('capped', {capped: true, size: 1024}));
assert.commandWorked(db2.createCollection('view', {viewOn: 'coll', pipeline: []}));
assert.commandWorked(db2.createCollection('ts', {timeseries: {timeField: 't'}}));

assertCatalogStats(db1, (stats) => {
    assert.eq(2, stats.capped);
    assert.eq(4, stats.collections);
    assert.eq(2, stats.timeseries);
    assert.eq(2, stats.views);
    // An system.views and system.buckets collection should have been created.
    assert.eq(internalCollectionsAtStart + 4, stats.internalCollections);
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
    assert.eq(4, stats.collections);
    assert.eq(2, stats.timeseries);
    assert.eq(2, stats.views);
    assert.eq(internalCollectionsAtStart + 4, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

assert(db1.coll.drop());
assert(db1.capped.drop());
assert(db1.view.drop());
assert(db1.ts.drop());

assertCatalogStats(db1, (stats) => {
    assert.eq(1, stats.capped);
    assert.eq(2, stats.collections);
    assert.eq(1, stats.timeseries);
    assert.eq(1, stats.views);
    // The system.views collection will stick around
    assert.eq(internalCollectionsAtStart + 3, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

db1.dropDatabase();

assertCatalogStats(db1, (stats) => {
    assert.eq(1, stats.capped);
    assert.eq(2, stats.collections);
    assert.eq(1, stats.timeseries);
    assert.eq(1, stats.views);
    // The system.views collection should be dropped
    assert.eq(internalCollectionsAtStart + 2, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

db2.dropDatabase();

assertCatalogStats(db1, (stats) => {
    assert.eq(0, stats.capped);
    assert.eq(0, stats.collections);
    assert.eq(0, stats.timeseries);
    assert.eq(0, stats.views);
    // The system.views collection should be dropped
    assert.eq(internalCollectionsAtStart, stats.internalCollections);
    assert.eq(internalViewsAtStart, stats.internalViews);
});

replSet.stopSet();
})();
