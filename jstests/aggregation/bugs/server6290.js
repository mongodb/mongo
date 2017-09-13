// The $isoDate operator is not available.  SERVER-6290

// load the test utilities
load('jstests/aggregation/extras/utils.js');

var t = db.jstests_aggregation_server6290;
t.drop();

t.save({});

var error = ErrorCodes.InvalidPipelineOperator;

// $isoDate is an invalid operator.
assertErrorCode(t, {$project: {a: {$isoDate: [{year: 1}]}}}, error);
// $date is an invalid operator.
assertErrorCode(t, {$project: {a: {$date: [{year: 1}]}}}, error);

// Alternative operands.
assertErrorCode(t, {$project: {a: {$isoDate: []}}}, error);
assertErrorCode(t, {$project: {a: {$date: []}}}, error);
assertErrorCode(t, {$project: {a: {$isoDate: 'foo'}}}, error);
assertErrorCode(t, {$project: {a: {$date: 'foo'}}}, error);

// Test with $group.
assertErrorCode(t, {$group: {_id: 0, a: {$first: {$isoDate: [{year: 1}]}}}}, error);
assertErrorCode(t, {$group: {_id: 0, a: {$first: {$date: [{year: 1}]}}}}, error);
