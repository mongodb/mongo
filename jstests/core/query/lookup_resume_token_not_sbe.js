/**
 * Test that an aggregate which requests a resume token always executes in the classic engine, even
 * when it contains a stage such as $lookup that is otherwise eligible for SBE pushdown.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   requires_fcv_80,
 * ]
 */

import {getEngine} from "jstests/libs/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";

const localColl = db.lookup_resume_token_not_sbe_local;
const foreignColl = db.lookup_resume_token_not_sbe_foreign;

localColl.drop();
foreignColl.drop();

assert.commandWorked(localColl.insert([
    {_id: 1, x: 1, b: 1},
    {_id: 2, x: 20, b: 2},
    {_id: 3, x: 5, b: 1},
]));
assert.commandWorked(foreignColl.insert([
    {_id: 11, k: 1, v: "a"},
    {_id: 12, k: 2, v: "b"},
]));

const pipeline = [{
    $lookup:
        {from: foreignColl.getName(), localField: "b", foreignField: "k", as: "lk"},
}];

if (checkSbeRestrictedOrFullyEnabled(db)) {
    const baseline = assert.commandWorked(db.runCommand({
        explain:
            {aggregate: localColl.getName(), pipeline: pipeline, cursor: {}, hint: {$natural: 1}},
    }));
    assert.eq(getEngine(baseline), "sbe", baseline);
}

const explain = assert.commandWorked(db.runCommand({
    explain: {
        aggregate: localColl.getName(),
        pipeline: pipeline,
        cursor: {},
        $_requestResumeToken: true,
        hint: {$natural: 1},
    },
}));
assert.eq(getEngine(explain), "classic", explain);

const res = assert.commandWorked(db.runCommand({
    aggregate: localColl.getName(),
    pipeline: pipeline,
    cursor: {},
    $_requestResumeToken: true,
    hint: {$natural: 1},
}));
assert.eq(3, res.cursor.firstBatch.length, res);
for (const doc of res.cursor.firstBatch) {
    assert(Array.isArray(doc.lk), {doc});
}
assert.hasFields(res.cursor, ["postBatchResumeToken"]);
