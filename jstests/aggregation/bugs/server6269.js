// Correct skipping behavior when $skip is applied after $unwind.  SERVER-6269

c = db.jstests_aggregation_server6269;
c.drop();

c.save( { _id:0, a:[ 1, 2, 3 ] } );
// The unwound a:1 document is skipped, but the remainder are returned.
assert.eq( [ { _id:0, a:2 }, { _id:0, a:3 } ],
           c.aggregate( { $unwind:'$a' }, { $skip:1 } ).result );

// Test with two documents.
c.save( { _id:1, a:[ 4, 5, 6 ] } );
assert.eq( [ { _id:0, a:3 }, { _id:1, a:4 }, { _id:1, a:5 }, { _id:1, a:6 } ],
          c.aggregate( { $unwind:'$a' }, { $skip:2 } ).result );
