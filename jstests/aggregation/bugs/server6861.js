// Providing the wrong number of fields in a pipeline stage specification triggers a parsing error.
// SERVER-6861
load('jstests/aggregation/extras/utils.js');

t = db.jstests_server6861;
t.drop();

t.save({a: 1});

function assertCode(code, expression) {
    assertErrorCode(t, expression, code);
}

function assertResult(result, expression) {
    assert.eq(result, t.aggregate(expression).toArray());
}

// Correct number of fields.
assertResult([{a: 1}], {$project: {_id: 0, a: 1}});

// Incorrect number of fields.
assertCode(16435, {});
assertCode(16435, {$project: {_id: 0, a: 1}, $group: {_id: 0}});
assertCode(16435, {$project: {_id: 0, a: 1}, $group: {_id: 0}, $sort: {a: 1}});

// Invalid stage specification.
assertCode(16436, {$noSuchStage: {a: 1}});
