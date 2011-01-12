
t = db.type2;
t.drop();

t.ensureIndex( { date: -1, country_code: 1, user_id: 1 }, { unique: 1, background : 1} );
t.insert( { date: new Date("08/27/2010"), tot_visit: 100} );
t.insert( { date: new Date("08/27/2010"), country_code: "IT", tot_visit: 77} );
t.insert( { date: new Date("08/27/2010"), country_code: "ES", tot_visit: 23} );
t.insert( { date: new Date("08/27/2010"), country_code: "ES", user_id: "a", tot_visit: 11} );
t.insert( { date: new Date("08/27/2010"), country_code: "ES", user_id: "b", tot_visit: 5} );
t.insert( { date: new Date("08/27/2010"), country_code: "ES", user_id: "c", tot_visit: 7} );

assert.eq(6, t.find({date: new Date("08/27/2010")}).count(), "A");

assert.eq(5, t.find({date: new Date("08/27/2010"), country_code: {$exists :true}}).count(), "B1");
assert.eq(1, t.find({date: new Date("08/27/2010"), country_code: {$exists : false}}).count(), "B2");
assert.eq(1, t.find({date: new Date("08/27/2010"), country_code: {$type : 10}}).count(), "B3");
assert.eq(1, t.find({date: new Date("08/27/2010"), country_code: null}).count(), "B4");

assert.eq(3, t.find({date: new Date("08/27/2010"), country_code: {$exists: true}, user_id: {$exists: true}}).count(), "C1");
assert.eq(2, t.find({date: new Date("08/27/2010"), country_code: {$exists: true}, user_id: {$exists: false}}).count(), "C2");
assert.eq(2, t.find({date: new Date("08/27/2010"), country_code: {$exists: true}, user_id: {$type: 10}}).count(), "C3");
assert.eq(2, t.find({date: new Date("08/27/2010"), country_code: {$exists: true}, user_id: null}).count(), "C4");
