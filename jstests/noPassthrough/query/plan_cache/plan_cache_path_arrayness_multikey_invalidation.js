/**
 * Tests that multikeyness metadata changes clear the plan cache.
 */
import {getCachedPlanForQuery, getQueryPlanner} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPathArrayness: true,
        internalQueryPlannerUseMultiplannerForSingleSolutions: true,
    },
});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB(jsTestName());
const query = {"b.c": 1};

const coll = db.coll;
coll.drop();
assert.commandWorked(coll.createIndex({"b.c": 1}));
assert.commandWorked(
    coll.insertMany([
        {_id: 1, b: {c: 1}},
        {_id: 2, b: {c: 2}},
        {_id: 3, b: {c: 3}},
    ]),
);

coll.find(query).toArray();
coll.find(query).toArray();
const entry = getCachedPlanForQuery(db, coll, query);
assert.eq(true, entry.isActive, entry);

assert.commandWorked(coll.insert({_id: 4, b: [{c: 1}]}));
const explain = coll.explain().find(query).finish();
assert(!getQueryPlanner(explain).winningPlan.isCached, {explain});

MongoRunner.stopMongod(conn);
