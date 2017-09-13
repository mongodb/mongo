// ensure $concat asserts on string

load('jstests/aggregation/extras/utils.js');

c = db.s6570;
c.drop();
c.save({v: "$", w: ".", x: "foo", y: "bar"});

assert.eq(c.aggregate({$project: {str: {$concat: ["X", "$x", "Y", "$y"]}}}).toArray()[0].str,
          "XfooYbar");
assert.eq(c.aggregate({$project: {str: {$concat: ["$v", "X", "$w", "Y"]}}}).toArray()[0].str,
          "$X.Y");
assert.eq(c.aggregate({$project: {str: {$concat: ["$w", "X", "$v", "Y"]}}}).toArray()[0].str,
          ".X$Y");

// Nullish (both with and without other strings)
assert.isnull(c.aggregate({$project: {str: {$concat: ["$missing"]}}}).toArray()[0].str);
assert.isnull(c.aggregate({$project: {str: {$concat: [null]}}}).toArray()[0].str);
assert.isnull(c.aggregate({$project: {str: {$concat: [undefined]}}}).toArray()[0].str);
assert.isnull(c.aggregate({$project: {str: {$concat: ["$x", "$missing", "$y"]}}}).toArray()[0].str);
assert.isnull(c.aggregate({$project: {str: {$concat: ["$x", null, "$y"]}}}).toArray()[0].str);
assert.isnull(c.aggregate({$project: {str: {$concat: ["$x", undefined, "$y"]}}}).toArray()[0].str);

// assert fail for all other types
assertErrorCode(c, {$project: {str: {$concat: [MinKey]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [1]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [NumberInt(1)]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [NumberLong(1)]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [true]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [function() {}]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [{}]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [[]]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [new Timestamp(0, 0)]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [new Date(0)]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [new BinData(0, "")]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [/asdf/]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [MaxKey]}}}, 16702);
assertErrorCode(c, {$project: {str: {$concat: [new ObjectId()]}}}, 16702);
