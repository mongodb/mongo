/**
 * Regression test to ensure we correctly reparse $all.
 *
 * @tags: [
 *   uses_explain,
 *   not_allowed_with_signed_security_token,
 *   does_not_support_stepdowns,
 *   config_shard_incompatible,
 *   incompatible_with_initial_sync,
 * ]
 */

import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {runWithKnobs} from "jstests/libs/query/knob_utils.js";

const coll = db.c;
coll.drop();

assert.commandWorked(
    coll.insertMany([
        {_id: 0, x: [3, 3]},
        {_id: 1, x: [3]},
        {_id: 2, x: [3, 5]},
        {_id: 3, x: [{}, {}]},
    ]),
);

function runTest() {
    {
        const filter = {x: {$elemMatch: {$all: [3, 5]}}};
        const parsedQuery = getQueryPlanner(coll.find(filter).explain()).parsedQuery;
        assert.eq(coll.find(filter).itcount(), 0);
        assert.eq(coll.find(filter).itcount(), coll.find(parsedQuery).itcount(), parsedQuery);
    }
    {
        const filter = {x: {$elemMatch: {$all: [3, 3]}}};
        const parsedQuery = getQueryPlanner(coll.find(filter).explain()).parsedQuery;
        assert.eq(coll.find(filter).itcount(), 3);
        assert.eq(coll.find(filter).itcount(), coll.find(parsedQuery).itcount(), parsedQuery);
    }
    {
        const filter = {x: {$elemMatch: {$all: [3, 3], $gte: 3}}};
        const parsedQuery = getQueryPlanner(coll.find(filter).explain()).parsedQuery;
        assert.eq(coll.find(filter).itcount(), 3);
        assert.eq(coll.find(filter).itcount(), coll.find(parsedQuery).itcount(), parsedQuery);
    }
    {
        const filter = {x: {$elemMatch: {$all: []}}};
        const parsedQuery = getQueryPlanner(coll.find(filter).explain()).parsedQuery;
        assert.eq(coll.find(filter).itcount(), 0);
        assert.eq(coll.find(filter).itcount(), coll.find(parsedQuery).itcount(), parsedQuery);
    }
    {
        const filter = {x: {$elemMatch: {}}};
        const parsedQuery = getQueryPlanner(coll.find(filter).explain()).parsedQuery;
        assert.eq(coll.find(filter).itcount(), 1);
        assert.eq(coll.find(filter).itcount(), coll.find(parsedQuery).itcount(), parsedQuery);
    }
}

// Validate with query stats off (this relies on correct reparsing!).
runWithKnobs(db, runTest, {internalQueryStatsRateLimit: 0, internalQueryStatsSampleRate: 0});
// Now repeat with whatever the default setting is.
runTest();
