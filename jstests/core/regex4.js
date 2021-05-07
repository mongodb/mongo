// @tags: [
//   assumes_balancer_off,
// ]

t = db.regex4;
t.drop();

assert.commandWorked(t.save({name: "eliot"}));
assert.commandWorked(t.save({name: "emily"}));
assert.commandWorked(t.save({name: "bob"}));
assert.commandWorked(t.save({name: "aaron"}));

assert.eq(2, t.find({name: /^e.*/}).count(), "no index count");
assert.eq(
    4, t.find({name: /^e.*/}).explain(true).executionStats.totalDocsExamined, "no index explain");
// assert.eq( 2 , t.find( { name : { $ne : /^e.*/ } } ).count() , "no index count ne" ); //
// SERVER-251

assert.commandWorked(t.createIndex({name: 1}));

assert.eq(2, t.find({name: /^e.*/}).count(), "index count");
assert.eq(2,
          t.find({name: /^e.*/}).explain(true).executionStats.totalKeysExamined,
          "index explain");  // SERVER-239
// assert.eq( 2 , t.find( { name : { $ne : /^e.*/ } } ).count() , "index count ne" ); // SERVER-251
