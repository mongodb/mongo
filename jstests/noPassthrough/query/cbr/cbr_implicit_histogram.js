/*
 * Test the implict_histograms.js override script correctly intercepts CRUD commands and generates
 * histograms for indexed fields.
 */

import {runCommandOverride} from "jstests/libs/override_methods/implicit_histograms.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const conn = MongoRunner.runMongod({setParameter: {planRankerMode: "histogramCE"}});

const db = conn.getDB("test");
const collName = jsTestName();
const coll = db[collName];
const statsColl = db.system.statistics[collName];

assert.commandWorked(coll.createIndexes([
    {a: 1},
    {a: 1, b: 1},
    {'c.d': 1},
    {e: 'hashed'},
]));

assert.commandWorked(coll.insert({a: 1}));

const testCases = [
    {command: {find: collName, filter: {a: 1}}, analyzeIsRun: true},
    {command: {aggregate: collName, pipeline: [{$match: {a: 1}}], cursor: {}}, analyzeIsRun: true},
    {command: {explain: {find: collName, filter: {a: 1}}}, analyzeIsRun: true},
    {command: {count: collName, query: {a: 1}}, analyzeIsRun: true},
    {command: {distinct: collName, key: "a"}, analyzeIsRun: true},
    {command: {delete: collName, deletes: [{q: {a: 2}, limit: 1}]}, analyzeIsRun: true},
    {command: {findAndModify: collName, query: {a: 2}, remove: true}, analyzeIsRun: true},
    {command: {update: collName, updates: [{q: {a: 2}, u: {a: 3}}]}, analyzeIsRun: true},
    {command: {insert: collName, documents: [{a: 2}]}, analyzeIsRun: false},
    {command: {planCacheListFilters: collName}, analyzeIsRun: false},
];

// Install implict histogram hook before running test cases.
OverrideHelpers.overrideRunCommand(runCommandOverride);

for (const tc of testCases) {
    statsColl.drop();
    assert.commandWorked(coll.runCommand(tc.command));
    const res = statsColl.find({}, {_id: 1}).sort({_id: 1}).toArray();
    const paths = res.map(e => e._id);
    if (tc.analyzeIsRun) {
        assert.eq(["_id", "a", "b", "c.d", "e"], paths);
    } else {
        assert.eq([], paths);
    }
}

MongoRunner.stopMongod(conn);
