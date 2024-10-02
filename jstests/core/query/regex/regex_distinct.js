/**
 * Verify the usage of DISTINCT_SCAN when a regex ending with .* is a prefix of an index.
 *
 * @tags: [
 *  requires_fcv_53,
 *  assumes_read_concern_local,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getPlanStages} from "jstests/libs/query/analyze_plan.js";

const coll = db.regex_distinct;
coll.drop();

assert.commandWorked(coll.insertMany(
    [{a: "abc", b: "foo"}, {a: "abc", b: "bar"}, {a: "abd", b: "far"}, {a: "aeb", b: "car"}]));

assert.commandWorked(coll.createIndex({a: 1}));

const results = coll.distinct("a", {a: {"$regex": "^ab.*"}});
results.sort();

const formatResultsFn = () => tojson(results);

if (FixtureHelpers.isMongos(db)) {
    // TODO: SERVER-13116 DISTINCT_SCAN will return orphaned documents. We assert that each
    // result returned has a matching value, but we do not enforce how many are returned (we allow
    // duplicates).
    assert.lte(2, results.length, formatResultsFn);
    for (let res of results) {
        assert(res == "abc" || res == "abd", formatResultsFn);
    }
} else {
    assert.eq(2, results.length, formatResultsFn);
    assert.eq(results[0], "abc", formatResultsFn);
    assert.eq(results[1], "abd", formatResultsFn);
}
const distinctScanStages =
    getPlanStages(coll.explain().distinct("a", {a: {"$regex": "^ab.*"}}), "DISTINCT_SCAN");

assert.eq(distinctScanStages.length, FixtureHelpers.numberOfShardsForCollection(coll));
