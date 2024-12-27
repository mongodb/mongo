/*
 * Test that query plans involving sort and project are correct.
 * @tags: [
 *   requires_fcv_72,
 *   requires_getmore,
 * ]
 */
(function() {
const coll = db.sort_project_queries;
coll.drop();

assert.commandWorked(coll.insert({_id: 0, a: 1, b: 2, foo: 1001, bar: 2001}));
assert.commandWorked(coll.insert({_id: 1, a: 1, b: 22, foo: 1002, bar: 2002}));

function runOneDocTest(pipeline, expectedResult) {
    let res = coll.aggregate(pipeline).toArray();
    assert.eq(res.length, 1);
    assert.docEq(res[0], expectedResult);
}

// Inclusion projection preserving relevant fields after sort.
{
    let pipe = [
        {$sort: {b: 1}},
        {$project: {a: 1, foo: 1}},
        {$group: {_id: '$a', maxFoo: {$max: "$foo"}}}
    ];
    runOneDocTest(pipe, {_id: 1, maxFoo: 1002});
}

// Inclusion projection removing a relevant field after sort.
{
    let pipe = [{$sort: {b: 1}}, {$project: {a: 1}}, {$group: {_id: '$a', maxFoo: {$max: "$foo"}}}];
    runOneDocTest(pipe, {_id: 1, maxFoo: null});
}

// Exclusion projection removing irrelevant field after sort.
{
    let pipe = [{$sort: {b: 1}}, {$project: {z: 0}}, {$group: {_id: '$a', maxFoo: {$max: "$foo"}}}];
    runOneDocTest(pipe, {_id: 1, maxFoo: 1002});
}

// Exclusion projection removing a subsequently referenced field after sort.
{
    let pipe =
        [{$sort: {b: 1}}, {$project: {foo: 0}}, {$group: {_id: '$a', maxFoo: {$max: "$foo"}}}];
    runOneDocTest(pipe, {_id: 1, maxFoo: null});
}
})();
