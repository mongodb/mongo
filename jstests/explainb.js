// nscanned and nscannedObjects report results for the winning plan; nscannedAllPlans and
// nscannedObjectsAllPlans report results for all plans.  SERVER-6268
//
// This file tests the output of .explain.

t = db.jstests_explainb;
t.drop();

t.ensureIndex( { a:1, b:1 } );
t.ensureIndex( { b:1, a:1 } );

t.save( { a:0, b:1 } );
t.save( { a:1, b:0 } );

explain = t.find( { a:{ $gte:0 }, b:{ $gte:0 } } ).explain( true );

// We don't check explain.cursor because all plans perform the same.
assert.eq( 2, explain.n );
// nscanned and nscannedObjects are reported.
assert.eq( 2, explain.nscanned );
assert.eq( 2, explain.nscannedObjects );

// A limit of 2.
explain = t.find( { a:{ $gte:0 }, b:{ $gte:0 } } ).limit( -2 ).explain( true );
assert.eq( 2, explain.n );

// A $or query.
explain = t.find( { $or:[ { a:{ $gte:0 }, b:{ $gte:1 } },
                          { a:{ $gte:1 }, b:{ $gte:0 } } ] } ).explain( true );
// One result from the first $or clause
assert.eq( 1, explain.clauses[ 0 ].n );
// But 2 total.
assert.eq( 2, explain.n );

// These are computed by summing the values for each clause.
printjson(explain);
assert.eq( 2, explain.n );

// A non $or case where nscanned != number of results
t.remove();
t.save( { a:'0', b:'1' } );
t.save( { a:'1', b:'0' } );
explain = t.find( { a:/0/, b:/1/ } ).explain( true );
assert.eq( 1, explain.n );
assert.eq( 2, explain.nscanned );
