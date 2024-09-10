/**
 * Test FCV upgrade/downgrade for featureFlagQueryStatsCountDistinct.
 */

import "jstests/multiVersion/libs/multi_rs.js";

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getLatestQueryStatsEntry, getQueryStats} from "jstests/libs/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Start up a replica set with the last-lts binary version.
const nodeOption = {
    binVersion: 'last-lts'
};
// Need at least 2 nodes because upgradeSet method needs to be able call step down
// with another primary-eligible node available.
const replSet = new ReplSetTest({nodes: [nodeOption, nodeOption]});
replSet.startSet();
replSet.initiate();

// Upgrade the set and enable the feature flag. The feature flag will be enabled as of
// the latest FCV. However, the repl set will still have FCV last-lts.
replSet.upgradeSet({binVersion: 'latest', setParameter: {internalQueryStatsRateLimit: -1}});

const primary = replSet.getPrimary();
const db = primary.getDB("test");
const collName = jsTestName();
const coll = db[collName];
coll.insert({a: 1});

// The feature flag should not be enabled yet.
assert(!FeatureFlagUtil.isEnabled(db, "QueryStatsCountDistinct"));

// Verify that query stats are not collected for count and distinct commands.
coll.count({a: 5});
coll.distinct("a");

let queryStats = getQueryStats(db, {collName});
assert.eq(queryStats.length, 0, queryStats);

// Upgrade FCV and confirm the feature flag is now enabled.
assert.commandWorked(
    primary.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
assert(FeatureFlagUtil.isPresentAndEnabled(db, "QueryStatsCountDistinct"));

// Now verify that query stats are collected for count and distinct commands.
coll.count({a: 5});

queryStats = getLatestQueryStatsEntry(db, {collName});
assert.eq(queryStats.key.queryShape.command, "count", queryStats);

coll.distinct("a");

queryStats = getLatestQueryStatsEntry(db, {collName});
assert.eq(queryStats.key.queryShape.command, "distinct", queryStats);

// Now test query stats during a failed FCV downgrade. During the failed FCV downgrade, the FCV
// reverts back to the lastLTSFCV which doesn't support queryStats.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));

assert.commandFailedWithCode(
    primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}), 549181);
assert(!FeatureFlagUtil.isEnabled(db, "QueryStatsCountDistinct"));

// Query stats should not be collected for count and distinct.
coll.count({b: 5});
coll.distinct("b");

queryStats = getQueryStats(db, {collName});
assert.eq(queryStats.length, 2, queryStats);

// Finally, test a successful downgrade, and confirm the feature flag is disabled.
assert.commandWorked(primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "off"}));

assert.commandWorked(
    primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
assert(!FeatureFlagUtil.isEnabled(db, "QueryStatsCountDistinct"));

// Query stats should still not be collected for count and distinct.
coll.count({b: 5});
coll.distinct("b");

queryStats = getQueryStats(db, {collName});
assert.eq(queryStats.length, 2, queryStats);

replSet.stopSet();
