// server-6335: don't allow $where clauses in a $match

// load the test utilities
load('jstests/aggregation/extras/utils.js');

assertErrorCode(db.foo, {$match: {$where: "return true"}}, ErrorCodes.BadValue);
assertErrorCode(db.foo, {$match: {$and: [{$where: "return true"}]}}, ErrorCodes.BadValue);
