// Test the dropping indexes required by an indexed $or causes the query to fail cleanly.
//
// @tags: [
//   requires_getmore,
//   # An index drop does not necessarily cause cursors to be killed on the secondary.
//   does_not_support_causal_consistency,
// ]
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

const coll = db.jstests_ord;
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

for (let i = 0; i < 80; ++i) {
    assert.commandWorked(coll.insert({a: 1}));
}

for (let i = 0; i < 100; ++i) {
    assert.commandWorked(coll.insert({b: 1}));
}

for (let i = 0; i < 1000; ++i) {
    assert.commandWorked(coll.insert({c: 1}));
}

// The test scenario relies on the optimizer choosing an index for the query. We can do a
// preliminary check here, using explain. However, this does not guarantee the query below will use
// an ixscan, because the chosen plan may not be the same.
const explainRes = assert.commandWorked(coll.find({$or: [{a: 1}, {b: 1}]}).explain());
const orStages = getPlanStages(getWinningPlanFromExplain(explainRes), "OR");
assert(orStages.length > 0, "Expected to find OR stage in explain: " + tojson(explainRes));
assert(orStages.every(orStage => (getPlanStages(orStage, "IXSCAN").length > 0)),
       "Expected the plan to be an OR which has (at least) one IXSCAN: " + tojson(explainRes));

// This query should match 180 out of 1180 total documents.
const cursor = coll.find({$or: [{a: 1}, {b: 1}]}).batchSize(100);
for (let i = 0; i < 100; ++i) {
    cursor.next();
}

// At this point, our initial query has ended and there is a cursor waiting to read additional
// documents from index {b:1}. In between getMores, drop the indexes depended on by the query. The
// query should fail when the next getMore executes.
assert.commandWorked(coll.dropIndex({a: 1}));
assert.commandWorked(coll.dropIndex({b: 1}));

if (FixtureHelpers.isMongos(db)) {
    // mongos may have some data left from a previous batch stored in memory, so it might not
    // return an error immediately, but it should eventually.
    assert.soon(function() {
        try {
            cursor.next();
            return false;  // We didn't throw an error yet.
        } catch (e) {
            return true;
        }
    });
} else {
    const error = assert.throws(() => cursor.next());
    assert.commandFailedWithCode(error, ErrorCodes.QueryPlanKilled);
}
