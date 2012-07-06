// SERVER-6232: seg fault when providing an empty object as an expression argument in agg
// clear and populated db
db.s6232.drop();
db.s6232.save({});

// case where an empty object is evaluated
result = db.s6232.aggregate({$project: {a: {$and: [{}]}}});
assert.eq(result.result[0].a, true);

// case where result should contain a new empty object
result = db.s6232.aggregate({$project: {a: {$ifNull: ['$b', {}]}}});
assert.eq(result.result[0].a, {});
