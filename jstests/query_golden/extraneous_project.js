/**
 * Tests that a $project which does not have an overall effect on the query is optimized out of the
 * final plan.
 * @tags: [
 *   # Checks for 'IndexScan' node in explain.
 *   requires_cqf,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For getPlanSkeleton.

db.setLogLevel(4, "query");

const coll = db.cqf_extraneous_project;
coll.drop();
assert.commandWorked(coll.insert([
    {username: "user8", a: 1},
    {username: "user9", a: 1},
    {username: "user8", a: 2},
    {username: "user7", a: 2},
    {username: "user8", a: 3}
]));

function run(pipeline) {
    jsTestLog(`Query: ${tojsononeline(pipeline)}`);
    show(coll.aggregate(pipeline));
    const explain = coll.explain("executionStats").aggregate(pipeline);
    print(`nReturned: ${explain.executionStats.nReturned}\n`);
    print(`Plan skeleton: `);
    printjson(getPlanSkeleton(explain));
}

run([
    {$match: {username: "/^user8/"}},
    {$project: {username: 1}},
    {$group: {_id: 1, count: {$sum: 1}}}
]);

run([{$match: {username: "/^user8/"}}, {$group: {_id: 1, count: {$sum: 1}}}]);
})();
