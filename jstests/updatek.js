// Test modifier operations on numerically equivalent string field names.  SERVER-4776

t = db.jstests_updatek;

t.drop();
t.save( { _id:0, '1':{}, '01':{} } );
t.update( {}, { $set:{ '1.b':1, '1.c':2 } } );
// make sure correct fields are set, with no duplicates
// must use string comparison because V8 reorders fields that are numerical in objects
assert.eq( "{ \"01\" : { }, \"1\" : { \"b\" : 1, \"c\" : 2 }, \"_id\" : 0 }", tojson(t.findOne()) );

t.drop();
t.save( { _id:0, '1':{}, '01':{} } );
t.update( {}, { $set:{ '1.b':1, '01.c':2 } } );
assert.eq( "{ \"01\" : { \"c\" : 2 }, \"1\" : { \"b\" : 1 }, \"_id\" : 0 }", tojson(t.findOne()) );

