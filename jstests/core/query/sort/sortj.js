// Test an in memory sort memory assertion after a plan has "taken over" in the query optimizer
// cursor.
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const t = db.jstests_sortj;
t.drop();

t.createIndex({a: 1});

const numShards = FixtureHelpers.numberOfShardsForCollection(t);

const big = new Array(100000).toString();
for (let i = 0; i < 1200 * numShards; ++i) {
    t.save({a: 1, b: big});
}

assert.throwsWithCode(
    () => t.find({a: {$gte: 0}, c: null}).sort({d: 1}).allowDiskUse(false).itcount(),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
t.drop();