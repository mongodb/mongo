// SERVER-6177: better error when projecting into a subfield with an existing expression

// load the test utilities
load('jstests/aggregation/extras/utils.js');

var c = db.c;
c.drop();

c.save({});

assertErrorCode(c, {$project: {'x': {$add: [1]}, 'x.b': 1}}, 40176);
assertErrorCode(c, {$project: {'x.b': 1, 'x': {$add: [1]}}}, 40176);
assertErrorCode(c, {$project: {'x': {'b': 1}, 'x.b': 1}}, 40176);
assertErrorCode(c, {$project: {'x.b': 1, 'x': {'b': 1}}}, 40176);
