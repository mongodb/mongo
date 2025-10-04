/**
 * Tests that query stats key hashes are consistent across versions.
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {
    getQueryStatsFindCmd,
    getQueryStatsKeyHashes,
    getQueryStatsServerParameters,
} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: {n1: {binVersion: "last-lts"}, n2: {binVersion: "last-lts"}, n3: {binVersion: "last-lts"}},
});

// Turn on the collecting of query stats metrics.
rst.startSet(getQueryStatsServerParameters());
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

let conn = rst.getPrimary();
const collName = jsTestName();
let coll = conn.getDB("test")[collName];
coll.drop();
coll.insert({x: 5});

// Run a few unique queries so that they generate different query stats entries.
function runQueries() {
    coll.find({x: 5}).toArray();
    coll.find({y: 5}).toArray();
    coll.find({y: 5}).sort({x: -1}).toArray();
    coll.find({y: 5}).maxTimeMS(123).toArray();
}

// First, collect and save query stats entries on the older version.
runQueries();
const preUpgradeEntries = getQueryStatsFindCmd(conn, {collName, transformIdentifiers: false});
assert.eq(preUpgradeEntries.length, 4, tojson(preUpgradeEntries));
const preUpgradeKeyHashes = getQueryStatsKeyHashes(preUpgradeEntries);

// Upgrade to the latest.
rst.upgradeSet({binVersion: "latest"});
conn = rst.getPrimary();
coll = conn.getDB("test")[collName];

// Run the same queries again and check that we got the same query stats key hash values as we did
// on the old version.
runQueries();
const postUpgradeEntries = getQueryStatsFindCmd(conn, {collName, transformIdentifiers: false});
assert.eq(postUpgradeEntries.length, 4, tojson(postUpgradeEntries));
const postUpgradeKeyHashes = getQueryStatsKeyHashes(postUpgradeEntries);
assert.sameMembers(
    postUpgradeKeyHashes,
    preUpgradeKeyHashes,
    `preUpgradeEntries = ${tojson(preUpgradeEntries)}, postUpgradeEntries = ${tojson(postUpgradeEntries)}`,
);

rst.stopSet();
