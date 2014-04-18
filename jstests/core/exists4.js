// Check various exists cases, based on SERVER-1735 example.

t = db.jstests_exists4;
t.drop();

t.ensureIndex({date: -1, country_code: 1, user_id: 1}, {unique: 1, background: 1}); 
t.insert({ date: new Date("08/27/2010"), tot_visit: 100}); 
t.insert({ date: new Date("08/27/2010"), country_code: "IT", tot_visit: 77}); 
t.insert({ date: new Date("08/27/2010"), country_code: "ES", tot_visit: 23}); 
t.insert({ date: new Date("08/27/2010"), country_code: "ES", user_id: "and...@spacca.org", tot_visit: 11}); 
t.insert({ date: new Date("08/27/2010"), country_code: "ES", user_id: "andrea.spa...@gmail.com", tot_visit: 5}); 
t.insert({ date: new Date("08/27/2010"), country_code: "ES", user_id: "andrea.spa...@progloedizioni.com", tot_visit: 7}); 

assert.eq( 6, t.find({date: new Date("08/27/2010")}).count() );
assert.eq( 5, t.find({date: new Date("08/27/2010"), country_code: {$exists: true}}).count() );
assert.eq( 1, t.find({date: new Date("08/27/2010"), country_code: {$exists: false}}).count() );
assert.eq( 1, t.find({date: new Date("08/27/2010"), country_code: null}).count() );
assert.eq( 3, t.find({date: new Date("08/27/2010"), country_code: {$exists: true}, user_id: {$exists: true}}).count() );
assert.eq( 2, t.find({date: new Date("08/27/2010"), country_code: {$exists: true}, user_id: {$exists: false}}).count() );
assert.eq( 2, t.find({date: new Date("08/27/2010"), country_code: {$exists: true}, user_id: null}).count() );
