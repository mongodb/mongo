// Tests that a query with $in filter over a large array chooses the optimal index.
import {code, codeOneLine, section, subSection} from "jstests/libs/pretty_md.js";
import {normalizePlan, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

const coll = db.large_in_with_indexes;
coll.drop();

Random.setRandomSeed(1);

const docs = [];
const numDocs = 100000;
for (let i = 0; i < numDocs; i++) {
    docs.push({_id: i, rd: Random.randInt(numDocs), ard: Random.randInt(numDocs)});
}

assert.commandWorked(coll.createIndex({rd: 1}));
assert.commandWorked(coll.createIndex({ard: 1}));
assert.commandWorked(coll.createIndex({rd: 1, ard: 1}));
assert.commandWorked(coll.createIndex({_id: 1, rd: 1, ard: 1}));
assert.commandWorked(coll.insertMany(docs));

section("Large Indexed $in");

const inArray = [];
for (let i = 0; i < 300; i++) {
    inArray.push(Random.randInt(numDocs));
}

const filter = {rd: {$gte: 7}, _id: {$in: inArray}};
const sort = {ard: 1};
codeOneLine({filter, sort});
const explain = coll.find(filter).sort(sort).explain("executionStats");

subSection("Expected plan");
print("Note: expecting an IXSCAN on _id_1_rd_1_ard_1");
code(tojson(normalizePlan(getWinningPlanFromExplain(explain.queryPlanner))));

subSection("Output");
const output = coll.find(filter).sort(sort).toArray();
codeOneLine(output);
